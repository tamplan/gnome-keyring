/*
 * gnome-keyring
 *
 * Copyright (C) 2014 Stef Walter
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
 *
 * Author: Stef Walter <stef@thewalter.net>
 */

#ifndef __GKD_SSH_AGENT_CLIENT_H__
#define __GKD_SSH_AGENT_CLIENT_H__

#include <glib.h>

gint                 gkd_ssh_agent_client_connect             (void);

void                 gkd_ssh_agent_client_cleanup             (void);

#endif /* __GKD_SSH_AGENT_CLIENT_H__ */