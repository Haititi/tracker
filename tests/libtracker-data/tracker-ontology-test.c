/*
 * Copyright (C) 2009, Nokia <ivan.frade@nokia.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA  02110-1301, USA.
 */

#include "config.h"

#include <string.h>
#include <locale.h>

#include <glib.h>
#include <gio/gio.h>
#include <glib/gstdio.h>

#include <libtracker-data/tracker-data-manager.h>
#include <libtracker-data/tracker-data-query.h>
#include <libtracker-data/tracker-data-update.h>
#include <libtracker-data/tracker-data.h>
#include <libtracker-data/tracker-sparql-query.h>

static gchar *tests_data_dir = NULL;
static gchar *xdg_location = NULL;

typedef struct _TestInfo TestInfo;

struct _TestInfo {
	const gchar *test_name;
	const gchar *data;
};

typedef struct _ChangeInfo ChangeInfo;

struct _ChangeInfo {
	const gchar *ontology;
	const gchar *update;
	const gchar *test_name;
	const gchar *ptr;
};

const TestInfo all_other_tests[] = {
	{ "init", NULL },
	{ NULL }
};

const TestInfo nie_tests[] = {
	{ "nie/filter-subject-1", "nie/data-1" },
	{ "nie/filter-characterset-1", "nie/data-1" },
	{ "nie/filter-comment-1", "nie/data-1" },
	{ "nie/filter-description-1", "nie/data-1" },
	{ "nie/filter-generator-1", "nie/data-1" },
	{ "nie/filter-identifier-1", "nie/data-1" },
	{ "nie/filter-keyword-1", "nie/data-1" },
	{ "nie/filter-language-1", "nie/data-1" },
	{ "nie/filter-legal-1", "nie/data-1" },
	{ "nie/filter-title-1", "nie/data-1" },
	{ "nie/filter-version-1", "nie/data-1" },
	{ NULL, NULL }
};

const TestInfo nmo_tests[] = {
	{ "nmo/filter-charset-1", "nmo/data-1" },
	{ "nmo/filter-contentdescription-1", "nmo/data-1" },
	{ "nmo/filter-contentid-1", "nmo/data-1" },
	{ "nmo/filter-contenttransferencoding-1", "nmo/data-1" },
	{ "nmo/filter-headername-1", "nmo/data-1" },
	{ "nmo/filter-headervalue-1", "nmo/data-1" },
	{ "nmo/filter-isanswered-1", "nmo/data-1" },
	{ "nmo/filter-isdeleted-1", "nmo/data-1" },
	{ "nmo/filter-isdraft-1", "nmo/data-1" },
	{ "nmo/filter-isflagged-1", "nmo/data-1" },
	{ "nmo/filter-isread-1", "nmo/data-1" },
	{ "nmo/filter-isrecent-1", "nmo/data-1" },
	{ "nmo/filter-messageid-1", "nmo/data-1" },
	{ "nmo/filter-messagesubject-1", "nmo/data-1" },
	{ NULL, NULL }
};

static void
query_helper (const gchar *query_filename, const gchar *results_filename)
{
	GError *error = NULL;
	gchar *queries = NULL, *query;
	gchar *results = NULL;
	GString *test_results = NULL;

	g_file_get_contents (query_filename, &queries, NULL, &error);
	g_assert_no_error (error);

	g_file_get_contents (results_filename, &results, NULL, &error);
	g_assert_no_error (error);

	/* perform actual query */

	query = strtok (queries, "~");

	while (query) {
		TrackerDBCursor *cursor;

		cursor = tracker_data_query_sparql_cursor (query, &error);
		g_assert_no_error (error);

		/* compare results with reference output */

		if (!test_results) {
			test_results = g_string_new ("");
		} else {
			g_string_append (test_results, "~\n");
		}

		if (cursor) {
			gint col;

			while (tracker_db_cursor_iter_next (cursor, NULL, &error)) {
				for (col = 0; col < tracker_db_cursor_get_n_columns (cursor); col++) {
					const gchar *str;

					if (col > 0) {
						g_string_append (test_results, "\t");
					}

					str = tracker_db_cursor_get_string (cursor, col, NULL);
					if (str != NULL) {
						/* bound variable */
						g_string_append_printf (test_results, "\"%s\"", str);
					}
				}

				g_string_append (test_results, "\n");
			}

			g_object_unref (cursor);
		}

		query = strtok (NULL, "~");
	}

	if (strcmp (results, test_results->str)) {
		/* print result difference */
		gchar *quoted_results;
		gchar *command_line;
		gchar *quoted_command_line;
		gchar *shell;
		gchar *diff;

		quoted_results = g_shell_quote (test_results->str);
		command_line = g_strdup_printf ("echo -n %s | diff -u %s -", quoted_results, results_filename);
		quoted_command_line = g_shell_quote (command_line);
		shell = g_strdup_printf ("sh -c %s", quoted_command_line);
		g_spawn_command_line_sync (shell, &diff, NULL, NULL, &error);
		g_assert_no_error (error);

		g_error ("%s", diff);

		g_free (quoted_results);
		g_free (command_line);
		g_free (quoted_command_line);
		g_free (shell);
		g_free (diff);
	}

	g_string_free (test_results, TRUE);
	g_free (results);
	g_free (queries);
}

static void
test_ontology_init (TestInfo      *test_info,
                    gconstpointer  context)
{
	GError *error = NULL;

	tracker_db_journal_set_rotating (FALSE, G_MAXSIZE, NULL);

	/* first-time initialization */
	tracker_data_manager_init (TRACKER_DB_MANAGER_FORCE_REINDEX,
	                           NULL,
	                           NULL,
	                           FALSE,
	                           FALSE,
	                           100,
	                           100,
	                           NULL,
	                           NULL,
	                           NULL,
	                           &error);

	g_assert_no_error (error);

	tracker_data_manager_shutdown ();

	tracker_db_journal_set_rotating (FALSE, G_MAXSIZE, NULL);

	/* initialization from existing database */
	tracker_data_manager_init (0,
	                           NULL,
	                           NULL,
	                           FALSE,
	                           FALSE,
	                           100,
	                           100,
	                           NULL,
	                           NULL,
	                           NULL,
	                           &error);

	g_assert_no_error (error);

	tracker_data_manager_shutdown ();
}

static void
test_query (TestInfo      *test_info,
            gconstpointer  context)
{
	GError *error = NULL;
	gchar *data_filename;
	gchar *query_filename;
	gchar *results_filename;
	gchar *prefix, *data_prefix, *test_prefix;

	prefix = g_build_path (G_DIR_SEPARATOR_S, TOP_SRCDIR, "tests", "libtracker-data", NULL);
	data_prefix = g_build_filename (prefix, test_info->data, NULL);
	test_prefix = g_build_filename (prefix, test_info->test_name, NULL);
	g_free (prefix);

	tracker_db_journal_set_rotating (FALSE, G_MAXSIZE, NULL);

	/* initialization */
	tracker_data_manager_init (TRACKER_DB_MANAGER_FORCE_REINDEX,
	                           NULL,
	                           NULL,
	                           FALSE,
	                           FALSE,
	                           100,
	                           100,
	                           NULL,
	                           NULL,
	                           NULL,
	                           NULL);

	/* load data set */
	data_filename = g_strconcat (data_prefix, ".ttl", NULL);
	tracker_turtle_reader_load (data_filename, &error);
	g_assert_no_error (error);

	query_filename = g_strconcat (test_prefix, ".rq", NULL);
	results_filename = g_strconcat (test_prefix, ".out", NULL);

	g_free (data_prefix);
	g_free (test_prefix);

	query_helper (query_filename, results_filename);

	/* cleanup */

	g_free (data_filename);
	g_free (query_filename);
	g_free (results_filename);

	tracker_data_manager_shutdown ();
}

static inline void
setup (TestInfo *info,
       gint      i)
{
	/* Sadly, we can't use ONE location per test because GLib
	 * caches XDG env vars, so g_get_*dir() will not change if we
	 * update the environment, this sucks majorly.
	 */
	if (!xdg_location) {
		gchar *basename;

		/* NOTE: g_test_build_filename() doesn't work env vars G_TEST_* are not defined?? */
		basename = g_strdup_printf ("%d", g_test_rand_int_range (0, G_MAXINT));
		xdg_location = g_build_path (G_DIR_SEPARATOR_S, tests_data_dir, basename, NULL);
		g_free (basename);

		g_assert_true (g_setenv ("XDG_DATA_HOME", xdg_location, TRUE));
		g_assert_true (g_setenv ("XDG_CACHE_HOME", xdg_location, TRUE));
		g_assert_true (g_setenv ("TRACKER_DB_ONTOLOGIES_DIR", TOP_SRCDIR "/src/ontologies/", TRUE));
	}
}

static void
setup_nie (TestInfo      *info,
           gconstpointer  context)
{
	gint i = GPOINTER_TO_INT (context);

	*info = nie_tests[i];
	setup (info, i);
}

static void
setup_nmo (TestInfo      *info,
           gconstpointer  context)
{
	gint i = GPOINTER_TO_INT (context);

	*info = nmo_tests[i];
	setup (info, i);
}

static void
setup_all_others (TestInfo      *info,
                  gconstpointer  context)
{
	gint i = GPOINTER_TO_INT (context);

	*info = all_other_tests[i];
	setup (info, i);
}

static void
teardown (TestInfo      *info,
          gconstpointer  context)
{
	gchar *cleanup_command;

	/* clean up */
	g_print ("Removing temporary data (%s)\n", xdg_location);

	cleanup_command = g_strdup_printf ("rm -Rf %s/", xdg_location);
	g_spawn_command_line_sync (cleanup_command, NULL, NULL, NULL, NULL);
	g_free (cleanup_command);

	g_free (xdg_location);
	xdg_location = NULL;
}

int
main (int argc, char **argv)
{
	gchar *current_dir;
	gint result;
	gint i;

	/* Warning warning!!! We need to impose a proper LC_COLLATE here, so
	 * that the expected order in the test results is always the same! */
	setlocale (LC_COLLATE, "en_US.utf8");

	current_dir = g_get_current_dir ();
	tests_data_dir = g_build_path (G_DIR_SEPARATOR_S, current_dir, "test-data", NULL);
	g_free (current_dir);

	g_test_init (&argc, &argv, NULL);

	/* add test cases */
	g_test_add ("/libtracker-data/ontology-init", TestInfo, GINT_TO_POINTER(0), setup_all_others, test_ontology_init, teardown);

	for (i = 0; nie_tests[i].test_name; i++) {
		gchar *testpath;

		testpath = g_strconcat ("/libtracker-data/nie/", nie_tests[i].test_name, NULL);
		g_test_add (testpath, TestInfo, GINT_TO_POINTER(i), setup_nie, test_query, teardown);
		g_free (testpath);
	}

	for (i = 0; nmo_tests[i].test_name; i++) {
		gchar *testpath;

		testpath = g_strconcat ("/libtracker-data/nmo/", nmo_tests[i].test_name, NULL);
		g_test_add (testpath, TestInfo, GINT_TO_POINTER(i), setup_nmo, test_query, teardown);
		g_free (testpath);
	}

	/* run tests */
	result = g_test_run ();

	g_remove (tests_data_dir);
	g_free (tests_data_dir);

	return result;
}
