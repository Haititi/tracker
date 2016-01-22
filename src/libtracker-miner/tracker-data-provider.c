/*
 * Copyright (C) 2014, Softathome <contact@softathome.com>
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
 *
 * Author: Martyn Russell <martyn@lanedo.com>
 */

#include "config.h"

#include <glib/gi18n.h>

#include "tracker-data-provider.h"

/**
 * SECTION:tracker-data-provider
 * @short_description: Provide data to be indexed
 * @include: libtracker-miner/miner.h
 *
 * #TrackerDataProvider allows you to operate on a set of #GFiles,
 * returning a #GFileInfo structure for each file enumerated (e.g.
 * tracker_data_provider_begin() will return a #TrackerEnumerator
 * which can be used to enumerate resources provided by the
 * #TrackerDataProvider.
 *
 * There is also a #TrackerFileDataProvider which is an implementation
 * of this #TrackerDataProvider interface.
 *
 * The #TrackerMinerFS class which is a subclass to #TrackerMiner
 * takes a #TrackerDataProvider property which is passed down to the
 * TrackerCrawler created upon instantiation. This property is
 * #TrackerMinerFS:data-provider.
 *
 * See the #TrackerEnumerator documentation for more details.
 *
 * Since: 1.2
 **/

typedef TrackerDataProviderIface TrackerDataProviderInterface;
G_DEFINE_INTERFACE (TrackerDataProvider, tracker_data_provider, G_TYPE_OBJECT)

static void
tracker_data_provider_default_init (TrackerDataProviderInterface *iface)
{
	g_signal_new ("item-created",
	              G_TYPE_FROM_INTERFACE (iface),
	              G_SIGNAL_RUN_LAST,
	              G_STRUCT_OFFSET (TrackerDataProviderIface, item_created),
	              NULL, NULL,
	              NULL,
	              G_TYPE_NONE,
	              2,
	              G_TYPE_OBJECT,
	              G_TYPE_BOOLEAN);

	g_signal_new ("item-updated",
	              G_TYPE_FROM_INTERFACE (iface),
	              G_SIGNAL_RUN_LAST,
	              G_STRUCT_OFFSET (TrackerDataProviderIface, item_updated),
	              NULL, NULL,
	              NULL,
	              G_TYPE_NONE,
	              2,
	              G_TYPE_OBJECT,
	              G_TYPE_BOOLEAN);

	g_signal_new ("item-attribute-updated",
	              G_TYPE_FROM_INTERFACE (iface),
	              G_SIGNAL_RUN_LAST,
	              G_STRUCT_OFFSET (TrackerDataProviderIface, item_attribute_updated),
	              NULL, NULL,
	              NULL,
	              G_TYPE_NONE,
	              2,
	              G_TYPE_OBJECT,
	              G_TYPE_BOOLEAN);

	g_signal_new ("item-deleted",
	              G_TYPE_FROM_INTERFACE (iface),
	              G_SIGNAL_RUN_LAST,
	              G_STRUCT_OFFSET (TrackerDataProviderIface, item_deleted),
	              NULL, NULL,
	              NULL,
	              G_TYPE_NONE,
	              2,
	              G_TYPE_OBJECT,
	              G_TYPE_BOOLEAN);

	g_signal_new ("item-moved",
	              G_TYPE_FROM_INTERFACE (iface),
	              G_SIGNAL_RUN_LAST,
	              G_STRUCT_OFFSET (TrackerDataProviderIface, item_moved),
	              NULL, NULL,
	              NULL,
	              G_TYPE_NONE,
	              4,
	              G_TYPE_OBJECT,
	              G_TYPE_OBJECT,
	              G_TYPE_BOOLEAN,
	              G_TYPE_BOOLEAN);
}

/**
 * tracker_data_provider_begin:
 * @data_provider: a #TrackerDataProvider
 * @url: a #GFile to enumerate
 * @attributes: an attribute query string
 * @flags: a set of #TrackerDirectoryFlags
 * @cancellable: (allow-none): optional #GCancellable object, %NULL to ignore.
 * @error: location to store the error occurring, or %NULL to ignore
 *
 * Creates a #TrackerEnumerator to enumerate children at the URI
 * provided by @url.
 *
 * The attributes value is a string that specifies the file attributes
 * that should be gathered. It is not an error if it's not possible to
 * read a particular requested attribute from a file - it just won't
 * be set. attributes should be a comma-separated list of attributes
 * or attribute wildcards. The wildcard "*" means all attributes, and
 * a wildcard like "standard::*" means all attributes in the standard
 * namespace. An example attribute query be "standard::*,owner::user".
 * The standard attributes are available as defines, like
 * %G_FILE_ATTRIBUTE_STANDARD_NAME. See g_file_enumerate_children() for
 * more details.
 *
 * The @flags provided will affect whether the @data_provider
 * implementation should be setting up data monitors for changes under
 * @url. Changes <emphasis>MUST NOT</emphasis> be signalled unless
 * #TRACKER_DIRECTORY_FLAG_MONITOR is provided in @flags for @url.
 *
 * Returns: (transfer full): a #TrackerEnumerator or %NULL on failure.
 * This must be freed with g_object_unref().
 *
 * Since: 1.2
 **/
TrackerEnumerator *
tracker_data_provider_begin (TrackerDataProvider    *data_provider,
                             GFile                  *url,
                             const gchar            *attributes,
                             TrackerDirectoryFlags   flags,
                             GCancellable           *cancellable,
                             GError                **error)
{
	TrackerDataProviderIface *iface;

	g_return_val_if_fail (TRACKER_IS_DATA_PROVIDER (data_provider), NULL);

	if (g_cancellable_set_error_if_cancelled (cancellable, error)) {
		return NULL;
	}

	iface = TRACKER_DATA_PROVIDER_GET_IFACE (data_provider);

	if (iface->begin == NULL) {
		g_set_error_literal (error,
		                     G_IO_ERROR,
		                     G_IO_ERROR_NOT_SUPPORTED,
		                     _("Operation not supported"));
		return NULL;
	}

	return (* iface->begin) (data_provider, url, attributes, flags, cancellable, error);
}

/**
 * tracker_data_provider_begin_async:
 * @data_provider: a #TrackerDataProvider.
 * @url: a #GFile to enumerate
 * @attributes: an attribute query string
 * @flags: a set of #TrackerDirectoryFlags
 * @io_priority: the [I/O priority][io-priority] of the request
 * @cancellable: (allow-none): optional #GCancellable object, %NULL to
 * ignore
 * @callback: (scope async): a #GAsyncReadyCallback to call when the
 * request is satisfied
 * @user_data: (closure): the data to pass to callback function
 *
 * Precisely the same operation as tracker_data_provider_begin()
 * is performing, but asynchronously.
 *
 * When all i/o for the operation is finished the @callback will be
 * called with the requested information.

 * See the documentation of #TrackerDataProvider for information about the
 * order of returned files.
 *
 * In case of a partial error the callback will be called with any
 * succeeding items and no error, and on the next request the error
 * will be reported. If a request is cancelled the callback will be
 * called with #G_IO_ERROR_CANCELLED.
 *
 * During an async request no other sync and async calls are allowed,
 * and will result in #G_IO_ERROR_PENDING errors.
 *
 * Any outstanding i/o request with higher priority (lower numerical
 * value) will be executed before an outstanding request with lower
 * priority. Default priority is %G_PRIORITY_DEFAULT.
 *
 * Since: 1.2
 **/
void
tracker_data_provider_begin_async (TrackerDataProvider   *data_provider,
                                   GFile                 *url,
                                   const gchar           *attributes,
                                   TrackerDirectoryFlags  flags,
                                   int                    io_priority,
                                   GCancellable          *cancellable,
                                   GAsyncReadyCallback    callback,
                                   gpointer               user_data)
{
	TrackerDataProviderIface *iface;

	g_return_if_fail (TRACKER_IS_DATA_PROVIDER (data_provider));

	iface = TRACKER_DATA_PROVIDER_GET_IFACE (data_provider);

	if (iface->begin_async == NULL) {
		g_critical (_("Operation not supported"));
		return;
	}

	(* iface->begin_async) (data_provider, url, attributes, flags, io_priority, cancellable, callback, user_data);
}

/**
 * tracker_data_provider_begin_finish:
 * @data_provider: a #TrackerDataProvider.
 * @result: a #GAsyncResult.
 * @error: a #GError location to store the error occurring, or %NULL
 * to ignore.
 *
 * Finishes the asynchronous operation started with
 * tracker_data_provider_begin_async().
 *
 * Returns: (transfer full): a #TrackerEnumerator or %NULL on failure.
 * This must be freed with g_object_unref().
 *
 * Since: 1.2
 **/
TrackerEnumerator *
tracker_data_provider_begin_finish (TrackerDataProvider  *data_provider,
                                    GAsyncResult         *result,
                                    GError              **error)
{
	TrackerDataProviderIface *iface;

	g_return_val_if_fail (TRACKER_IS_DATA_PROVIDER (data_provider), NULL);
	g_return_val_if_fail (G_IS_ASYNC_RESULT (result), NULL);

	iface = TRACKER_DATA_PROVIDER_GET_IFACE (data_provider);

	if (g_async_result_legacy_propagate_error (result, error)) {
		return NULL;
	}

	return (* iface->begin_finish) (data_provider, result, error);
}

/**
 * tracker_data_provider_end:
 * @data_provider: a #TrackerDataProvider
 * @enumerator: a #TrackerEnumerator originally created by
 * tracker_data_provider_begin().
 * @cancellable: (allow-none): optional #GCancellable object, %NULL to ignore.
 * @error: location to store the error occurring, or %NULL to ignore
 *
 * Closes any caches or operations related to creating the
 * #TrackerEnumerator to enumerate data at @url.
 *
 * The attributes value is a string that specifies the file attributes
 * that should be gathered. It is not an error if it's not possible to
 * read a particular requested attribute from a file - it just won't
 * be set. attributes should be a comma-separated list of attributes
 * or attribute wildcards. The wildcard "*" means all attributes, and
 * a wildcard like "standard::*" means all attributes in the standard
 * namespace. An example attribute query be "standard::*,owner::user".
 * The standard attributes are available as defines, like
 * G_FILE_ATTRIBUTE_STANDARD_NAME. See g_file_enumerate_children() for
 * more details.
 *
 * Returns: %TRUE on success, otherwise %FALSE and @error is set.
 *
 * Since: 1.2
 **/
gboolean
tracker_data_provider_end (TrackerDataProvider  *data_provider,
                           TrackerEnumerator    *enumerator,
                           GCancellable         *cancellable,
                           GError              **error)
{
	TrackerDataProviderIface *iface;

	g_return_val_if_fail (TRACKER_IS_DATA_PROVIDER (data_provider), FALSE);
	g_return_val_if_fail (TRACKER_IS_ENUMERATOR (enumerator), FALSE);

	if (g_cancellable_set_error_if_cancelled (cancellable, error)) {
		return FALSE;
	}

	iface = TRACKER_DATA_PROVIDER_GET_IFACE (data_provider);

	if (iface->end == NULL) {
		g_set_error_literal (error,
		                     G_IO_ERROR,
		                     G_IO_ERROR_NOT_SUPPORTED,
		                     _("Operation not supported"));
		return FALSE;
	}

	return (* iface->end) (data_provider, enumerator, cancellable, error);
}

/**
 * tracker_data_provider_end_async:
 * @data_provider: a #TrackerDataProvider.
 * @enumerator: a #TrackerEnumerator originally created by
 * tracker_data_provider_begin().
 * @io_priority: the [I/O priority][io-priority] of the request
 * @cancellable: (allow-none): optional #GCancellable object, %NULL to
 * ignore
 * @callback: (scope async): a #GAsyncReadyCallback to call when the
 * request is satisfied
 * @user_data: (closure): the data to pass to callback function
 *
 * Precisely the same operation as tracker_data_provider_end()
 * is performing, but asynchronously.
 *
 * When all i/o for the operation is finished the @callback will be
 * called with the requested information.

 * See the documentation of #TrackerDataProvider for information about the
 * order of returned files.
 *
 * In case of a partial error the callback will be called with any
 * succeeding items and no error, and on the next request the error
 * will be reported. If a request is cancelled the callback will be
 * called with %G_IO_ERROR_CANCELLED.
 *
 * During an async request no other sync and async calls are allowed,
 * and will result in %G_IO_ERROR_PENDING errors.
 *
 * Any outstanding i/o request with higher priority (lower numerical
 * value) will be executed before an outstanding request with lower
 * priority. Default priority is %G_PRIORITY_DEFAULT.
 *
 * Since: 1.2
 **/
void
tracker_data_provider_end_async (TrackerDataProvider  *data_provider,
                                 TrackerEnumerator    *enumerator,
                                 int                   io_priority,
                                 GCancellable         *cancellable,
                                 GAsyncReadyCallback   callback,
                                 gpointer              user_data)
{
	TrackerDataProviderIface *iface;

	g_return_if_fail (TRACKER_IS_DATA_PROVIDER (data_provider));
	g_return_if_fail (TRACKER_IS_ENUMERATOR (enumerator));

	iface = TRACKER_DATA_PROVIDER_GET_IFACE (data_provider);

	if (iface->end_async == NULL) {
		g_critical (_("Operation not supported"));
		return;
	}

	(* iface->end_async) (data_provider, enumerator, io_priority, cancellable, callback, user_data);
}

/**
 * tracker_data_provider_end_finish:
 * @data_provider: a #TrackerDataProvider.
 * @result: a #GAsyncResult.
 * @error: a #GError location to store the error occurring, or %NULL
 * to ignore.
 *
 * Finishes the asynchronous operation started with
 * tracker_data_provider_end_async().
 *
 * Returns: %TRUE on success, otherwise %FALSE and @error is set.
 *
 * Since: 1.2
 **/
gboolean
tracker_data_provider_end_finish (TrackerDataProvider  *data_provider,
                                  GAsyncResult         *result,
                                  GError              **error)
{
	TrackerDataProviderIface *iface;

	g_return_val_if_fail (TRACKER_IS_DATA_PROVIDER (data_provider), FALSE);
	g_return_val_if_fail (G_IS_ASYNC_RESULT (result), FALSE);

	iface = TRACKER_DATA_PROVIDER_GET_IFACE (data_provider);

	if (g_async_result_legacy_propagate_error (result, error)) {
		return FALSE;
	}

	return (* iface->end_finish) (data_provider, result, error);
}

/**
 * tracker_data_provider_monitor_add:
 * @data_provider: a #TrackerDataProvider
 * @container: a #GFile
 * @error: location to store the error occurring, or %NULL to ignore
 *
 * Tells @data_provider to monitor changes on @container. This is very
 * similar to the g_file_monitor_file() API. This is called depending
 * on the flags passed to tracker_data_provider_begin() and
 * tracker_data_provider_begin_async() where
 * #TRACKER_DIRECTORY_FLAG_MONITOR can be passed.
 *
 * Returns: %TRUE on success, otherwise %FALSE and @error is set.
 *
 * Since: 1.4
 **/
gboolean
tracker_data_provider_monitor_add (TrackerDataProvider  *data_provider,
                                   GFile                *container,
                                   GError              **error)
{
	TrackerDataProviderIface *iface;

	g_return_val_if_fail (TRACKER_IS_DATA_PROVIDER (data_provider), FALSE);
	g_return_val_if_fail (G_IS_FILE (container), FALSE);

	iface = TRACKER_DATA_PROVIDER_GET_IFACE (data_provider);

	if (iface->monitor_add == NULL) {
		g_set_error_literal (error,
		                     G_IO_ERROR,
		                     G_IO_ERROR_NOT_SUPPORTED,
		                     _("Operation not supported"));
		return FALSE;
	}

	return (* iface->monitor_add) (data_provider, container, error);
}

/**
 * tracker_data_provider_monitor_remove:
 * @data_provider: a #TrackerDataProvider
 * @container: a #GFile
 * @recursively: Remove monitors on all containers below @container too
 * @children_only: if @recursively is %TRUE, Remove all monitors but @container
 * @error: location to store the error occurring, or %NULL to ignore
 *
 * Tells @data_provider to stop monitoring changes on @container. This
 * is very similar to the g_file_monitor_file() API. This may be called
 * depending on the flags passed to tracker_data_provider_begin() and
 * tracker_data_provider_begin_async() where
 * #TRACKER_DIRECTORY_FLAG_MONITOR can be passed.
 *
 * Returns: %TRUE on success, otherwise %FALSE and @error is set.
 *
 * Since: 1.4
 **/
gboolean
tracker_data_provider_monitor_remove (TrackerDataProvider  *data_provider,
                                      GFile                *container,
                                      gboolean              recursively,
                                      gboolean              children_only,
                                      GError              **error)
{
	TrackerDataProviderIface *iface;

	g_return_val_if_fail (TRACKER_IS_DATA_PROVIDER (data_provider), FALSE);
	g_return_val_if_fail (G_IS_FILE (container), FALSE);

	iface = TRACKER_DATA_PROVIDER_GET_IFACE (data_provider);

	if (iface->monitor_remove == NULL) {
		g_set_error_literal (error,
		                     G_IO_ERROR,
		                     G_IO_ERROR_NOT_SUPPORTED,
		                     _("Operation not supported"));
		return FALSE;
	}

	return (* iface->monitor_remove) (data_provider, container, recursively, children_only, error);
}

/**
 * tracker_data_provider_monitor_move:
 * @data_provider: a #TrackerDataProvider
 * @container_from: a #GFile
 * @container_to: a #GFile
 * @error: location to store the error occurring, or %NULL to ignore
 *
 * Tells @data_provider to move @container_from to @container_to.
 * Typically used for file systems where a folder or directory is
 * moved.
 *
 * Returns: %TRUE on success, otherwise %FALSE and @error is set.
 *
 * Since: 1.4
 **/
gboolean
tracker_data_provider_monitor_move (TrackerDataProvider  *data_provider,
                                    GFile                *container_from,
                                    GFile                *container_to,
                                    GError              **error)
{
	TrackerDataProviderIface *iface;

	g_return_val_if_fail (TRACKER_IS_DATA_PROVIDER (data_provider), FALSE);
	g_return_val_if_fail (G_IS_FILE (container_from), FALSE);
	g_return_val_if_fail (G_IS_FILE (container_to), FALSE);

	iface = TRACKER_DATA_PROVIDER_GET_IFACE (data_provider);

	if (iface->monitor_move == NULL) {
		g_set_error_literal (error,
		                     G_IO_ERROR,
		                     G_IO_ERROR_NOT_SUPPORTED,
		                     _("Operation not supported"));
		return FALSE;
	}

	return (* iface->monitor_move) (data_provider, container_from, container_to, error);
}

/**
 * tracker_data_provider_is_monitored:
 * @data_provider: a #TrackerDataProvider
 * @container: a #GFile
 * @error: location to store the error occurring, or %NULL to ignore
 *
 * Asks if @data_provider is monitoring changes to @container.
 *
 * Returns: %TRUE on success, otherwise %FALSE and @error is set.
 *
 * Since: 1.4
 **/
gboolean
tracker_data_provider_is_monitored (TrackerDataProvider  *data_provider,
                                    GFile                *container,
                                    GError              **error)
{
	TrackerDataProviderIface *iface;

	g_return_val_if_fail (TRACKER_IS_DATA_PROVIDER (data_provider), FALSE);
	g_return_val_if_fail (G_IS_FILE (container), FALSE);

	iface = TRACKER_DATA_PROVIDER_GET_IFACE (data_provider);

	if (iface->is_monitored == NULL) {
		g_set_error_literal (error,
		                     G_IO_ERROR,
		                     G_IO_ERROR_NOT_SUPPORTED,
		                     _("Operation not supported"));
		return FALSE;
	}

	return (* iface->is_monitored) (data_provider, container, error);
}

/**
 * tracker_data_provider_is_monitored_by_path:
 * @data_provider: a #TrackerDataProvider
 * @container: a string
 * @error: location to store the error occurring, or %NULL to ignore
 *
 * The same as tracker_data_provider_is_monitored() but takes a string
 * to represent the path instead.
 *
 * Returns: %TRUE on success, otherwise %FALSE and @error is set.
 *
 * Since: 1.4
 **/
gboolean
tracker_data_provider_is_monitored_by_path (TrackerDataProvider  *data_provider,
                                            const gchar          *container,
                                            GError              **error)
{
	TrackerDataProviderIface *iface;

	g_return_val_if_fail (TRACKER_IS_DATA_PROVIDER (data_provider), FALSE);
	g_return_val_if_fail (container != NULL, FALSE);

	iface = TRACKER_DATA_PROVIDER_GET_IFACE (data_provider);

	if (iface->is_monitored_by_path == NULL) {
		g_set_error_literal (error,
		                     G_IO_ERROR,
		                     G_IO_ERROR_NOT_SUPPORTED,
		                     _("Operation not supported"));
		return FALSE;
	}

	return (* iface->is_monitored_by_path) (data_provider, container, error);
}

/**
 * tracker_data_provider_monitor_count:
 * @data_provider: a #TrackerDataProvider
 * @error: location to store the error occurring, or %NULL to ignore
 *
 * Asks the @data_provider how many containers are being monitored.
 *
 * Returns: A value of 0 or higher on success, otherwise @error is set.
 *
 * Since: 1.4
 **/
guint
tracker_data_provider_monitor_count (TrackerDataProvider  *data_provider,
                                     GError              **error)
{
	TrackerDataProviderIface *iface;

	g_return_val_if_fail (TRACKER_IS_DATA_PROVIDER (data_provider), 0);

	iface = TRACKER_DATA_PROVIDER_GET_IFACE (data_provider);

	if (iface->monitor_count == NULL) {
		g_set_error_literal (error,
		                     G_IO_ERROR,
		                     G_IO_ERROR_NOT_SUPPORTED,
		                     _("Operation not supported"));
		return FALSE;
	}

	return (* iface->monitor_count) (data_provider, error);
}

/**
 * tracker_data_provider_set_indexing_tree:
 * @data_provider: a #TrackerDataProvider
 * @indexing_tree: a #TrackerIndexingTree
 * @error: location to store the error occurring, or %NULL to ignore
 *
 * Tells @data_provider to use @indexing_tree. This is used by
 * implementations to know what the rules are for ignoring content
 * (among other things).
 *
 * Returns: %TRUE on success, otherwise %FALSE and @error is set.
 *
 * Since: 1.4
 **/
gboolean
tracker_data_provider_set_indexing_tree (TrackerDataProvider  *data_provider,
                                         TrackerIndexingTree  *indexing_tree,
                                         GError              **error)
{
	TrackerDataProviderIface *iface;

	g_return_val_if_fail (TRACKER_IS_DATA_PROVIDER (data_provider), FALSE);
	g_return_val_if_fail (TRACKER_IS_INDEXING_TREE (indexing_tree), FALSE);

	iface = TRACKER_DATA_PROVIDER_GET_IFACE (data_provider);

	if (iface->set_indexing_tree == NULL) {
		g_set_error_literal (error,
		                     G_IO_ERROR,
		                     G_IO_ERROR_NOT_SUPPORTED,
		                     _("Operation not supported"));
		return FALSE;
	}

	return (* iface->set_indexing_tree) (data_provider, indexing_tree, error);
}

/**
 * tracker_data_provider_get_indexing_tree:
 * @data_provider: a #TrackerDataProvider
 * @error: location to store the error occurring, or %NULL to ignore
 *
 * Return the #TrackerIndexingTree that @data_provider is using. For
 * details about why this is needed by @data_provider, see
 * tracker_data_provider_set_indexing_tree().
 *
 * Returns: (transfer none): A #TrackerIndexingTree, otherwise %NULL and
 * @error is set.
 *
 * Since: 1.4
 **/
TrackerIndexingTree *
tracker_data_provider_get_indexing_tree (TrackerDataProvider  *data_provider,
                                         GError              **error)
{
	TrackerDataProviderIface *iface;

	g_return_val_if_fail (TRACKER_IS_DATA_PROVIDER (data_provider), NULL);

	iface = TRACKER_DATA_PROVIDER_GET_IFACE (data_provider);

	if (iface->get_indexing_tree == NULL) {
		g_set_error_literal (error,
		                     G_IO_ERROR,
		                     G_IO_ERROR_NOT_SUPPORTED,
		                     _("Operation not supported"));
		return NULL;
	}

	return (* iface->get_indexing_tree) (data_provider, error);
}
