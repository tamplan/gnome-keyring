/*
 * gnome-keyring
 *
 * Copyright (C) 2010 Stefan Walter
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

#include "gkm-wrap-prompt.h"

#include "egg/egg-secure-memory.h"

#include "gkm/gkm-attributes.h"
#include "gkm/gkm-util.h"

#include "pkcs11/pkcs11.h"
#include "pkcs11/pkcs11i.h"

#include "ui/gku-prompt.h"

#include <glib/gi18n.h>

#include <string.h>

struct _GkmWrapPrompt {
	GkuPrompt parent;

	CK_FUNCTION_LIST_PTR module;
	CK_SESSION_HANDLE session;
	CK_OBJECT_HANDLE object;

	gpointer prompt_data;
	GDestroyNotify destroy_data;

	GQueue pool;
};

typedef struct _CredentialPrompt {
	GArray *template;
	CK_ULONG n_template;
	gchar *password;
	guint iteration;
} CredentialPrompt;

G_DEFINE_TYPE (GkmWrapPrompt, gkm_wrap_prompt, GKU_TYPE_PROMPT);

/* -----------------------------------------------------------------------------
 * INTERNAL
 */

#if 0
static gchar*
location_string_for_attributes (GP11Attributes *attrs)
{
	gchar *identifier;
	gchar *location;

	identifier = identifier_string_for_attributes (attrs);
	if (identifier == NULL)
		return NULL;

	/*
	 * COMPAT: Format it into a string. This is done this way for compatibility
	 * with old gnome-keyring releases. In the future this may change.
	 *
	 * FYI: gp11_object_get_data() null terminates
	 */
	location = g_strdup_printf ("LOCAL:/keyrings/%s.keyring", (gchar*)identifier);
	g_free (identifier);
	return location;
}

static void
set_warning_wrong (GkdSecretUnlock *self)
{
	g_assert (GKD_SECRET_IS_UNLOCK (self));
	gku_prompt_set_warning (GKU_PROMPT (self), _("The unlock password was incorrect"));
}

static void
attach_unlock_to_login (GP11Object *collection, GkdSecretSecret *master)
{
	DBusError derr = DBUS_ERROR_INIT;
	GP11Attributes *attrs;
	GP11Object *cred;
	gchar *location;
	gchar *label;

	g_assert (GP11_IS_OBJECT (collection));

	/* Relevant information for the unlock item */
	attrs = attributes_for_collection (collection);
	g_return_if_fail (attrs);
	location = location_string_for_attributes (attrs);
	label = label_string_for_attributes (attrs);
	gp11_attributes_unref (attrs);

	attrs = gkd_login_attach_make_attributes (label, "keyring", location, NULL);
	g_free (location);
	g_free (label);

	cred = gkd_secret_session_create_credential (master->session, NULL, attrs, master, &derr);
	gp11_attributes_unref (attrs);
	g_object_unref (cred);

	if (!cred) {
		g_warning ("couldn't save unlock password in login collection: %s", derr.message);
		dbus_error_free (&derr);
	}
}

/* Save it to the login keyring */
if (!transient)
	attach_unlock_to_login (collection, master);

/* Or try to use login keyring's passwords */
} else {
	attrs = attributes_for_collection (coll);
	location = location_string_for_attributes (attrs);
	gp11_attributes_unref (attrs);

	if (location) {
		password = gkd_login_lookup_secret ("keyring", location, NULL);
		g_free (location);

		if (password) {
			if (gkd_secret_unlock_with_password (coll, NULL, 0, NULL))
				locked = FALSE;
			egg_secure_strfree (password);
		}
	}

#endif

static GkuPrompt*
on_prompt_attention (gpointer user_data)
{
	/* We passed the prompt as the argument */
	return g_object_ref (user_data);
}

static gpointer
pool_alloc (GkmWrapPrompt *self, gsize length)
{
	gpointer memory = g_malloc0 (length);

	g_assert (GKM_WRAP_IS_PROMPT (self));
	g_queue_push_tail (&self->pool, memory);
	return memory;
}

static gpointer
pool_dup (GkmWrapPrompt *self, gconstpointer original, gsize length)
{
	gpointer memory = pool_alloc (self, length);
	memcpy (memory, original, length);
	return memory;
}

static CK_ATTRIBUTE_PTR
get_unlock_options_from_object (GkmWrapPrompt *self, CK_ULONG_PTR n_options)
{
	CK_ATTRIBUTE_PTR options;
	CK_ATTRIBUTE attr;
	CK_ULONG i;
	CK_RV rv;

	g_assert (GKM_WRAP_IS_PROMPT (self));
	g_assert (self->module);
	g_assert (n_options);

	*n_options = 0;

	attr.type = CKA_G_CREDENTIAL_TEMPLATE;
	attr.ulValueLen = 0;
	attr.pValue = NULL;

	/* Get the length of the entire template */
	rv = (self->module->C_GetAttributeValue) (self->session, self->object, &attr, 1);
	if (rv != CKR_OK) {
		if (rv != CKR_ATTRIBUTE_TYPE_INVALID)
			g_warning ("couldn't get credential template for prompt: %s",
			           gkm_util_rv_to_string (rv));
		return NULL;
	}

	/* Number of attributes, rounded down */
	*n_options = (attr.ulValueLen / sizeof (CK_ATTRIBUTE));;
	attr.pValue = options = pool_alloc (self, attr.ulValueLen);

	/* Get the size of each value */
	rv = (self->module->C_GetAttributeValue) (self->session, self->object, &attr, 1);
	if (rv != CKR_OK) {
		g_warning ("couldn't read credential template for prompt: %s",
		           gkm_util_rv_to_string (rv));
		return NULL;
	}

	/* Allocate memory for each value */
	for (i = 0; i < *n_options; ++i) {
		if (options[i].ulValueLen != (CK_ULONG)-1)
			options[i].pValue = pool_alloc (self, options[i].ulValueLen);
	}

	/* Now get the actual values */
	rv = (self->module->C_GetAttributeValue) (self->session, self->object, &attr, 1);
	if (rv != CKR_OK) {
		g_warning ("couldn't retrieve credential template for prompt: %s",
		           gkm_util_rv_to_string (rv));
		return NULL;
	}

	return options;
}

static void
set_unlock_options_on_object (GkmWrapPrompt *self, CK_ATTRIBUTE_PTR options, CK_ULONG n_options)
{
	CK_ATTRIBUTE attr;
	CK_RV rv;

	g_assert (GKM_WRAP_IS_PROMPT (self));
	g_assert (self->module);
	g_assert (options);

	attr.type = CKA_G_CREDENTIAL_TEMPLATE;
	attr.pValue = options;
	attr.ulValueLen = sizeof (CK_ATTRIBUTE) * n_options;

	rv = (self->module->C_SetAttributeValue) (self->session, self->object, &attr, 1);
	if (rv != CKR_OK && rv != CKR_ATTRIBUTE_TYPE_INVALID) {
		g_warning ("Couldn't set credential template for prompt: %s",
		           gkm_util_rv_to_string (rv));
	}
}

static CK_ATTRIBUTE_PTR
get_unlock_options_from_prompt (GkmWrapPrompt *self, CK_ULONG_PTR n_options)
{
	CK_ATTRIBUTE_PTR options;
	CK_BBOOL bval;
	CK_ULONG uval;
	gint value;

	g_assert (GKM_WRAP_IS_PROMPT (self));
	g_assert (n_options);

	*n_options = 4;
	options = pool_alloc (self, sizeof (CK_ATTRIBUTE) * (*n_options));

	/* CKA_TOKEN */
	bval = TRUE;
	options[0].type = CKA_TOKEN;
	options[0].pValue = pool_dup (self, &bval, sizeof (bval));
	options[0].ulValueLen = sizeof (bval);

	/* CKA_GNOME_TRANSIENT */
	bval = TRUE;
	options[1].type = CKA_GNOME_TRANSIENT;
	options[1].pValue = pool_dup (self, &bval, sizeof (bval));
	options[1].ulValueLen = sizeof (bval);

	/* CKA_G_DESTRUCT_IDLE */
	value = 0;
	gku_prompt_get_unlock_option (GKU_PROMPT (self), GKU_UNLOCK_IDLE, &value);
	uval = value < 0 ? 0 : value;
	options[2].type = CKA_G_DESTRUCT_IDLE;
	options[2].pValue = pool_dup (self, &uval, sizeof (uval));
	options[2].ulValueLen = sizeof (uval);

	/* CKA_G_DESTRUCT_AFTER */
	value = 0;
	gku_prompt_get_unlock_option (GKU_PROMPT (self), GKU_UNLOCK_TIMEOUT, &value);
	uval = value < 0 ? 0 : value;
	options[3].type = CKA_G_DESTRUCT_AFTER;
	options[3].pValue = pool_dup (self, &uval, sizeof (uval));
	options[3].ulValueLen = sizeof (uval);

	return options;
}

static void
set_unlock_options_on_prompt (GkmWrapPrompt *self, CK_ATTRIBUTE_PTR options, CK_ULONG n_options)
{
	gboolean bval;
	gulong uval;

	g_assert (GKM_WRAP_IS_PROMPT (self));
	g_assert (options || !n_options);

	if (gkm_attributes_find_boolean (options, n_options, CKA_GNOME_TRANSIENT, &bval))
		gku_prompt_set_unlock_option (GKU_PROMPT (self), GKU_UNLOCK_AUTO, bval ? 0 : 1);

	if (gkm_attributes_find_ulong (options, n_options, CKA_G_DESTRUCT_IDLE, &uval))
		gku_prompt_set_unlock_option (GKU_PROMPT (self), GKU_UNLOCK_IDLE, (int)uval);

	if (gkm_attributes_find_ulong (options, n_options, CKA_G_DESTRUCT_AFTER, &uval))
		gku_prompt_set_unlock_option (GKU_PROMPT (self), GKU_UNLOCK_TIMEOUT, (int)uval);
}

static CK_ATTRIBUTE_PTR
get_attributes_from_object (GkmWrapPrompt *self, CK_ULONG *n_attrs)
{
	CK_ATTRIBUTE attrs[3];
	CK_ULONG i, count = 3;
	CK_RV rv;

	g_assert (GKM_WRAP_IS_PROMPT (self));
	g_assert (n_attrs);
	g_assert (self->module);

	memset (attrs, 0, sizeof (attrs));
	attrs[0].type = CKA_LABEL;
	attrs[1].type = CKA_ID;
	attrs[2].type = CKA_CLASS;

	rv = (self->module->C_GetAttributeValue) (self->session, self->object, attrs, count);
	if (rv != CKR_OK && rv != CKR_ATTRIBUTE_TYPE_INVALID) {
		g_warning ("Couldn't retrieve information about object to unlock: %s",
		           gkm_util_rv_to_string (rv));
		return NULL;
	}

	/* Allocate for each value, note we're null terminating values */
	for (i = 0; i < count; ++i) {
		if (attrs[i].ulValueLen != (CK_ULONG)-1)
			attrs[i].pValue = pool_alloc (self, attrs[i].ulValueLen + 1);
	}

	/* Now get the actual values */
	rv = (self->module->C_GetAttributeValue) (self->session, self->object, attrs, count);
	if (rv != CKR_OK && rv != CKR_ATTRIBUTE_TYPE_INVALID) {
		g_warning ("couldn't retrieve credential template for prompt: %s",
		           gkm_util_rv_to_string (rv));
		return NULL;
	}

	*n_attrs = count;
	return pool_dup (self, attrs, sizeof (attrs));

}

static void
prepare_unlock_keyring_login (GkmWrapPrompt *self)
{
	GkuPrompt *prompt;
	const gchar *text;

	g_assert (GKM_WRAP_IS_PROMPT (self));

	prompt = GKU_PROMPT (self);

	gku_prompt_set_title (prompt, _("Unlock Login Keyring"));

	text = _("Enter password for to unlock your login keyring");
	gku_prompt_set_primary_text (prompt, text);

#if 0
	/* TODO: Reimplement this */
	if (gkd_login_did_unlock_fail ())
		text = _("The password you use to log in to your computer no longer matches that of your login keyring.");
	else
#endif
		text = _("The login keyring did not get unlocked when you logged into your computer.");
	gku_prompt_set_secondary_text (prompt, text);

	gku_prompt_hide_widget (prompt, "name_area");
	gku_prompt_hide_widget (prompt, "confirm_area");
	gku_prompt_show_widget (prompt, "password_area");
}



static void
prepare_unlock_keyring_other (GkmWrapPrompt *self, const gchar *label)
{
	GkuPrompt *prompt;
	gchar *text;

	g_assert (GKM_WRAP_IS_PROMPT (self));

	prompt = GKU_PROMPT (self);

	gku_prompt_set_title (prompt, _("Unlock Keyring"));

	text = g_markup_printf_escaped (_("Enter password for keyring '%s' to unlock"), label);
	gku_prompt_set_primary_text (prompt, text);
	g_free (text);

	text = g_markup_printf_escaped (_("An application wants access to the keyring '%s', but it is locked"), label);
	gku_prompt_set_secondary_text (prompt, text);
	g_free (text);

	gku_prompt_hide_widget (prompt, "name_area");
	gku_prompt_hide_widget (prompt, "confirm_area");
	gku_prompt_show_widget (prompt, "details_area");
	gku_prompt_show_widget (prompt, "password_area");
	gku_prompt_show_widget (prompt, "lock_area");
	gku_prompt_show_widget (prompt, "options_area");

#if 0 /* TODO: Implement */
	if (gkd_login_is_usable ())
		gku_prompt_show_widget (prompt, "auto_unlock_check");
	else
#endif
		gku_prompt_hide_widget (prompt, "auto_unlock_check");
}


static const gchar*
prepare_unlock_object_title (CK_OBJECT_CLASS klass)
{
	switch (klass) {
	case CKO_PRIVATE_KEY:
		return _("Unlock private key");
	case CKO_CERTIFICATE:
		return _("Unlock certificate");
	case CKO_PUBLIC_KEY:
		return _("Unlock public key");
	default:
		return _("Unlock");
	}
}

static const gchar*
prepare_unlock_object_primary (CK_OBJECT_CLASS klass)
{
	switch (klass) {
	case CKO_PRIVATE_KEY:
		return _("Enter password to unlock the private key");
	case CKO_CERTIFICATE:
		return _("Enter password to unlock the certificate");
	case CKO_PUBLIC_KEY:
		return _("Enter password to unlock the public key");
	default:
		return _("Enter password to unlock");
	}
}

static gchar*
prepare_unlock_object_secondary (CK_OBJECT_CLASS klass, const gchar *label)
{
	switch (klass) {
	case CKO_PRIVATE_KEY:
		/* TRANSLATORS: The private key is locked */
		return g_strdup_printf (_("An application wants access to the private key '%s', but it is locked"), label);
	case CKO_CERTIFICATE:
		/* TRANSLATORS: The certificate is locked */
		return g_strdup_printf (_("An application wants access to the certificate '%s', but it is locked"), label);
	case CKO_PUBLIC_KEY:
		/* TRANSLATORS: The public key is locked */
		return g_strdup_printf (_("An application wants access to the public key '%s', but it is locked"), label);
	default:
		/* TRANSLATORS: The object '%s' is locked */
		return g_strdup_printf (_("An application wants access to '%s', but it is locked"), label);
	}
}

static void
prepare_unlock_object (GkmWrapPrompt *self, const gchar *label, CK_OBJECT_CLASS klass)
{
	GkuPrompt *prompt;
	gchar *text;

	g_assert (GKM_WRAP_IS_PROMPT (self));

	prompt = GKU_PROMPT (self);

	gku_prompt_set_title (prompt, prepare_unlock_object_title (klass));
	gku_prompt_set_primary_text (prompt, prepare_unlock_object_primary (klass));

	text = prepare_unlock_object_secondary (klass, label);
	gku_prompt_set_secondary_text (prompt, text);
	g_free (text);

	gku_prompt_hide_widget (prompt, "name_area");
	gku_prompt_hide_widget (prompt, "confirm_area");
	gku_prompt_show_widget (prompt, "details_area");
	gku_prompt_show_widget (prompt, "password_area");
	gku_prompt_show_widget (prompt, "lock_area");
	gku_prompt_show_widget (prompt, "options_area");
	gku_prompt_hide_widget (prompt, "auto_unlock_check");
}

static void
prepare_unlock_prompt (GkmWrapPrompt *self, gboolean first)
{
	CK_ATTRIBUTE_PTR attrs;
	CK_ATTRIBUTE_PTR attr;
	CK_ULONG n_attrs;
	GkuPrompt *prompt;
	const gchar *label = NULL;
	CK_OBJECT_CLASS klass;

	g_assert (GKM_WRAP_IS_PROMPT (self));

	prompt = GKU_PROMPT (self);

	/* Hard reset on first prompt, soft on later */
	gku_prompt_reset (GKU_PROMPT (prompt), first);

	/* Load up all the values, note they're null terminated */
	attrs = get_attributes_from_object (self, &n_attrs);
	g_return_if_fail (attrs);

	/* Load up the object class */
	if (!gkm_attributes_find_ulong (attrs, n_attrs, CKA_CLASS, &klass))
		klass = (CK_ULONG)-1;

	/* Load up its label */
	attr = gkm_attributes_find (attrs, n_attrs, CKA_LABEL);
	if (attr != NULL)
		label = attr->pValue;

	/* Load up the identifier */
	attr = gkm_attributes_find (attrs, n_attrs, CKA_ID);
	if (attr != NULL && !label)
		label = attr->pValue;

	if (!label)
		label = _("Unnamed");

	if (klass == CKO_G_COLLECTION) {
		if (attr && attr->pValue && attr->ulValueLen == 5 &&
		    memcmp (attr->pValue, "login", 5)) {
			prepare_unlock_keyring_login (self);
		} else {
			prepare_unlock_keyring_other (self, label);
		}
	} else {
		prepare_unlock_object (self, label, klass);
	}

	if (!first)
		gku_prompt_set_warning (GKU_PROMPT (self), _("The unlock password was incorrect"));
}

static void
prepare_unlock_token (GkmWrapPrompt *self, CK_TOKEN_INFO_PTR tinfo)
{
	GkuPrompt *prompt;
	gchar *label;
	gchar *text;

	g_assert (GKM_WRAP_IS_PROMPT (self));

	prompt = GKU_PROMPT (self);

	label = g_strndup ((gchar*)tinfo->label, sizeof (tinfo->label));
	g_strchomp (label);

	/* Build up the prompt */
	gku_prompt_show_widget (prompt, "password_area");
	gku_prompt_hide_widget (prompt, "confirm_area");
	gku_prompt_hide_widget (prompt, "original_area");
	gku_prompt_set_title (prompt, _("Unlock certificate/key storage"));
	gku_prompt_set_primary_text (prompt, _("Enter password to unlock the certificate/key storage"));

	/* TRANSLATORS: The storage is locked, and needs unlocking before the application can use it. */
	text = g_strdup_printf (_("An application wants access to the certificate/key storage '%s', but it is locked"), label);
	gku_prompt_set_secondary_text (prompt, text);
	g_free (text);

#if 0
	if (gkd_login_is_usable ()) {
		gku_prompt_show_widget (prompt, "details_area");
		gku_prompt_show_widget (prompt, "lock_area");
		gku_prompt_hide_widget (prompt, "options_area");
	}
#endif

	g_free (label);
}

/* -----------------------------------------------------------------------------
 * OBJECT
 */

static void
gkm_wrap_prompt_init (GkmWrapPrompt *self)
{
	g_queue_init (&self->pool);
}

static void
gkm_wrap_prompt_finalize (GObject *obj)
{
	GkmWrapPrompt *self = GKM_WRAP_PROMPT (obj);

	if (self->destroy_data && self->prompt_data)
		(self->destroy_data) (self->prompt_data);
	self->destroy_data = NULL;
	self->prompt_data = NULL;

	while (!g_queue_is_empty(&self->pool))
		g_free (g_queue_pop_head (&self->pool));


	G_OBJECT_CLASS (gkm_wrap_prompt_parent_class)->finalize (obj);
}


static void
gkm_wrap_prompt_class_init (GkmWrapPromptClass *klass)
{
	GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
	gobject_class->finalize = gkm_wrap_prompt_finalize;
}

/* -----------------------------------------------------------------------------
 * CREDENTIAL
 */

static void
credential_prompt_free (gpointer user_data)
{
	CredentialPrompt *data = user_data;
	egg_secure_strfree (data->password);
	g_array_free (data->template, TRUE);
	g_slice_free (CredentialPrompt, data);
}

GkmWrapPrompt*
gkm_wrap_prompt_for_credential (CK_FUNCTION_LIST_PTR module, CK_SESSION_HANDLE session,
                                CK_ATTRIBUTE_PTR template, CK_ULONG n_template)
{
	CredentialPrompt *data;
	CK_ATTRIBUTE_PTR attr;
	CK_ATTRIBUTE_PTR options;
	CK_ULONG n_options, i;
	CK_OBJECT_CLASS klass;
	CK_OBJECT_HANDLE object;
	GkmWrapPrompt *self;

	g_return_val_if_fail (module, NULL);
	g_return_val_if_fail (session, NULL);
	g_return_val_if_fail (n_template || !template, NULL);

	/* Must be credential and have object for protected outh path */
	if (!gkm_attributes_find_ulong (template, n_template, CKA_CLASS, &klass) ||
	    !gkm_attributes_find_ulong (template, n_template, CKA_G_OBJECT, &object) ||
	    klass != CKO_G_CREDENTIAL || object == 0)
		return NULL;

	/* Must have CKA_VALUE with pValue set to null for protected auth path */
	attr = gkm_attributes_find (template, n_template, CKA_VALUE);
	if (attr == NULL || attr->pValue != NULL)
		return NULL;

	/* Build up the prompt */
	self = g_object_new (GKM_WRAP_TYPE_PROMPT, NULL);
	self->prompt_data = data = g_slice_new0 (CredentialPrompt);
	self->destroy_data = credential_prompt_free;
	self->module = module;
	self->session = session;
	self->object = object;

	/* Build up a copy of the template with CKA_VALUE first */
	data->template = g_array_new (FALSE, FALSE, sizeof (CK_ATTRIBUTE));
	g_array_append_val (data->template, *attr);
	for (i = 0; i < n_template; ++i) {
		if (template[i].type != CKA_VALUE)
			g_array_append_val (data->template, template[i]);
	}

	data->n_template = n_template;

	/* Now load up the unlock options into the prompt*/
	options = get_unlock_options_from_object (self, &n_options);
	if (options != NULL)
		set_unlock_options_on_prompt (self, options, n_options);

	return self;
}

gboolean
gkm_wrap_prompt_do_credential (GkmWrapPrompt *self, CK_ATTRIBUTE_PTR *template,
                               CK_ULONG *n_template)
{
	CK_ATTRIBUTE_PTR options;
	CK_ATTRIBUTE_PTR attr;
	CK_ULONG n_options, i;
	CredentialPrompt *data;

	g_return_val_if_fail (GKM_WRAP_IS_PROMPT (self), FALSE);
	g_return_val_if_fail (template, FALSE);
	g_return_val_if_fail (n_template, FALSE);

	g_assert (self->destroy_data == credential_prompt_free);
	data = self->prompt_data;

	prepare_unlock_prompt (self, data->iteration == 0);
	++(data->iteration);

	gku_prompt_request_attention_sync (NULL, on_prompt_attention,
	                                   g_object_ref (self), g_object_unref);

	if (gku_prompt_get_response (GKU_PROMPT (self)) != GKU_RESPONSE_OK)
		return FALSE;

	egg_secure_strfree (data->password);
	data->password = gku_prompt_get_password (GKU_PROMPT (self), "password");
	g_return_val_if_fail (data->password, FALSE);

	/* Truncate any extra options off the end of template */
	g_assert (data->n_template > 0);
	g_assert (data->template->len >= data->n_template);
	g_array_set_size (data->template, data->n_template);

	/* Put the password into the template, always first */
	attr = &g_array_index (data->template, CK_ATTRIBUTE, 0);
	g_assert (attr->type == CKA_VALUE);
	attr->pValue = data->password;
	attr->ulValueLen = strlen (data->password);

	/* Tag any options onto the end of template */
	options = get_unlock_options_from_prompt (self, &n_options);
	for (i = 0; options && i < n_options; ++i)
		g_array_append_val (data->template, options[i]);

	*template = (CK_ATTRIBUTE_PTR)data->template->data;
	*n_template = data->template->len;
	return TRUE;
}

void
gkm_wrap_prompt_done_credential (GkmWrapPrompt *self, CK_RV call_result)
{
	CK_ATTRIBUTE_PTR options;
	CK_ULONG n_options;
	gint value = 0;

	g_return_if_fail (GKM_WRAP_IS_PROMPT (self));

	/* Save the options, and possibly auto unlock */
	if (call_result == CKR_OK) {
		options = get_unlock_options_from_prompt (self, &n_options);
		if (options != NULL)
			set_unlock_options_on_object (self, options, n_options);

		if (gku_prompt_get_unlock_option (GKU_PROMPT (self), GKU_UNLOCK_AUTO, &value) && value) {
			g_assert_not_reached (); /* TODO: Need to implement */
		}

	/* Make sure to remove any auto-unlock when we fail */
	} else if (call_result == CKR_PIN_INCORRECT) {
		/* TODO: Implement removal from login keyring */
	}
}

#if 0
GkmWrapPrompt*
gkm_wrap_prompt_for_init_pin (CK_FUNCTION_LIST_PTR module, CK_SESSION_HANDLE session,
                              CK_UTF8CHAR_PTR pin, CK_ULONG pin_len)
{

}

gboolean
gkm_wrap_prompt_do_init_pin (GkmWrapPrompt *prompt, CK_RV last_result,
                             CK_UTF8CHAR_PTR *pin, CK_ULONG *n_pin)
{

}

void
gkm_wrap_prompt_done_init_pin (GkmWrapPrompt *prompt, CK_RV call_result)
{

}

GkmWrapPrompt*
gkm_wrap_prompt_for_set_pin (CK_FUNCTION_LIST_PTR module, CK_SESSION_HANDLE session,
                             CK_UTF8CHAR_PTR old_pin, CK_ULONG n_old_pin,
                             CK_UTF8CHAR_PTR new_pin, CK_ULONG n_new_pin)
{

}

gboolean
gkm_wrap_prompt_do_set_pin (GkmWrapPrompt *prompt, CK_RV last_result,
                            CK_UTF8CHAR_PTR *old_pin, CK_ULONG *n_old_pin,
                            CK_UTF8CHAR_PTR *new_pin, CK_ULONG *n_new_pin)
{

}

void
gkm_wrap_prompt_done_set_pin (GkmWrapPrompt *prompt, CK_RV call_result)
{

}
#endif

/* -----------------------------------------------------------------------------
 * LOGIN
 */

static GkmWrapPrompt*
login_prompt_for_specific (CK_FUNCTION_LIST_PTR module, CK_SESSION_HANDLE session,
                           CK_OBJECT_HANDLE object)
{
	GkmWrapPrompt *self;
	CK_ATTRIBUTE attr;
	CK_BBOOL always;
	CK_RV rv;

	g_assert (module);

	/*
	 * Should have an object at this point, if none exists it's an
	 * indication of either a buggy PKCS#11 module, or bugs in this
	 * wrap-layer not stashing away the context specific object.
	 */
	g_return_val_if_fail (object != 0, NULL);

	/* Find out if the object is CKA_ALWAYS_AUTHENTICATE */
	always = CK_FALSE;
	attr.type = CKA_ALWAYS_AUTHENTICATE;
	attr.pValue = &always;
	attr.ulValueLen = sizeof (always);

	rv = (module->C_GetAttributeValue) (session, object, &attr, 1);
	if (rv != CKR_OK || always != CK_TRUE)
		return NULL;

	/* Build up the prompt */
	self = g_object_new (GKM_WRAP_TYPE_PROMPT, NULL);
	self->module = module;
	self->session = session;
	self->object = object;
	self->destroy_data = (GDestroyNotify)egg_secure_strfree;

	return self;
}

static gboolean
login_prompt_do_specific (GkmWrapPrompt *self, CK_RV last_result,
                          CK_UTF8CHAR_PTR *pin, CK_ULONG *n_pin)
{
	g_assert (GKM_WRAP_IS_PROMPT (self));
	g_assert (pin);
	g_assert (n_pin);

	prepare_unlock_prompt (self, *pin == NULL);

	gku_prompt_request_attention_sync (NULL, on_prompt_attention,
	                                   g_object_ref (self), g_object_unref);

	if (gku_prompt_get_response (GKU_PROMPT (self)) != GKU_RESPONSE_OK)
		return FALSE;

	g_assert (self->destroy_data == (GDestroyNotify)egg_secure_strfree);
	egg_secure_strfree (self->prompt_data);

	self->prompt_data = gku_prompt_get_password (GKU_PROMPT (self), "password");
	g_return_val_if_fail (self->prompt_data, FALSE);

	*pin = self->prompt_data;
	*n_pin = strlen (self->prompt_data);
	return TRUE;
}

static GkmWrapPrompt*
login_prompt_for_user (CK_FUNCTION_LIST_PTR module, CK_SESSION_HANDLE session)
{
	GkmWrapPrompt *self;

	g_assert (module);

	/* Build up the prompt */
	self = g_object_new (GKM_WRAP_TYPE_PROMPT, NULL);
	self->module = module;
	self->session = session;
	self->destroy_data = (GDestroyNotify)egg_secure_strfree;

	return self;
}

static gboolean
login_prompt_do_user (GkmWrapPrompt *self, CK_RV last_result,
                       CK_UTF8CHAR_PTR *pin, CK_ULONG *n_pin)
{
	CK_SESSION_INFO sinfo;
	CK_TOKEN_INFO tinfo;
	CK_RV rv;

	g_assert (GKM_WRAP_IS_PROMPT (self));
	g_assert (self->module);
	g_assert (pin);
	g_assert (n_pin);

	rv = (self->module->C_GetSessionInfo) (self->session, &sinfo);
	if (rv != CKR_OK)
		return FALSE;

	rv = (self->module->C_GetTokenInfo) (sinfo.slotID, &tinfo);
	if (rv != CKR_OK)
		return FALSE;

	/* Hard reset on first prompt, soft on later */
	gku_prompt_reset (GKU_PROMPT (self), last_result == CKR_OK);

	prepare_unlock_token (self, &tinfo);

	gku_prompt_request_attention_sync (NULL, on_prompt_attention,
	                                   g_object_ref (self), g_object_unref);

	if (gku_prompt_get_response (GKU_PROMPT (self)) != GKU_RESPONSE_OK)
		return FALSE;

	g_assert (self->destroy_data == (GDestroyNotify)egg_secure_strfree);
	egg_secure_strfree (self->prompt_data);

	self->prompt_data = gku_prompt_get_password (GKU_PROMPT (self), "password");
	g_return_val_if_fail (self->prompt_data, FALSE);

	*pin = self->prompt_data;
	*n_pin = strlen (self->prompt_data);
	return TRUE;
}

GkmWrapPrompt*
gkm_wrap_prompt_for_login (CK_FUNCTION_LIST_PTR module, CK_USER_TYPE user_type,
                           CK_SESSION_HANDLE session, CK_OBJECT_HANDLE object,
                           CK_UTF8CHAR_PTR pin, CK_ULONG n_pin)
{
	g_return_val_if_fail (module, NULL);

	if (pin != NULL || n_pin != 0)
		return NULL;

	switch (user_type) {
	case CKU_CONTEXT_SPECIFIC:
		return login_prompt_for_specific (module, session, object);
	case CKU_USER:
		return login_prompt_for_user (module, session);
	default:
		return NULL;
	}
}

gboolean
gkm_wrap_prompt_do_login (GkmWrapPrompt *self, CK_USER_TYPE user_type, CK_RV last_result,
                          CK_UTF8CHAR_PTR *pin, CK_ULONG *n_pin)
{
	g_return_val_if_fail (GKM_WRAP_IS_PROMPT (self), FALSE);
	g_return_val_if_fail (pin, FALSE);
	g_return_val_if_fail (n_pin, FALSE);

	switch (user_type) {
	case CKU_CONTEXT_SPECIFIC:
		return login_prompt_do_specific (self, last_result, pin, n_pin);
	case CKU_USER:
		return login_prompt_do_user (self, last_result, pin, n_pin);
	default:
		return FALSE;
	}
}

void
gkm_wrap_prompt_done_login (GkmWrapPrompt *self, CK_USER_TYPE user_type, CK_RV call_result)
{
	g_return_if_fail (GKM_WRAP_IS_PROMPT (self));

#if 0
	switch (user_type) {
	case CKU_CONTEXT_SPECIFIC:
		login_prompt_done_specific (self, call_result);
		break;
	case CKU_USER:
		login_prompt_done_user (self, call_result);
		break;
	}
#endif
}