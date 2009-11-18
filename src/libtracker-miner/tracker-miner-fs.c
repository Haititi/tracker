/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*- */
/*
 * Copyright (C) 2009, Nokia (urho.konttori@nokia.com)
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

#include "config.h"

#include <libtracker-common/tracker-dbus.h>
#include <libtracker-common/tracker-file-utils.h>

#include "tracker-crawler.h"
#include "tracker-marshal.h"
#include "tracker-miner-fs.h"
#include "tracker-monitor.h"
#include "tracker-utils.h"

/**
 * SECTION:tracker-miner-fs
 * @short_description: Abstract base class for filesystem miners
 * @include: libtracker-miner/tracker-miner-fs.h
 *
 * #TrackerMinerFS is an abstract base class for miners that collect data
 * from the filesystem, all the filesystem crawling and monitoring is
 * abstracted away, leaving to implementations the decisions of what
 * directories/files should it process, and the actual data extraction.
 **/

#define TRACKER_MINER_FS_GET_PRIVATE(o) (G_TYPE_INSTANCE_GET_PRIVATE ((o), TRACKER_TYPE_MINER_FS, TrackerMinerFSPrivate))

typedef struct {
	GFile *file;
	GFile *source_file;
} ItemMovedData;

typedef struct {
	GFile    *file;
	gboolean  recurse;
} DirectoryData;

typedef struct {
	GFile *file;
	GCancellable *cancellable;
	TrackerSparqlBuilder *builder;
} ProcessData;

typedef struct {
	GMainLoop *main_loop;
	gboolean value;
} SparqlQueryData;

typedef struct {
	GMainLoop *main_loop;
	gint       level;
	GString   *sparql;
	const gchar *source_uri;
	const gchar *uri;
} RecursiveMoveData;

struct TrackerMinerFSPrivate {
	TrackerMonitor *monitor;
	TrackerCrawler *crawler;

	/* File queues for indexer */
	GQueue         *items_created;
	GQueue         *items_updated;
	GQueue         *items_deleted;
	GQueue         *items_moved;

	GQuark          quark_ignore_file;

	GList          *directories;
	DirectoryData  *current_directory;

	GTimer	       *timer;

	guint           crawl_directories_id;
	guint		item_queues_handler_id;

	gdouble         throttle;

	GList          *processing_pool;
	guint           pool_limit;

	/* Status */
	guint           been_started : 1;
	guint           been_crawled : 1;
	guint           shown_totals : 1;
	guint           is_paused : 1;
	guint           is_crawling : 1;

	/* Statistics */
	guint		total_directories_found;
	guint		total_directories_ignored;
	guint		total_files_found;
	guint		total_files_ignored;

	guint		directories_found;
	guint		directories_ignored;
	guint		files_found;
	guint		files_ignored;
};

enum {
	QUEUE_NONE,
	QUEUE_CREATED,
	QUEUE_UPDATED,
	QUEUE_DELETED,
	QUEUE_MOVED
};

enum {
	CHECK_FILE,
	CHECK_DIRECTORY,
	CHECK_DIRECTORY_CONTENTS,
	MONITOR_DIRECTORY,
	PROCESS_FILE,
	FINISHED,
	LAST_SIGNAL
};

enum {
	PROP_0,
	PROP_THROTTLE,
	PROP_POOL_LIMIT
};

static void           fs_finalize                  (GObject        *object);
static void           fs_set_property              (GObject        *object,
						    guint           prop_id,
						    const GValue   *value,
						    GParamSpec     *pspec);
static void           fs_get_property              (GObject        *object,
						    guint           prop_id,
						    GValue         *value,
						    GParamSpec     *pspec);

static gboolean       fs_defaults                  (TrackerMinerFS *fs,
						    GFile          *file);
static gboolean       fs_contents_defaults         (TrackerMinerFS *fs,
						    GFile          *parent,
						    GList          *children);
static void           miner_started                (TrackerMiner   *miner);
static void           miner_stopped                (TrackerMiner   *miner);
static void           miner_paused                 (TrackerMiner   *miner);
static void           miner_resumed                (TrackerMiner   *miner);

static DirectoryData *directory_data_new           (GFile          *file,
						    gboolean        recurse);
static void           directory_data_free          (DirectoryData  *dd);
static ItemMovedData *item_moved_data_new          (GFile          *file,
						    GFile          *source_file);
static void           item_moved_data_free         (ItemMovedData  *data);
static void           monitor_item_created_cb      (TrackerMonitor *monitor,
						    GFile          *file,
						    gboolean        is_directory,
						    gpointer        user_data);
static void           monitor_item_updated_cb      (TrackerMonitor *monitor,
						    GFile          *file,
						    gboolean        is_directory,
						    gpointer        user_data);
static void           monitor_item_deleted_cb      (TrackerMonitor *monitor,
						    GFile          *file,
						    gboolean        is_directory,
						    gpointer        user_data);
static void           monitor_item_moved_cb        (TrackerMonitor *monitor,
						    GFile          *file,
						    GFile          *other_file,
						    gboolean        is_directory,
						    gboolean        is_source_monitored,
						    gpointer        user_data);
static gboolean       crawler_check_file_cb        (TrackerCrawler *crawler,
						    GFile          *file,
						    gpointer        user_data);
static gboolean       crawler_check_directory_cb   (TrackerCrawler *crawler,
						    GFile          *file,
						    gpointer        user_data);
static gboolean       crawler_check_directory_contents_cb (TrackerCrawler *crawler,
							   GFile          *parent,
							   GList          *children,
							   gpointer        user_data);
static void           crawler_finished_cb          (TrackerCrawler *crawler,
						    GQueue         *found,
						    gboolean        was_interrupted,
						    guint           directories_found,
						    guint           directories_ignored,
						    guint           files_found,
						    guint           files_ignored,
						    gpointer        user_data);
static void           crawl_directories_start      (TrackerMinerFS *fs);
static void           crawl_directories_stop       (TrackerMinerFS *fs);

static void           item_queue_handlers_set_up   (TrackerMinerFS *fs);

static void           item_update_uri_recursively (TrackerMinerFS    *fs,
						   RecursiveMoveData *data,
						   const gchar       *source_uri,
						   const gchar       *uri);

static guint signals[LAST_SIGNAL] = { 0, };

G_DEFINE_ABSTRACT_TYPE (TrackerMinerFS, tracker_miner_fs, TRACKER_TYPE_MINER)

static void
tracker_miner_fs_class_init (TrackerMinerFSClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS (klass);
        TrackerMinerFSClass *fs_class = TRACKER_MINER_FS_CLASS (klass);
        TrackerMinerClass *miner_class = TRACKER_MINER_CLASS (klass);

	object_class->finalize = fs_finalize;
	object_class->set_property = fs_set_property;
	object_class->get_property = fs_get_property;

        miner_class->started = miner_started;
        miner_class->stopped = miner_stopped;
	miner_class->paused  = miner_paused;
	miner_class->resumed = miner_resumed;

	fs_class->check_file        = fs_defaults;
	fs_class->check_directory   = fs_defaults;
	fs_class->monitor_directory = fs_defaults;
	fs_class->check_directory_contents = fs_contents_defaults;

	g_object_class_install_property (object_class,
					 PROP_THROTTLE,
					 g_param_spec_double ("throttle",
							      "Throttle",
							      "Modifier for the indexing speed, 0 is max speed",
							      0, 1, 0,
							      G_PARAM_READWRITE));
	g_object_class_install_property (object_class,
					 PROP_POOL_LIMIT,
					 g_param_spec_uint ("process-pool-limit",
							    "Processing pool limit",
							    "Number of files that can be concurrently processed",
							    1, G_MAXUINT, 1,
							    G_PARAM_READWRITE | G_PARAM_CONSTRUCT));
	/**
	 * TrackerMinerFS::check-file:
	 * @miner_fs: the #TrackerMinerFS
	 * @file: a #GFile
	 *
	 * The ::check-file signal is emitted either on the filesystem crawling
	 * phase or whenever a new file appears in a monitored directory
	 * in order to check whether @file must be inspected my @miner_fs.
	 *
	 * Returns: %TRUE if @file must be inspected.
	 **/
	signals[CHECK_FILE] =
		g_signal_new ("check-file",
			      G_OBJECT_CLASS_TYPE (object_class),
			      G_SIGNAL_RUN_LAST,
			      G_STRUCT_OFFSET (TrackerMinerFSClass, check_file),
			      tracker_accumulator_check_file,
			      NULL,
			      tracker_marshal_BOOLEAN__OBJECT,
			      G_TYPE_BOOLEAN, 1, G_TYPE_FILE);
	/**
	 * TrackerMinerFS::check-directory:
	 * @miner_fs: the #TrackerMinerFS
	 * @directory: a #GFile
	 *
	 * The ::check-directory signal is emitted either on the filesystem crawling
	 * phase or whenever a new directory appears in a monitored directory
	 * in order to check whether @directory must be inspected my @miner_fs.
	 *
	 * Returns: %TRUE if @directory must be inspected.
	 **/
	signals[CHECK_DIRECTORY] =
		g_signal_new ("check-directory",
			      G_OBJECT_CLASS_TYPE (object_class),
			      G_SIGNAL_RUN_LAST,
			      G_STRUCT_OFFSET (TrackerMinerFSClass, check_directory),
			      tracker_accumulator_check_file,
			      NULL,
			      tracker_marshal_BOOLEAN__OBJECT,
			      G_TYPE_BOOLEAN, 1, G_TYPE_FILE);
	/**
	 * TrackerMinerFS::check-directory-contents:
	 * @miner_fs: the #TrackerMinerFS
	 * @directory: a #GFile
	 * @children: #GList of #GFile<!-- -->s
	 *
	 * The ::check-directory-contents signal is emitted either on the filesystem
	 * crawling phase or whenever a new directory appears in a monitored directory
	 * in order to check whether @directory must be inspected my @miner_fs based on
	 * the directory contents, for some implementations this signal may be useful
	 * to discard backup directories for example.
	 *
	 * Returns: %TRUE if @directory must be inspected.
	 **/
	signals[CHECK_DIRECTORY_CONTENTS] =
		g_signal_new ("check-directory-contents",
			      G_OBJECT_CLASS_TYPE (object_class),
			      G_SIGNAL_RUN_LAST,
			      G_STRUCT_OFFSET (TrackerMinerFSClass, check_directory_contents),
			      tracker_accumulator_check_file,
			      NULL,
			      tracker_marshal_BOOLEAN__OBJECT_POINTER,
			      G_TYPE_BOOLEAN, 2, G_TYPE_FILE, G_TYPE_POINTER);
	/**
	 * TrackerMinerFS::monitor-directory:
	 * @miner_fs: the #TrackerMinerFS
	 * @directory: a #GFile
	 *
	 * The ::monitor-directory is emitted either on the filesystem crawling phase
	 * or whenever a new directory appears in a monitored directory in order to
	 * check whether @directory must be monitored for filesystem changes or not.
	 *
	 * Returns: %TRUE if the directory must be monitored for changes.
	 **/
	signals[MONITOR_DIRECTORY] =
		g_signal_new ("monitor-directory",
			      G_OBJECT_CLASS_TYPE (object_class),
			      G_SIGNAL_RUN_LAST,
			      G_STRUCT_OFFSET (TrackerMinerFSClass, monitor_directory),
			      tracker_accumulator_check_file,
			      NULL,
			      tracker_marshal_BOOLEAN__OBJECT,
			      G_TYPE_BOOLEAN, 1, G_TYPE_FILE);
	/**
	 * TrackerMinerFS::process-file:
	 * @miner_fs: the #TrackerMinerFS
	 * @file: a #GFile
	 * @builder: a #TrackerSparqlBuilder
	 * @cancellable: a #GCancellable
	 *
	 * The ::process-file signal is emitted whenever a file should
	 * be processed, and it's metadata extracted.
	 *
	 * @builder is the #TrackerSparqlBuilder where all sparql updates
	 * to be performed for @file will be appended.
	 *
	 * This signal allows both synchronous and asynchronous extraction,
	 * in the synchronous case @cancellable can be safely ignored. In
	 * either case, on successful metadata extraction, implementations
	 * must call tracker_miner_fs_notify_file() to indicate that
	 * processing has finished on @file, so the miner can execute
	 * the SPARQL updates and continue processing other files.
	 *
	 * Returns: %TRUE if the file is accepted for processing,
	 *          %FALSE if the file should be ignored.
	 **/
	signals[PROCESS_FILE] =
		g_signal_new ("process-file",
			      G_OBJECT_CLASS_TYPE (object_class),
			      G_SIGNAL_RUN_LAST,
			      G_STRUCT_OFFSET (TrackerMinerFSClass, process_file),
			      NULL, NULL,
			      tracker_marshal_BOOLEAN__OBJECT_OBJECT_OBJECT,
			      G_TYPE_BOOLEAN,
			      3, G_TYPE_FILE, TRACKER_TYPE_SPARQL_BUILDER, G_TYPE_CANCELLABLE),
	/**
	 * TrackerMinerFS::finished:
	 * @miner_fs: the #TrackerMinerFS
	 * @elapsed: elapsed time since mining was started
	 * @directories_found: number of directories found
	 * @directories_ignored: number of ignored directories
	 * @files_found: number of files found
	 * @files_ignored: number of ignored files
	 *
	 * The ::finished signal is emitted when @miner_fs has finished
	 * all pending processing.
	 **/
	signals[FINISHED] =
		g_signal_new ("finished",
			      G_TYPE_FROM_CLASS (object_class),
			      G_SIGNAL_RUN_LAST,
			      G_STRUCT_OFFSET (TrackerMinerFSClass, finished),
			      NULL, NULL,
			      tracker_marshal_VOID__DOUBLE_UINT_UINT_UINT_UINT,
			      G_TYPE_NONE,
			      5,
			      G_TYPE_DOUBLE,
			      G_TYPE_UINT,
			      G_TYPE_UINT,
			      G_TYPE_UINT,
			      G_TYPE_UINT);

	g_type_class_add_private (object_class, sizeof (TrackerMinerFSPrivate));
}

static void
tracker_miner_fs_init (TrackerMinerFS *object)
{
	TrackerMinerFSPrivate *priv;

	object->private = TRACKER_MINER_FS_GET_PRIVATE (object);

	priv = object->private;

	/* For each module we create a TrackerCrawler and keep them in
	 * a hash table to look up.
	 */
	priv->items_created = g_queue_new ();
	priv->items_updated = g_queue_new ();
	priv->items_deleted = g_queue_new ();
	priv->items_moved = g_queue_new ();

	/* Set up the crawlers now we have config and hal */
	priv->crawler = tracker_crawler_new ();

	g_signal_connect (priv->crawler, "check-file",
			  G_CALLBACK (crawler_check_file_cb),
			  object);
	g_signal_connect (priv->crawler, "check-directory",
			  G_CALLBACK (crawler_check_directory_cb),
			  object);
	g_signal_connect (priv->crawler, "check-directory-contents",
			  G_CALLBACK (crawler_check_directory_contents_cb),
			  object);
	g_signal_connect (priv->crawler, "finished",
			  G_CALLBACK (crawler_finished_cb),
			  object);

	/* Set up the monitor */
	priv->monitor = tracker_monitor_new ();

	g_signal_connect (priv->monitor, "item-created",
			  G_CALLBACK (monitor_item_created_cb),
			  object);
	g_signal_connect (priv->monitor, "item-updated",
			  G_CALLBACK (monitor_item_updated_cb),
			  object);
	g_signal_connect (priv->monitor, "item-deleted",
			  G_CALLBACK (monitor_item_deleted_cb),
			  object);
	g_signal_connect (priv->monitor, "item-moved",
			  G_CALLBACK (monitor_item_moved_cb),
			  object);

	priv->quark_ignore_file = g_quark_from_static_string ("tracker-ignore-file");
}

static ProcessData *
process_data_new (GFile                *file,
		  GCancellable         *cancellable,
		  TrackerSparqlBuilder *builder)
{
	ProcessData *data;

	data = g_slice_new0 (ProcessData);
	data->file = g_object_ref (file);

	if (cancellable) {
		data->cancellable = g_object_ref (cancellable);
	}

	if (builder) {
		data->builder = g_object_ref (builder);
	}

	return data;
}

static void
process_data_free (ProcessData *data)
{
	g_object_unref (data->file);

	if (data->cancellable) {
		g_object_unref (data->cancellable);
	}

	if (data->builder) {
		g_object_unref (data->builder);
	}

	g_slice_free (ProcessData, data);
}

static ProcessData *
process_data_find (TrackerMinerFS *fs,
		   GFile          *file)
{
	GList *l;

	for (l = fs->private->processing_pool; l; l = l->next) {
		ProcessData *data = l->data;

		if (g_file_equal (data->file, file)) {
			return data;
		}
	}

	return NULL;
}

static void
fs_finalize (GObject *object)
{
	TrackerMinerFSPrivate *priv;

	priv = TRACKER_MINER_FS_GET_PRIVATE (object);

	if (priv->timer) {
		g_timer_destroy (priv->timer);
	}

	if (priv->item_queues_handler_id) {
		g_source_remove (priv->item_queues_handler_id);
		priv->item_queues_handler_id = 0;
	}

	crawl_directories_stop (TRACKER_MINER_FS (object));

	g_object_unref (priv->crawler);
	g_object_unref (priv->monitor);

	if (priv->directories) {
		g_list_foreach (priv->directories, (GFunc) directory_data_free, NULL);
		g_list_free (priv->directories);
	}

	g_list_foreach (priv->processing_pool, (GFunc) process_data_free, NULL);
	g_list_free (priv->processing_pool);

	g_queue_foreach (priv->items_moved, (GFunc) item_moved_data_free, NULL);
	g_queue_free (priv->items_moved);

	g_queue_foreach (priv->items_deleted, (GFunc) g_object_unref, NULL);
	g_queue_free (priv->items_deleted);

	g_queue_foreach (priv->items_updated, (GFunc) g_object_unref, NULL);
	g_queue_free (priv->items_updated);

	g_queue_foreach (priv->items_created, (GFunc) g_object_unref, NULL);
	g_queue_free (priv->items_created);

	G_OBJECT_CLASS (tracker_miner_fs_parent_class)->finalize (object);
}

static void
fs_set_property (GObject      *object,
		 guint         prop_id,
		 const GValue *value,
		 GParamSpec   *pspec)
{
	TrackerMinerFS *fs = TRACKER_MINER_FS (object);

	switch (prop_id) {
	case PROP_THROTTLE:
		tracker_miner_fs_set_throttle (TRACKER_MINER_FS (object),
					       g_value_get_double (value));
		break;
	case PROP_POOL_LIMIT:
		fs->private->pool_limit = g_value_get_uint (value);
		g_message ("Miner process pool limit is set to %d", fs->private->pool_limit);
		break;
	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
		break;
	}
}

static void
fs_get_property (GObject    *object,
		 guint       prop_id,
		 GValue     *value,
		 GParamSpec *pspec)
{
	TrackerMinerFS *fs;

	fs = TRACKER_MINER_FS (object);

	switch (prop_id) {
	case PROP_THROTTLE:
		g_value_set_double (value, fs->private->throttle);
		break;
	case PROP_POOL_LIMIT:
		g_value_set_uint (value, fs->private->pool_limit);
		break;
	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
		break;
	}
}

static gboolean
fs_defaults (TrackerMinerFS *fs,
	     GFile          *file)
{
	return TRUE;
}

static gboolean
fs_contents_defaults (TrackerMinerFS *fs,
		      GFile          *parent,
		      GList          *children)
{
	return TRUE;
}

static void
miner_started (TrackerMiner *miner)
{
	TrackerMinerFS *fs;

	fs = TRACKER_MINER_FS (miner);

	fs->private->been_started = TRUE;

	g_object_set (miner,
		      "progress", 0.0,
		      "status", _("Initializing"),
		      NULL);

	crawl_directories_start (fs);
}

static void
miner_stopped (TrackerMiner *miner)
{
	g_object_set (miner,
		      "progress", 1.0,
		      "status", _("Idle"),
		      NULL);
}

static void
miner_paused (TrackerMiner *miner)
{
	TrackerMinerFS *fs;

	fs = TRACKER_MINER_FS (miner);

	fs->private->is_paused = TRUE;

	tracker_crawler_pause (fs->private->crawler);

	if (fs->private->item_queues_handler_id) {
		g_source_remove (fs->private->item_queues_handler_id);
		fs->private->item_queues_handler_id = 0;
	}
}

static void
miner_resumed (TrackerMiner *miner)
{
	TrackerMinerFS *fs;

	fs = TRACKER_MINER_FS (miner);

	fs->private->is_paused = FALSE;

	tracker_crawler_resume (fs->private->crawler);

	/* Only set up queue handler if we have items waiting to be
	 * processed.
	 */
	if (g_queue_get_length (fs->private->items_deleted) > 0 ||
	    g_queue_get_length (fs->private->items_created) > 0 ||
	    g_queue_get_length (fs->private->items_updated) > 0 ||
	    g_queue_get_length (fs->private->items_moved) > 0) {
		item_queue_handlers_set_up (fs);
	}
}

static DirectoryData *
directory_data_new (GFile    *file,
		    gboolean  recurse)
{
	DirectoryData *dd;

	dd = g_slice_new (DirectoryData);

	dd->file = g_object_ref (file);
	dd->recurse = recurse;

	return dd;
}

static void
directory_data_free (DirectoryData *dd)
{
	if (!dd) {
		return;
	}

	g_object_unref (dd->file);
	g_slice_free (DirectoryData, dd);
}

static void
process_print_stats (TrackerMinerFS *fs)
{
	/* Only do this the first time, otherwise the results are
	 * likely to be inaccurate. Devices can be added or removed so
	 * we can't assume stats are correct.
	 */
	if (!fs->private->shown_totals) {
		fs->private->shown_totals = TRUE;

		g_message ("--------------------------------------------------");
		g_message ("Total directories : %d (%d ignored)",
			   fs->private->total_directories_found,
			   fs->private->total_directories_ignored);
		g_message ("Total files       : %d (%d ignored)",
			   fs->private->total_files_found,
			   fs->private->total_files_ignored);
		g_message ("Total monitors    : %d",
			   tracker_monitor_get_count (fs->private->monitor));
		g_message ("--------------------------------------------------\n");
	}
}

static void
commit_cb (GObject      *object,
           GAsyncResult *result,
           gpointer      user_data)
{
	TrackerMiner *miner = TRACKER_MINER (object);
	GError *error = NULL;

	tracker_miner_commit_finish (miner, result, &error);

	if (error) {
		g_critical ("Could not commit: %s", error->message);
	}
}

static void
process_stop (TrackerMinerFS *fs)
{
	/* Now we have finished crawling, print stats and enable monitor events */
	process_print_stats (fs);

	tracker_miner_commit (TRACKER_MINER (fs), NULL, commit_cb, NULL);

	g_message ("Idle");

	g_object_set (fs,
		      "progress", 1.0,
		      "status", _("Idle"),
		      NULL);

	g_signal_emit (fs, signals[FINISHED], 0,
		       g_timer_elapsed (fs->private->timer, NULL),
		       fs->private->total_directories_found,
		       fs->private->total_directories_ignored,
		       fs->private->total_files_found,
		       fs->private->total_files_ignored);

	if (fs->private->timer) {
		g_timer_destroy (fs->private->timer);
		fs->private->timer = NULL;
	}

	fs->private->total_directories_found = 0;
	fs->private->total_directories_ignored = 0;
	fs->private->total_files_found = 0;
	fs->private->total_files_ignored = 0;

	fs->private->been_crawled = TRUE;
}

static ItemMovedData *
item_moved_data_new (GFile *file,
		     GFile *source_file)
{
	ItemMovedData *data;

	data = g_slice_new (ItemMovedData);
	data->file = g_object_ref (file);
	data->source_file = g_object_ref (source_file);

	return data;
}

static void
item_moved_data_free (ItemMovedData *data)
{
	g_object_unref (data->file);
	g_object_unref (data->source_file);
	g_slice_free (ItemMovedData, data);
}

static void
sparql_update_cb (GObject      *object,
                  GAsyncResult *result,
                  gpointer      user_data)
{
	TrackerMinerFS *fs;
	TrackerMinerFSPrivate *priv;
	ProcessData *data;

	GError *error = NULL;

	tracker_miner_execute_update_finish (TRACKER_MINER (object), result, &error);

	fs = TRACKER_MINER_FS (object);
	priv = fs->private;
	data = user_data;

	if (error) {
		g_critical ("Could not execute sparql: %s", error->message);
	} else {
		if (fs->private->been_crawled) {
			/* Only commit immediately for
			 * changes after initial crawling.
			 */
			tracker_miner_commit (TRACKER_MINER (fs), NULL, commit_cb, NULL);
		}
	}

	priv->processing_pool = g_list_remove (priv->processing_pool, data);
	process_data_free (data);

	item_queue_handlers_set_up (fs);
}

static void
sparql_query_cb (GObject      *object,
                 GAsyncResult *result,
                 gpointer      user_data)
{
	SparqlQueryData *data = user_data;
	TrackerMiner *miner = TRACKER_MINER (object);

	GError *error = NULL;

	const GPtrArray *query_results = tracker_miner_execute_sparql_finish (miner, result, &error);

	data->value = query_results && query_results->len == 1;
	g_main_loop_quit (data->main_loop);
}

static void
item_add_or_update_cb (TrackerMinerFS *fs,
		       ProcessData    *data,
		       const GError   *error)
{
	gchar *uri;

	uri = g_file_get_uri (data->file);

	if (error) {
		if (error->code == G_IO_ERROR_NOT_FOUND) {
			g_message ("Could not process '%s': %s", uri, error->message);
		} else {
			g_critical ("Could not process '%s': %s", uri, error->message);
		}

		fs->private->processing_pool =
			g_list_remove (fs->private->processing_pool, data);
		process_data_free (data);

		item_queue_handlers_set_up (fs);
	} else {
		gchar *full_sparql;

		g_debug ("Adding item '%s'", uri);

		full_sparql = g_strdup_printf ("DROP GRAPH <%s> %s",
					       uri, tracker_sparql_builder_get_result (data->builder));

		tracker_miner_execute_batch_update (TRACKER_MINER (fs),
						    full_sparql,
						    NULL,
						    sparql_update_cb,
						    data);
		g_free (full_sparql);
	}

	g_free (uri);
}

static gboolean
item_add_or_update (TrackerMinerFS *fs,
		    GFile          *file)
{
	TrackerMinerFSPrivate *priv;
	TrackerSparqlBuilder *sparql;
	GCancellable *cancellable;
	gboolean processing, retval;
	ProcessData *data;

	priv = fs->private;
	retval = TRUE;

	cancellable = g_cancellable_new ();
	sparql = tracker_sparql_builder_new_update ();
	g_object_ref (file);

	data = process_data_new (file, cancellable, sparql);
	priv->processing_pool = g_list_prepend (priv->processing_pool, data);

	g_signal_emit (fs, signals[PROCESS_FILE], 0,
		       file, sparql, cancellable,
		       &processing);

	if (!processing) {
		/* Re-fetch data, since it might have been
		 * removed in broken implementations
		 */
		data = process_data_find (fs, file);

		if (!data) {
			gchar *uri;

			uri = g_file_get_uri (file);
			g_critical ("%s has returned FALSE in ::process-file for '%s', "
				    "but it seems that this file has been processed through "
				    "tracker_miner_fs_notify_file(), this is an "
				    "implementation error", G_OBJECT_TYPE_NAME (fs), uri);
			g_free (uri);
		} else {
			priv->processing_pool = g_list_remove (priv->processing_pool, data);
			process_data_free (data);
		}
	} else {
		guint length;

		length = g_list_length (priv->processing_pool);

		if (length >= priv->pool_limit) {
			retval = FALSE;
		}
	}

	g_object_unref (file);
	g_object_unref (cancellable);
	g_object_unref (sparql);

	return retval;
}

static gboolean
item_query_exists (TrackerMinerFS *miner,
		   GFile          *file)
{
	gboolean   result;
	gchar     *sparql, *uri;
	SparqlQueryData data;

	uri = g_file_get_uri (file);
	sparql = g_strdup_printf ("SELECT ?s WHERE { ?s a rdfs:Resource . FILTER (?s = <%s>) }",
	                          uri);

	data.main_loop = g_main_loop_new (NULL, FALSE);
	data.value = FALSE;

	tracker_miner_execute_sparql (TRACKER_MINER (miner),
				      sparql,
				      NULL,
				      sparql_query_cb,
				      &data);

	g_main_loop_run (data.main_loop);
	result = data.value;

	g_main_loop_unref (data.main_loop);

	g_free (sparql);
	g_free (uri);

	return result;
}

static gboolean
item_remove (TrackerMinerFS *fs,
	     GFile          *file)
{
	GString *sparql;
	gchar *uri, *slash_uri;
	ProcessData *data;

	uri = g_file_get_uri (file);

	g_debug ("Removing item: '%s' (Deleted from filesystem)",
		 uri);

	if (!item_query_exists (fs, file)) {
		g_debug ("  File does not exist anyway (uri:'%s')", uri);
		return TRUE;
	}

	if (!g_str_has_suffix (uri, "/")) {
		slash_uri = g_strconcat (uri, "/", NULL);
	} else {
		slash_uri = g_strdup (uri);
	}

	sparql = g_string_new ("");

	/* Delete all children */
	g_string_append_printf (sparql,
				"DELETE FROM <%s> { ?u a rdfs:Resource } "
				"WHERE { ?u nfo:belongsToContainer ?p . FILTER (fn:starts-with (?p, \"%s\")) } ",
				uri, slash_uri);

	/* Delete resource itself */
	g_string_append_printf (sparql,
				"DELETE FROM <%s> { <%s> a rdfs:Resource }",
				uri, uri);

	data = process_data_new (file, NULL, NULL);
	fs->private->processing_pool = g_list_prepend (fs->private->processing_pool, data);

	tracker_miner_execute_batch_update (TRACKER_MINER (fs),
					    sparql->str,
					    NULL,
					    sparql_update_cb,
					    data);

	g_string_free (sparql, TRUE);
	g_free (slash_uri);
	g_free (uri);

	return FALSE;
}

static void
item_update_uri_recursively_cb (GObject      *object,
                                GAsyncResult *result,
                                gpointer      user_data)
{
	TrackerMinerFS *fs = TRACKER_MINER_FS (object);
	RecursiveMoveData *data = user_data;
	GError *error = NULL;

	const GPtrArray *query_results = tracker_miner_execute_sparql_finish (TRACKER_MINER (object), result, &error);

	if (error) {
		g_critical ("Could not query children: %s", error->message);
	} else {
		if (query_results) {
			gint i;

			for (i = 0; i < query_results->len; i++) {
				gchar **child_source_uri, *child_uri;

				child_source_uri = g_ptr_array_index (query_results, i);

				if (!g_str_has_prefix (*child_source_uri, data->source_uri)) {
					g_warning ("Child URI '%s' does not start with parent URI '%s'",
						   *child_source_uri,
						   data->source_uri);
					continue;
				}

				child_uri = g_strdup_printf ("%s%s", data->uri, *child_source_uri + strlen (data->source_uri));

				item_update_uri_recursively (fs, data, *child_source_uri, child_uri);

				g_free (child_uri);
			}
		}
	}

	data->level--;

	g_assert (data->level >= 0);

	if (data->level == 0) {
		g_main_loop_quit (data->main_loop);
	}
}

static void
item_update_uri_recursively (TrackerMinerFS    *fs,
			     RecursiveMoveData *move_data,
			     const gchar       *source_uri,
			     const gchar       *uri)
{
	gchar *sparql;

	move_data->level++;

	g_string_append_printf (move_data->sparql, " <%s> tracker:uri <%s> .", source_uri, uri);

	sparql = g_strdup_printf ("SELECT ?child WHERE { ?child nfo:belongsToContainer <%s> }", source_uri);
	tracker_miner_execute_sparql (TRACKER_MINER (fs),
				      sparql,
				      NULL,
				      item_update_uri_recursively_cb,
				      move_data);
	g_free (sparql);
}

static gboolean
item_move (TrackerMinerFS *fs,
	   GFile          *file,
	   GFile          *source_file)
{
	gchar     *uri, *source_uri, *escaped_filename;
	GFileInfo *file_info;
	GString   *sparql;
	RecursiveMoveData move_data;
	ProcessData *data;

	uri = g_file_get_uri (file);
	source_uri = g_file_get_uri (source_file);

	/* Get 'source' ID */
	if (!item_query_exists (fs, source_file)) {
		gboolean retval;

		g_message ("Source file '%s' not found in store to move, indexing '%s' from scratch", source_uri, uri);

		retval = item_add_or_update (fs, file);

		g_free (source_uri);
		g_free (uri);

		return retval;
	}

	file_info = g_file_query_info (file,
				       G_FILE_ATTRIBUTE_STANDARD_DISPLAY_NAME,
				       G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
				       NULL, NULL);

	if (!file_info) {
		gboolean retval;

		/* Destination file has gone away, ignore dest file and remove source if any */
		retval = item_remove (fs, source_file);

		g_free (source_uri);
		g_free (uri);

		return retval;
	}

	g_debug ("Moving item from '%s' to '%s'",
		 source_uri,
		 uri);

	sparql = g_string_new ("");

	g_string_append_printf (sparql,
				"DELETE FROM <%s> { <%s> nfo:fileName ?o } WHERE { <%s> nfo:fileName ?o }",
				source_uri, source_uri, source_uri);

	g_string_append_printf (sparql, " INSERT INTO <%s> {", uri);

	escaped_filename = g_strescape (g_file_info_get_display_name (file_info), NULL);

	g_string_append_printf (sparql, " <%s> nfo:fileName \"%s\" .", source_uri, escaped_filename);

	move_data.main_loop = g_main_loop_new (NULL, FALSE);
	move_data.level = 0;
	move_data.sparql = sparql;
	move_data.source_uri = source_uri;
	move_data.uri = uri;

	item_update_uri_recursively (fs, &move_data, source_uri, uri);

	g_main_loop_run (move_data.main_loop);

	g_main_loop_unref (move_data.main_loop);

	g_string_append (sparql, " }");

	data = process_data_new (file, NULL, NULL);
	fs->private->processing_pool = g_list_prepend (fs->private->processing_pool, data);

	tracker_miner_execute_batch_update (TRACKER_MINER (fs),
					    sparql->str,
					    NULL,
					    sparql_update_cb,
					    data);

	g_free (uri);
	g_free (source_uri);
	g_object_unref (file_info);
	g_string_free (sparql, TRUE);

	return TRUE;
}

static gint
item_queue_get_next_file (TrackerMinerFS  *fs,
			  GFile          **file,
			  GFile          **source_file)
{
	ItemMovedData *data;
	GFile *queue_file;

	/* Deleted items first */
	queue_file = g_queue_pop_head (fs->private->items_deleted);
	if (queue_file) {
		*file = queue_file;
		*source_file = NULL;
		return QUEUE_DELETED;
	}

	/* Created items next */
	queue_file = g_queue_pop_head (fs->private->items_created);
	if (queue_file) {
		*file = queue_file;
		*source_file = NULL;
		return QUEUE_CREATED;
	}

	/* Updated items next */
	queue_file = g_queue_pop_head (fs->private->items_updated);
	if (queue_file) {
		*file = queue_file;
		*source_file = NULL;
		return QUEUE_UPDATED;
	}

	/* Moved items next */
	data = g_queue_pop_head (fs->private->items_moved);
	if (data) {
		*file = g_object_ref (data->file);
		*source_file = g_object_ref (data->source_file);
		item_moved_data_free (data);

		return QUEUE_MOVED;
	}

	*file = NULL;
	*source_file = NULL;

	return QUEUE_NONE;
}

static gdouble
item_queue_get_progress (TrackerMinerFS *fs)
{
	guint items_to_process = 0;
	guint items_total = 0;

	items_to_process += g_queue_get_length (fs->private->items_deleted);
	items_to_process += g_queue_get_length (fs->private->items_created);
	items_to_process += g_queue_get_length (fs->private->items_updated);
	items_to_process += g_queue_get_length (fs->private->items_moved);

	items_total += fs->private->total_directories_found;
	items_total += fs->private->total_files_found;

	if (items_to_process == 0 && items_total > 0) {
		return 0.0;
	}

	if (items_total == 0 || items_to_process > items_total) {
		return 1.0;
	}

	return (gdouble) (items_total - items_to_process) / items_total;
}

static gboolean
item_queue_handlers_cb (gpointer user_data)
{
	TrackerMinerFS *fs;
	GFile *file, *source_file;
	gint queue;
	GTimeVal time_now;
	static GTimeVal time_last = { 0 };
	gboolean keep_processing = TRUE;

	fs = user_data;
	queue = item_queue_get_next_file (fs, &file, &source_file);

	if (file && tracker_file_is_locked (file)) {
		/* File is locked, ignore any updates on it */
		g_object_unref (file);

		if (source_file) {
			g_object_unref (source_file);
		}

		return TRUE;
	}

	if (!fs->private->timer) {
		fs->private->timer = g_timer_new ();
	}

	/* Update progress, but don't spam it. */
	g_get_current_time (&time_now);

	if ((time_now.tv_sec - time_last.tv_sec) >= 1) {
		time_last = time_now;
		g_object_set (fs, "progress", item_queue_get_progress (fs), NULL);
	}

	/* Handle queues */
	switch (queue) {
	case QUEUE_NONE:
		/* Print stats and signal finished */
		if (!fs->private->is_crawling &&
		    !fs->private->processing_pool) {
			process_stop (fs);
		}

		/* No more files left to process */
		keep_processing = FALSE;
		break;
	case QUEUE_MOVED:
		keep_processing = item_move (fs, file, source_file);
		break;
	case QUEUE_DELETED:
		keep_processing = item_remove (fs, file);
		break;
	case QUEUE_CREATED:
	case QUEUE_UPDATED:
		keep_processing = item_add_or_update (fs, file);
		break;
	default:
		g_assert_not_reached ();
	}

	if (file) {
		g_object_unref (file);
	}

	if (source_file) {
		g_object_unref (source_file);
	}

	if (!keep_processing) {
		fs->private->item_queues_handler_id = 0;
		return FALSE;
	} else {
		if (fs->private->been_crawled) {
			/* Only commit immediately for
			 * changes after initial crawling.
			 */
			tracker_miner_commit (TRACKER_MINER (fs), NULL, commit_cb, NULL);
		}

		return TRUE;
	}
}

static guint
_tracker_idle_add (TrackerMinerFS *fs,
		   GSourceFunc     func,
		   gpointer        user_data)
{
	guint interval;

	interval = MAX_TIMEOUT_INTERVAL * fs->private->throttle;

	if (interval == 0) {
		return g_idle_add (func, user_data);
	} else {
		return g_timeout_add (interval, func, user_data);
	}
}

static void
item_queue_handlers_set_up (TrackerMinerFS *fs)
{
	gchar *status;

	if (fs->private->item_queues_handler_id != 0) {
		return;
	}

	if (fs->private->is_paused) {
		return;
	}

	if (g_list_length (fs->private->processing_pool) >= fs->private->pool_limit) {
		/* There is no room in the pool for more files */
		return;
	}

	g_object_get (fs, "status", &status, NULL);

	if (g_strcmp0 (status, _("Processing files")) != 0) {
		/* Don't spam this */
		g_message ("Processing files...");
		g_object_set (fs, "status", _("Processing files"), NULL);
	}

	g_free (status);

	fs->private->item_queues_handler_id =
		_tracker_idle_add (fs,
				   item_queue_handlers_cb,
				   fs);
}

static gboolean
should_change_index_for_file (TrackerMinerFS *fs,
			      GFile          *file)
{
	gboolean            uptodate;
	GFileInfo          *file_info;
	guint64             time;
	time_t              mtime;
	struct tm           t;
	gchar              *query, *uri;
	SparqlQueryData     data;

	file_info = g_file_query_info (file,
				       G_FILE_ATTRIBUTE_TIME_MODIFIED,
				       G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
				       NULL,
				       NULL);
	if (!file_info) {
		/* NOTE: We return TRUE here because we want to update the DB
		 * about this file, not because we want to index it.
		 */
		return TRUE;
	}

	time = g_file_info_get_attribute_uint64 (file_info, G_FILE_ATTRIBUTE_TIME_MODIFIED);
	mtime = (time_t) time;
	g_object_unref (file_info);

	uri = g_file_get_uri (file);

	gmtime_r (&mtime, &t);

	query = g_strdup_printf ("SELECT ?file { ?file nfo:fileLastModified \"%04d-%02d-%02dT%02d:%02d:%02dZ\" . FILTER (?file = <%s>) }",
	                         t.tm_year + 1900,
				 t.tm_mon + 1,
				 t.tm_mday,
				 t.tm_hour,
				 t.tm_min,
				 t.tm_sec,
				 uri);

	data.main_loop = g_main_loop_new (NULL, FALSE);
	data.value = FALSE;

	tracker_miner_execute_sparql (TRACKER_MINER (fs),
						      query,
						      NULL,
						      sparql_query_cb,
						      &data);

	g_main_loop_run (data.main_loop);
	uptodate = data.value;

	g_main_loop_unref (data.main_loop);
	g_free (query);
	g_free (uri);

	if (uptodate) {
		/* File already up-to-date in the database */
		return FALSE;
	}

	/* File either not yet in the database or mtime is different
	 * Update in database required
	 */
	return TRUE;
}

static gboolean
should_check_file (TrackerMinerFS *fs,
		   GFile          *file,
		   gboolean        is_dir)
{
	gboolean should_check;

	if (is_dir) {
		g_signal_emit (fs, signals[CHECK_DIRECTORY], 0, file, &should_check);
	} else {
		g_signal_emit (fs, signals[CHECK_FILE], 0, file, &should_check);
	}

	return should_check;
}

static gboolean
should_process_file (TrackerMinerFS *fs,
		     GFile          *file,
		     gboolean        is_dir)
{
	if (!should_check_file (fs, file, is_dir)) {
		return FALSE;
	}

	/* Check whether file is up-to-date in tracker-store */
	return should_change_index_for_file (fs, file);
}

static void
monitor_item_created_cb (TrackerMonitor *monitor,
			 GFile		*file,
			 gboolean	 is_directory,
			 gpointer	 user_data)
{
	TrackerMinerFS *fs;
	gboolean should_process = TRUE;
	gchar *path;

	fs = user_data;
	should_process = should_check_file (fs, file, is_directory);

	path = g_file_get_path (file);

	g_debug ("%s:'%s' (%s) (create monitor event or user request)",
		 should_process ? "Found " : "Ignored",
		 path,
		 is_directory ? "DIR" : "FILE");

	if (should_process) {
		if (is_directory) {
			tracker_miner_fs_add_directory (fs, file, TRUE);
		} else {
			g_queue_push_tail (fs->private->items_created,
					   g_object_ref (file));

			item_queue_handlers_set_up (fs);
		}
	}

	g_free (path);
}

static void
monitor_item_updated_cb (TrackerMonitor *monitor,
			 GFile		*file,
			 gboolean	 is_directory,
			 gpointer	 user_data)
{
	TrackerMinerFS *fs;
	gboolean should_process;
	gchar *path;

	fs = user_data;
	should_process = should_check_file (fs, file, is_directory);

	path = g_file_get_path (file);

 	g_debug ("%s:'%s' (%s) (update monitor event or user request)",
		 should_process ? "Found " : "Ignored",
		 path,
		 is_directory ? "DIR" : "FILE");

	if (should_process) {
		g_queue_push_tail (fs->private->items_updated,
				   g_object_ref (file));

		item_queue_handlers_set_up (fs);
	}

	g_free (path);
}

static void
monitor_item_deleted_cb (TrackerMonitor *monitor,
			 GFile		*file,
			 gboolean	 is_directory,
			 gpointer	 user_data)
{
	TrackerMinerFS *fs;
	gboolean should_process;
	gchar *path;

	fs = user_data;
	should_process = should_check_file (fs, file, is_directory);
	path = g_file_get_path (file);

	g_debug ("%s:'%s' (%s) (delete monitor event or user request)",
		 should_process ? "Found " : "Ignored",
		 path,
		 is_directory ? "DIR" : "FILE");

	if (should_process) {
		g_queue_push_tail (fs->private->items_deleted,
				   g_object_ref (file));

		item_queue_handlers_set_up (fs);
	}

#if 0
	/* FIXME: Should we do this for MOVE events too? */

	/* Remove directory from list of directories we are going to
	 * iterate if it is in there.
	 */
	l = g_list_find_custom (fs->private->directories,
				path,
				(GCompareFunc) g_strcmp0);

	/* Make sure we don't remove the current device we are
	 * processing, this is because we do this same clean up later
	 * in process_device_next()
	 */
	if (l && l != fs->private->current_directory) {
		directory_data_free (l->data);
		fs->private->directories =
			g_list_delete_link (fs->private->directories, l);
	}
#endif

	g_free (path);
}

static void
monitor_item_moved_cb (TrackerMonitor *monitor,
		       GFile	      *file,
		       GFile	      *other_file,
		       gboolean        is_directory,
		       gboolean        is_source_monitored,
		       gpointer        user_data)
{
	TrackerMinerFS *fs;

	fs = user_data;

	if (!is_source_monitored) {
		if (is_directory) {
			gchar *path;

			path = g_file_get_path (other_file);

			g_debug ("Not in store:'?'->'%s' (DIR) (move monitor event, source unknown)",
				 path);

			/* If the source is not monitored, we need to crawl it. */
			tracker_miner_fs_add_directory (fs, other_file, TRUE);

			g_free (path);
		}
	} else {
		gchar *path;
		gchar *other_path;
		gboolean source_stored, should_process_other;

		path = g_file_get_path (file);
		other_path = g_file_get_path (other_file);

		source_stored = item_query_exists (fs, file);
		should_process_other = should_check_file (fs, other_file, is_directory);

		g_debug ("%s:'%s'->'%s':%s (%s) (move monitor event or user request)",
			 source_stored ? "In store" : "Not in store",
			 path,
			 other_path,
			 should_process_other ? "Found " : "Ignored",
			 is_directory ? "DIR" : "FILE");

		/* FIXME: Guessing this soon the queue the event should pertain
		 *        to could introduce race conditions if events from other
		 *        queues for the same files are processed before items_moved,
		 *        Most of these decisions should be taken when the event is
		 *        actually being processed.
		 */
		if (!source_stored && !should_process_other) {
			/* Do nothing */
		} else if (!source_stored) {
			/* Source file was not stored, check dest file as new */
			if (!is_directory) {
				g_queue_push_tail (fs->private->items_created,
						   g_object_ref (other_file));

				item_queue_handlers_set_up (fs);
			} else {
				g_debug ("Not in store:'?'->'%s' (DIR) (move monitor event, source monitored)",
					 path);

				tracker_miner_fs_add_directory (fs, other_file, TRUE);
			}
		} else if (!should_process_other) {
			/* Delete old file */
			g_queue_push_tail (fs->private->items_deleted, g_object_ref (file));

			item_queue_handlers_set_up (fs);
		} else {
			/* Move old file to new file */
			g_queue_push_tail (fs->private->items_moved,
					   item_moved_data_new (other_file, file));

			item_queue_handlers_set_up (fs);
		}

		g_free (other_path);
		g_free (path);
	}
}

static gboolean
crawler_check_file_cb (TrackerCrawler *crawler,
		       GFile	      *file,
		       gpointer        user_data)
{
	TrackerMinerFS *fs = user_data;

	return should_process_file (fs, file, FALSE);
}

static gboolean
crawler_check_directory_cb (TrackerCrawler *crawler,
			    GFile	   *file,
			    gpointer	    user_data)
{
	TrackerMinerFS *fs = user_data;
	gboolean should_check, should_change_index;
	gboolean add_monitor = TRUE;

	should_check = should_check_file (fs, file, TRUE);
	should_change_index = should_change_index_for_file (fs, file);

	if (!should_change_index) {
		/* Mark the file as ignored, we still want the crawler
		 * to iterate over its contents, but the directory hasn't
		 * actually changed, hence this flag.
		 */
		g_object_set_qdata (G_OBJECT (file),
				    fs->private->quark_ignore_file,
				    GINT_TO_POINTER (TRUE));
	}

	g_signal_emit (fs, signals[MONITOR_DIRECTORY], 0, file, &add_monitor);

	/* FIXME: Should we add here or when we process the queue in
	 * the finished sig?
	 */
	if (add_monitor) {
		tracker_monitor_add (fs->private->monitor, file);
	}

	/* We _HAVE_ to check ALL directories because mtime updates
	 * are not guaranteed on parents on Windows AND we on Linux
	 * only the immediate parent directory mtime is updated, this
	 * is not done recursively.
	 *
	 * As such, we only use the "check" rules here, we don't do
	 * any database comparison with mtime.
	 */
	return should_check;
}

static gboolean
crawler_check_directory_contents_cb (TrackerCrawler *crawler,
				     GFile          *parent,
				     GList          *children,
				     gpointer        user_data)
{
	TrackerMinerFS *fs = user_data;
	gboolean process;

	g_signal_emit (fs, signals[CHECK_DIRECTORY_CONTENTS], 0, parent, children, &process);

	return process;
}

static void
crawler_finished_cb (TrackerCrawler *crawler,
		     GQueue         *found,
		     gboolean        was_interrupted,
		     guint	     directories_found,
		     guint	     directories_ignored,
		     guint	     files_found,
		     guint	     files_ignored,
		     gpointer	     user_data)
{
	TrackerMinerFS *fs = user_data;
	GList *l;

	/* Add items in queue to current queues. */
	for (l = found->head; l; l = l->next) {
		GFile *file = l->data;

		if (!g_object_get_qdata (G_OBJECT (file), fs->private->quark_ignore_file)) {
			g_queue_push_tail (fs->private->items_created, g_object_ref (file));
		}
	}

	fs->private->is_crawling = FALSE;

	/* Update stats */
	fs->private->directories_found += directories_found;
	fs->private->directories_ignored += directories_ignored;
	fs->private->files_found += files_found;
	fs->private->files_ignored += files_ignored;

	fs->private->total_directories_found += directories_found;
	fs->private->total_directories_ignored += directories_ignored;
	fs->private->total_files_found += files_found;
	fs->private->total_files_ignored += files_ignored;

	g_message ("%s crawling files after %2.2f seconds",
		   was_interrupted ? "Stopped" : "Finished",
		   g_timer_elapsed (fs->private->timer, NULL));
	g_message ("  Found %d directories, ignored %d directories",
		   directories_found,
		   directories_ignored);
	g_message ("  Found %d files, ignored %d files",
		   files_found,
		   files_ignored);

	directory_data_free (fs->private->current_directory);
	fs->private->current_directory = NULL;

	/* Proceed to next thing to process */
	crawl_directories_start (fs);
}

static gboolean
crawl_directories_cb (gpointer user_data)
{
	TrackerMinerFS *fs = user_data;
	gchar *path;
	gchar *str;

	if (fs->private->current_directory) {
		g_critical ("One directory is already being processed, bailing out");
		fs->private->crawl_directories_id = 0;
		return FALSE;
	}

	if (!fs->private->directories) {
		/* Now we handle the queue */
		item_queue_handlers_set_up (fs);
		crawl_directories_stop (fs);

		fs->private->crawl_directories_id = 0;
		return FALSE;
	}

	fs->private->current_directory = fs->private->directories->data;
	fs->private->directories = g_list_remove (fs->private->directories,
						  fs->private->current_directory);

	path = g_file_get_path (fs->private->current_directory->file);

	if (fs->private->current_directory->recurse) {
		str = g_strdup_printf (_("Crawling recursively directory '%s'"), path);
	} else {
		str = g_strdup_printf (_("Crawling single directory '%s'"), path);
	}

	g_message ("%s", str);

	g_object_set (fs, "status", str, NULL);
	g_free (str);
	g_free (path);

	if (tracker_crawler_start (fs->private->crawler,
				   fs->private->current_directory->file,
				   fs->private->current_directory->recurse)) {
		/* Crawler when restart the idle function when done */
		fs->private->is_crawling = TRUE;
		fs->private->crawl_directories_id = 0;
		return FALSE;
	}

	/* Directory couldn't be processed */
	directory_data_free (fs->private->current_directory);
	fs->private->current_directory = NULL;

	return TRUE;
}

static void
crawl_directories_start (TrackerMinerFS *fs)
{
	if (fs->private->crawl_directories_id != 0) {
		/* Processing ALREADY going on */
		return;
	}

	if (!fs->private->been_started) {
		/* Miner has not been started yet */
		return;
	}

	if (!fs->private->timer) {
		fs->private->timer = g_timer_new ();
	}

	fs->private->directories_found = 0;
	fs->private->directories_ignored = 0;
	fs->private->files_found = 0;
	fs->private->files_ignored = 0;

	fs->private->crawl_directories_id = _tracker_idle_add (fs, crawl_directories_cb, fs);
}

static void
crawl_directories_stop (TrackerMinerFS *fs)
{
	if (fs->private->crawl_directories_id == 0) {
		/* No processing going on, nothing to stop */
		return;
	}

	if (fs->private->current_directory) {
		tracker_crawler_stop (fs->private->crawler);
	}

	/* Is this the right time to emit FINISHED? What about
	 * monitor events left to handle? Should they matter
	 * here?
	 */
	if (fs->private->crawl_directories_id != 0) {
		g_source_remove (fs->private->crawl_directories_id);
		fs->private->crawl_directories_id = 0;
	}
}

/**
 * tracker_miner_fs_add_directory:
 * @fs: a #TrackerMinerFS
 * @file: #GFile for the directory to inspect
 * @recurse: whether the directory should be inspected recursively
 *
 * Tells the filesystem miner to inspect a directory.
 **/
void
tracker_miner_fs_add_directory (TrackerMinerFS *fs,
				GFile          *file,
				gboolean        recurse)
{
	g_return_if_fail (TRACKER_IS_MINER_FS (fs));
	g_return_if_fail (G_IS_FILE (file));

	fs->private->directories =
		g_list_append (fs->private->directories,
			       directory_data_new (file, recurse));

	crawl_directories_start (fs);
}

static void
check_files_removal (GQueue *queue,
		     GFile  *parent)
{
	GList *l;

	l = queue->head;

	while (l) {
		GFile *file = l->data;
		GList *link = l;

		l = l->next;

		if (g_file_equal (file, parent) ||
		    g_file_has_prefix (file, parent)) {
			g_queue_delete_link (queue, link);
			g_object_unref (file);
		}
	}
}

/**
 * tracker_miner_fs_remove_directory:
 * @fs: a #TrackerMinerFS
 * @file: #GFile for the directory to be removed
 *
 * Removes a directory from being inspected by @fs.
 *
 * Returns: %TRUE if the directory was successfully removed.
 **/
gboolean
tracker_miner_fs_remove_directory (TrackerMinerFS *fs,
				   GFile          *file)
{
	TrackerMinerFSPrivate *priv;
	gboolean return_val = FALSE;
	GList *dirs, *pool;

	g_return_val_if_fail (TRACKER_IS_MINER_FS (fs), FALSE);
	g_return_val_if_fail (G_IS_FILE (file), FALSE);

	priv = fs->private;

	if (fs->private->current_directory) {
		GFile *current_file;

		current_file = fs->private->current_directory->file;

		if (g_file_equal (file, current_file) ||
		    g_file_has_prefix (file, current_file)) {
			/* Dir is being processed currently, cancel crawler */
			tracker_crawler_stop (fs->private->crawler);
			return_val = TRUE;
		}
	}

	dirs = fs->private->directories;

	while (dirs) {
		DirectoryData *data = dirs->data;
		GList *link = dirs;

		dirs = dirs->next;

		if (g_file_equal (file, data->file) ||
		    g_file_has_prefix (file, data->file)) {
			directory_data_free (data);
			fs->private->directories = g_list_delete_link (fs->private->directories, link);
			return_val = TRUE;
		}
	}

	/* Remove anything contained in the removed directory
	 * from all relevant processing queues.
	 */
	check_files_removal (priv->items_updated, file);
	check_files_removal (priv->items_created, file);

	pool = fs->private->processing_pool;

	while (pool) {
		ProcessData *data = pool->data;

		if (g_file_equal (data->file, file) ||
		    g_file_has_prefix (data->file, file)) {
			g_cancellable_cancel (data->cancellable);
		}

		pool = pool->next;
	}

	return return_val;
}

/**
 * tracker_miner_fs_set_throttle:
 * @fs: a #TrackerMinerFS
 * @throttle: throttle value, between 0 and 1
 *
 * Tells the filesystem miner to throttle its operations.
 * a value of 0 means no throttling at all, so the miner
 * will perform operations at full speed, 1 is the slowest
 * value.
 **/
void
tracker_miner_fs_set_throttle (TrackerMinerFS *fs,
			       gdouble         throttle)
{
	g_return_if_fail (TRACKER_IS_MINER_FS (fs));

	throttle = CLAMP (throttle, 0, 1);

	if (fs->private->throttle == throttle) {
		return;
	}

	fs->private->throttle = throttle;

	/* Update timeouts */
	if (fs->private->item_queues_handler_id != 0) {
		g_source_remove (fs->private->item_queues_handler_id);

		fs->private->item_queues_handler_id =
			_tracker_idle_add (fs,
					   item_queue_handlers_cb,
					   fs);
	}

	if (fs->private->crawl_directories_id) {
		g_source_remove (fs->private->crawl_directories_id);

		fs->private->crawl_directories_id =
			_tracker_idle_add (fs, crawl_directories_cb, fs);
	}
}

/**
 * tracker_miner_fs_get_throttle:
 * @fs: a #TrackerMinerFS
 *
 * Gets the current throttle value. see tracker_miner_fs_set_throttle().
 *
 * Returns: current throttle value.
 **/
gdouble
tracker_miner_fs_get_throttle (TrackerMinerFS *fs)
{
	g_return_val_if_fail (TRACKER_IS_MINER_FS (fs), 0);

	return fs->private->throttle;
}

/**
 * tracker_miner_fs_notify_file:
 * @fs: a #TrackerMinerFS
 * @file: a #GFile
 * @error: a #GError with the error that happened during processing, or %NULL.
 *
 * Notifies @fs that all processing on @file has been finished, if any error
 * happened during file data processing, it should be passed in @error, else
 * that parameter will contain %NULL to reflect success.
 **/
void
tracker_miner_fs_notify_file (TrackerMinerFS *fs,
			      GFile          *file,
			      const GError   *error)
{
	ProcessData *data;

	g_return_if_fail (TRACKER_IS_MINER_FS (fs));
	g_return_if_fail (G_IS_FILE (file));

	data = process_data_find (fs, file);

	if (!data) {
		gchar *uri;

		uri = g_file_get_uri (file);
		g_critical ("%s has notified that file '%s' has been processed, "
			    "but that file was not in the processing queue. "
			    "This is an implementation error, please ensure that "
			    "tracker_miner_fs_notify_file() is called on the right "
			    "file and that the ::process-file signal didn't return "
			    "FALSE for it", G_OBJECT_TYPE_NAME (fs), uri);
		g_free (uri);

		return;
	}

	item_add_or_update_cb (fs, data, error);
}
