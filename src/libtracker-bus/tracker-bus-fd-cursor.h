/*
 * Copyright (C) 2010, Nokia <ivan.frade@nokia.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA  02110-1301, USA.
 */

#ifndef __LIBTRACKER_BUS_FD_CURSOR_H__
#define __LIBTRACKER_BUS_FD_CURSOR_H__

#include <glib.h>
#include <gio/gio.h>

#include <dbus/dbus-glib.h>

#include <libtracker-sparql/tracker-sparql.h>

G_BEGIN_DECLS

void
tracker_bus_fd_query_async (DBusGConnection     *gconnection,
                            const gchar         *query,
                            GCancellable        *cancellable,
                            GAsyncReadyCallback  callback,
                            gpointer             user_data);

TrackerSparqlCursor *
tracker_bus_fd_query_finish (GAsyncResult     *res,
                             GError          **error);

G_END_DECLS

#endif
