/* 
 * gnome-keyring
 * 
 * Copyright (C) 2009 Stefan Walter
 * 
 * This program is free software; you can redistribute it and/or modify 
 * it under the terms of the GNU Lesser General Public License as
 * published by the Free Software Foundation; either version 2.1 of
 * the License, or (at your option) any later version.
 *  
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *  
 * You should have received a copy of the GNU Lesser General Public
 * License along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA
 * 02111-1307, USA.  
 */

#include "config.h"

#include "gck-secret-data.h"
#include "gck-secret-fields.h"
#include "gck-secret-item.h"

#include "gck/gck-attributes.h"
#include "gck/gck-module.h"
#include "gck/gck-secret.h"
#include "gck/gck-session.h"
#include "gck/gck-transaction.h"

#include "pkcs11/pkcs11i.h"

#include <glib/gi18n.h>

enum {
	PROP_0,
	PROP_COLLECTION,
	PROP_FIELDS,
	PROP_SCHEMA
};

struct _GckSecretItem {
	GckSecretObject parent;
	GHashTable *fields;
	gchar *schema;
	GckSecretCollection *collection;
};

G_DEFINE_TYPE (GckSecretItem, gck_secret_item, GCK_TYPE_SECRET_OBJECT);

/* -----------------------------------------------------------------------------
 * INTERNAL 
 */

static gboolean
complete_set_schema (GckTransaction *transaction, GObject *obj, gpointer user_data)
{
	GckSecretItem *self = GCK_SECRET_ITEM (obj);
	gchar *old_schema = user_data;

	if (gck_transaction_get_failed (transaction)) {
		g_free (self->schema);
		self->schema = old_schema;
	} else {
		gck_object_notify_attribute (GCK_OBJECT (obj), CKA_G_SCHEMA);
		g_object_notify (G_OBJECT (obj), "schema");
		gck_secret_object_was_modified (GCK_SECRET_OBJECT (self));
		g_free (old_schema);
	}

	return TRUE;
}

static void
begin_set_schema (GckSecretItem *self, GckTransaction *transaction, gchar *schema)
{
	g_assert (GCK_IS_SECRET_OBJECT (self));
	g_assert (!gck_transaction_get_failed (transaction));

	if (self->schema != schema) {
		gck_transaction_add (transaction, self, complete_set_schema, self->schema);
		self->schema = schema;
	}
}

static gboolean
complete_set_secret (GckTransaction *transaction, GObject *obj, gpointer user_data)
{
	GckSecretItem *self = GCK_SECRET_ITEM (obj);
	
	if (!gck_transaction_get_failed (transaction)) {
		gck_object_notify_attribute (GCK_OBJECT (obj), CKA_VALUE);
		gck_secret_object_was_modified (GCK_SECRET_OBJECT (self));
	}

	return TRUE;
}

static gboolean
complete_set_fields (GckTransaction *transaction, GObject *obj, gpointer user_data)
{
	GckSecretItem *self = GCK_SECRET_ITEM (obj);
	GHashTable *old_fields = user_data;
	
	if (gck_transaction_get_failed (transaction)) {
		if (self->fields)
			g_hash_table_unref (self->fields);
		self->fields = old_fields;
	} else {
		gck_object_notify_attribute (GCK_OBJECT (obj), CKA_G_FIELDS);
		g_object_notify (G_OBJECT (obj), "fields");
		gck_secret_object_was_modified (GCK_SECRET_OBJECT (self));
		if (old_fields)
			g_hash_table_unref (old_fields);
	}

	return TRUE;
}

static void
begin_set_fields (GckSecretItem *self, GckTransaction *transaction, GHashTable *fields)
{
	g_assert (GCK_IS_SECRET_OBJECT (self));
	g_assert (!gck_transaction_get_failed (transaction));
	
	gck_transaction_add (transaction, self, complete_set_fields, self->fields);
	self->fields = fields;
}

static GckObject*
factory_create_item (GckSession *session, GckTransaction *transaction,
                     CK_ATTRIBUTE_PTR attrs, CK_ULONG n_attrs)
{
	GckSecretCollection *collection = NULL;
	GckSecretItem *item;
	GckManager *m_manager;
	GckManager *s_manager;
	CK_ATTRIBUTE *attr;
	gboolean is_token;
	gchar *identifier;

	g_return_val_if_fail (GCK_IS_TRANSACTION (transaction), NULL);
	g_return_val_if_fail (attrs || !n_attrs, NULL);

	/* See if a collection attribute was specified */
	attr = gck_attributes_find (attrs, n_attrs, CKA_G_COLLECTION);
	if (attr == NULL) {
		gck_transaction_fail (transaction, CKR_TEMPLATE_INCOMPLETE);
		return NULL;
	}

	m_manager = gck_module_get_manager (gck_session_get_module (session));
	s_manager = gck_session_get_manager (session);

	gck_attribute_consume (attr);
	if (!gck_attributes_find_boolean (attrs, n_attrs, CKA_TOKEN, &is_token))
		collection = gck_secret_collection_find (attr, m_manager, s_manager, NULL);
	else if (is_token)
		collection = gck_secret_collection_find (attr, m_manager, NULL);
	else
		collection = gck_secret_collection_find (attr, s_manager, NULL);

	if (!collection) {
		gck_transaction_fail (transaction, CKR_TEMPLATE_INCONSISTENT);
		return NULL;
	}

	/* If an ID was specified, then try and see if that ID already exists */
	if (gck_attributes_find_string (attrs, n_attrs, CKA_ID, &identifier)) {
		item = gck_secret_collection_get_item (collection, identifier);
		if (item == NULL) {
			gck_transaction_fail (transaction, CKR_TEMPLATE_INCONSISTENT);
			return NULL;
		} else {
			gck_attributes_consume (attrs, n_attrs, CKA_ID, G_MAXULONG);
			return g_object_ref (item);
		}
	}

	/* Create a new collection which will own the item */
	item = gck_secret_collection_create_item (collection, transaction);
	gck_session_complete_object_creation (session, transaction, GCK_OBJECT (item), attrs, n_attrs);
	return g_object_ref (item);
}

/* -----------------------------------------------------------------------------
 * OBJECT 
 */

static gboolean
gck_secret_item_real_is_locked (GckSecretObject *obj, GckSession *session)
{
	GckSecretItem *self = GCK_SECRET_ITEM (obj);
	if (!self->collection)
		return TRUE;
	return gck_secret_object_is_locked (GCK_SECRET_OBJECT (self->collection), session);
}

static CK_RV
gck_secret_item_real_get_attribute (GckObject *base, GckSession *session, CK_ATTRIBUTE_PTR attr)
{
	GckSecretItem *self = GCK_SECRET_ITEM (base);
	GckSecretData *sdata;
	const gchar *identifier;
	const guchar *secret;
	gsize n_secret = 0;

	g_return_val_if_fail (self->collection, CKR_GENERAL_ERROR);

	switch (attr->type) {
	case CKA_CLASS:
		return gck_attribute_set_ulong (attr, CKO_SECRET_KEY);

	case CKA_VALUE:
		sdata = gck_secret_collection_unlocked_data (self->collection, session);
		if (sdata == NULL)
			return CKR_USER_NOT_LOGGED_IN;
		identifier = gck_secret_object_get_identifier (GCK_SECRET_OBJECT (self));
		secret = gck_secret_data_get_raw (sdata, identifier, &n_secret);
		return gck_attribute_set_data (attr, secret, n_secret);

	case CKA_G_COLLECTION:
		g_return_val_if_fail (self->collection, CKR_GENERAL_ERROR);
		identifier = gck_secret_object_get_identifier (GCK_SECRET_OBJECT (self->collection));
		return gck_attribute_set_string (attr, identifier);

	case CKA_G_FIELDS:
		if (!self->fields)
			return gck_attribute_set_data (attr, NULL, 0);
		return gck_secret_fields_serialize (attr, self->fields);

	case CKA_G_SCHEMA:
		return gck_attribute_set_string (attr, self->schema);
	}

	return GCK_OBJECT_CLASS (gck_secret_item_parent_class)->get_attribute (base, session, attr);
}

static void
gck_secret_item_real_set_attribute (GckObject *base, GckSession *session, 
                                    GckTransaction *transaction, CK_ATTRIBUTE_PTR attr)
{
	GckSecretItem *self = GCK_SECRET_ITEM (base);
	const gchar *identifier;
	GckSecretData *sdata;
	GHashTable *fields;
	GckSecret *secret;
	gchar *schema;
	CK_RV rv;

	if (!self->collection) {
		gck_transaction_fail (transaction, CKR_GENERAL_ERROR);
		g_return_if_reached ();
	}

	/* Check that the object is not locked */
	sdata = gck_secret_collection_unlocked_data (self->collection, session);
	if (sdata == NULL) {
		gck_transaction_fail (transaction, CKR_USER_NOT_LOGGED_IN);
		return;
	}

	switch (attr->type) {
	case CKA_VALUE:
		identifier = gck_secret_object_get_identifier (GCK_SECRET_OBJECT (self));
		secret = gck_secret_new (attr->pValue, attr->ulValueLen);
		gck_secret_data_set_transacted (sdata, transaction, identifier, secret);
		g_object_unref (secret);
		if (!gck_transaction_get_failed (transaction))
			gck_transaction_add (transaction, self, complete_set_secret, NULL);
		return;

	case CKA_G_FIELDS:
		rv = gck_secret_fields_parse (attr, &fields);
		if (rv != CKR_OK)
			gck_transaction_fail (transaction, rv);
		else
			begin_set_fields (self, transaction, fields);
		return;

	case CKA_G_SCHEMA:
		rv = gck_attribute_get_string (attr, &schema);
		if (rv != CKR_OK)
			gck_transaction_fail (transaction, rv);
		else
			begin_set_schema (self, transaction, schema);
	}

	GCK_OBJECT_CLASS (gck_secret_item_parent_class)->set_attribute (base, session, transaction, attr);
}

static void
gck_secret_item_init (GckSecretItem *self)
{
	
}

static GObject* 
gck_secret_item_constructor (GType type, guint n_props, GObjectConstructParam *props) 
{
	GckSecretItem *self = GCK_SECRET_ITEM (G_OBJECT_CLASS (gck_secret_item_parent_class)->constructor(type, n_props, props));
	g_return_val_if_fail (self, NULL);
	
	g_return_val_if_fail (self->collection, NULL);

	return G_OBJECT (self);
}

static void
gck_secret_item_set_property (GObject *obj, guint prop_id, const GValue *value, 
                              GParamSpec *pspec)
{
	GckSecretItem *self = GCK_SECRET_ITEM (obj);
	
	switch (prop_id) {
	case PROP_COLLECTION:
		g_return_if_fail (!self->collection);
		self->collection = g_value_get_object (value);
		g_return_if_fail (self->collection);
		g_object_add_weak_pointer (G_OBJECT (self->collection), 
		                           (gpointer*)&(self->collection));
		break;
	case PROP_FIELDS:
		gck_secret_item_set_fields (self, g_value_get_boxed (value));
		break;
	case PROP_SCHEMA:
		gck_secret_item_set_schema (self, g_value_get_string (value));
		break;
	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID (obj, prop_id, pspec);
		break;
	}
}

static void
gck_secret_item_get_property (GObject *obj, guint prop_id, GValue *value, 
                              GParamSpec *pspec)
{
	GckSecretItem *self = GCK_SECRET_ITEM (obj);
	
	switch (prop_id) {
	case PROP_COLLECTION:
		g_value_set_object (value, gck_secret_item_get_collection (self));
		break;
	case PROP_FIELDS:
		g_value_set_boxed (value, gck_secret_item_get_fields (self));
		break;
	case PROP_SCHEMA:
		g_value_set_string (value, gck_secret_item_get_schema (self));
		break;
	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID (obj, prop_id, pspec);
		break;
	}
}

static void
gck_secret_item_dispose (GObject *obj)
{
	GckSecretItem *self = GCK_SECRET_ITEM (obj);

	if (self->collection)
		g_object_remove_weak_pointer (G_OBJECT (self->collection),
		                              (gpointer*)&(self->collection));
	self->collection = NULL;
	
	G_OBJECT_CLASS (gck_secret_item_parent_class)->dispose (obj);
}

static void
gck_secret_item_finalize (GObject *obj)
{
	GckSecretItem *self = GCK_SECRET_ITEM (obj);
	
	g_assert (!self->collection);
	
	if (self->fields)
		g_hash_table_unref (self->fields);
	self->fields = NULL;

	G_OBJECT_CLASS (gck_secret_item_parent_class)->finalize (obj);
}

static void
gck_secret_item_class_init (GckSecretItemClass *klass)
{
	GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
	GckObjectClass *gck_class = GCK_OBJECT_CLASS (klass);
	GckSecretObjectClass *secret_class = GCK_SECRET_OBJECT_CLASS (klass);
	
	gck_secret_item_parent_class = g_type_class_peek_parent (klass);
	
	gobject_class->constructor = gck_secret_item_constructor;
	gobject_class->dispose = gck_secret_item_dispose;
	gobject_class->finalize = gck_secret_item_finalize;
	gobject_class->set_property = gck_secret_item_set_property;
	gobject_class->get_property = gck_secret_item_get_property;

	gck_class->get_attribute = gck_secret_item_real_get_attribute;
	gck_class->set_attribute = gck_secret_item_real_set_attribute;

	secret_class->is_locked = gck_secret_item_real_is_locked;

	g_object_class_install_property (gobject_class, PROP_COLLECTION,
	           g_param_spec_object ("collection", "Collection", "Item's Collection",
	                                GCK_TYPE_SECRET_COLLECTION, G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY));

	g_object_class_install_property (gobject_class, PROP_FIELDS,
	           g_param_spec_boxed ("fields", "Fields", "Item's fields",
	                               GCK_BOXED_SECRET_FIELDS, G_PARAM_READWRITE));

	g_object_class_install_property (gobject_class, PROP_SCHEMA,
	           g_param_spec_string ("schema", "Schema", "Item's type or schema",
	                                NULL, G_PARAM_READWRITE));
}

/* -----------------------------------------------------------------------------
 * PUBLIC
 */

GckFactory*
gck_secret_item_get_factory (void)
{
	static CK_OBJECT_CLASS klass = CKO_SECRET_KEY;

	static CK_ATTRIBUTE attributes[] = {
		{ CKA_CLASS, &klass, sizeof (klass) },
	};

	static GckFactory factory = {
		attributes,
		G_N_ELEMENTS (attributes),
		factory_create_item
	};

	return &factory;
}

GckSecretCollection*
gck_secret_item_get_collection (GckSecretItem *self)
{
	g_return_val_if_fail (GCK_IS_SECRET_ITEM (self), NULL);
	return self->collection;
}

GHashTable*
gck_secret_item_get_fields (GckSecretItem *self)
{
	g_return_val_if_fail (GCK_IS_SECRET_ITEM (self), NULL);
	if (self->fields == NULL)
		self->fields = gck_secret_fields_new ();
	return self->fields;
}

void
gck_secret_item_set_fields (GckSecretItem *self, GHashTable *fields)
{
	g_return_if_fail (GCK_IS_SECRET_ITEM (self));
	
	if (fields)
		g_hash_table_ref (fields);
	if (self->fields)
		g_hash_table_unref (self->fields);
	self->fields = fields;
	
	g_object_notify (G_OBJECT (self), "fields");
	gck_object_notify_attribute (GCK_OBJECT (self), CKA_G_FIELDS);
}

const gchar*
gck_secret_item_get_schema (GckSecretItem *self)
{
	g_return_val_if_fail (GCK_IS_SECRET_ITEM (self), NULL);
	return self->schema;
}

void
gck_secret_item_set_schema (GckSecretItem *self, const gchar *schema)
{
	g_return_if_fail (GCK_IS_SECRET_ITEM (self));

	if (schema != self->schema) {
		g_free (self->schema);
		self->schema = g_strdup (schema);
		g_object_notify (G_OBJECT (self), "schema");
		gck_object_notify_attribute (GCK_OBJECT (self), CKA_G_SCHEMA);
	}
}