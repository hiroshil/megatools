/*
 *  megatools - Mega.nz client library and tools
 *  Copyright (C) 2019 Leo Meyer <leichtenfels@protonmail.ch>
 *  Copyright (C) 2013  Ondřej Jirman <megous@megous.com>
 *  sync.c - synchronize a local folder with a remote mega.nz folder
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along
 *  with this program; if not, write to the Free Software Foundation, Inc.,
 *  51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */
#include <sys/types.h>
#include <utime.h>

#include "tools.h"
#include "shell.h"

static gchar *opt_remote_path;
static gchar *opt_local_path;
static gboolean opt_delete;
static gboolean opt_download;
static gboolean opt_noprogress;
static gboolean opt_dryrun;
static gboolean opt_quiet;
static gboolean opt_force;
static struct mega_session *s;

static GOptionEntry entries[] = {
	{ "remote", 'r', 0, G_OPTION_ARG_STRING, &opt_remote_path, "Remote directory", "PATH" },
	{ "local", 'l', 0, G_OPTION_ARG_STRING, &opt_local_path, "Local directory", "PATH" },
	{ "delete", '\0', 0, G_OPTION_ARG_NONE, &opt_delete, "Delete missing files on target", NULL },
	{ "download", 'd', 0, G_OPTION_ARG_NONE, &opt_download, "Download files from mega", NULL },
	{ "no-progress", '\0', 0, G_OPTION_ARG_NONE, &opt_noprogress, "Disable progress bar", NULL },
	{ "dryrun", 'n', 0, G_OPTION_ARG_NONE, &opt_dryrun, "Don't perform any actual changes", NULL },
	{ "quiet", 'q', 0, G_OPTION_ARG_NONE, &opt_quiet, "Output warnings and errors only", NULL },
	{ "force", '\0', 0, G_OPTION_ARG_NONE, &opt_force, "Overwrite directories on target with files", NULL },
	{ NULL }
};

static gchar *cur_file = NULL;

static void status_callback(struct mega_status_data *data, gpointer userdata)
{
	if (!opt_noprogress && data->type == MEGA_STATUS_PROGRESS)
		tool_show_progress(cur_file, data);
}

// upload operation

static gboolean up_sync_file(GFile *root, GFile *file, const gchar *remote_path)
{
	GError *local_err = NULL;
  gc_free gchar* local_path = g_file_get_path(file);

	GStatBuf fileStat;
	if (g_stat(local_path, &fileStat)) {
		g_printerr("ERROR: Unable to stat %s\n", local_path);
		return FALSE;
	}

  if (fileStat.st_size <= 0) {
		if (mega_debug & MEGA_DEBUG_APP) {
			g_print("Ignoring empty file %s\n", local_path);
		}
		return FALSE;
  }

	struct mega_node *node = mega_session_stat(s, remote_path);
	if (node) {
    // check whether the node represents a directory
 		if (!opt_force && node->type == MEGA_NODE_FOLDER) {
			g_printerr("ERROR: Target is a directory, cannot overwrite (use --force): %s\n", remote_path);
			return FALSE;
		}

    // if the local timestamp is not available, fall back to the upload timestamp
    glong timestamp = node->local_ts > 0 ? node->local_ts : node->timestamp;

		gboolean doUpload = FALSE;

		// size check
		if (node->size != fileStat.st_size) {
			doUpload = TRUE;
			if (mega_debug & MEGA_DEBUG_APP)
				g_print("File %s: sizes differ\n", remote_path);
		}

		// timestamp check
		if (!doUpload) {
			if (fileStat.st_mtime != timestamp) {
				doUpload = TRUE;
				// get local file timestamp
				if (mega_debug & MEGA_DEBUG_APP) {
					guchar local_ts_buf[20];
					g_snprintf(&local_ts_buf[0], 20, "%lu", fileStat.st_mtime);
					guchar remote_ts_buf[20];
					g_snprintf(&remote_ts_buf[0], 20, "%lu", timestamp);
					g_print("File %s: timestamp mismatch\n", remote_path);
					g_print("  Local file timestamp is: %s\n", &local_ts_buf[0]);
					g_print("  Remote timestamp is: %s\n", &remote_ts_buf[0]);
				}
			}
		}

		if (!doUpload) {
			if (mega_debug & MEGA_DEBUG_APP) {
				g_print("File %s appears identical, skipping\n", remote_path);
			}
			return FALSE;
		}

    if (!opt_quiet)
	    g_print("R %s\n", remote_path);

		if (!opt_dryrun) {
			if (!mega_session_rm(s, remote_path, &local_err)) {
				g_printerr("ERROR: Can't remove %s: %s\n", remote_path, local_err->message);
				return FALSE;
			}
		}
	}

	if (!opt_quiet)
		g_print("F %s\n", remote_path);

	if (!opt_dryrun) {
		g_free(cur_file);
		cur_file = g_file_get_basename(file);

		if (!mega_session_put_compat(s, remote_path, local_path, &local_err)) {
			if (!opt_noprogress && tool_is_stdout_tty())
				g_print("\r" ESC_CLREOL);

			g_printerr("ERROR: Upload failed for %s: %s\n", remote_path, local_err->message);
			g_clear_error(&local_err);
			return FALSE;
		}

		if (!opt_noprogress && tool_is_stdout_tty())
			g_print("\r" ESC_CLREOL);
	}

	return TRUE;
}

static gboolean up_sync_dir(GFile *root, GFile *file, const gchar *remote_path)
{
	GError *local_err = NULL;
	GFileInfo *i;
	GSList *remote_children = NULL;
	GHashTable *hash_table = NULL;

	if (root != file) {
		struct mega_node *node = mega_session_stat(s, remote_path);
    // if the remote node is a file, it is deleted and replaced by the directory
		if (node && node->type == MEGA_NODE_FILE) {
      if (!opt_quiet)
        g_print("R %s\n", remote_path);

      if (!opt_dryrun) {
			  if (!mega_session_rm(s, remote_path, &local_err)) {
				  g_printerr("ERROR: Can't remove %s: %s\n", remote_path, local_err->message);
				  return FALSE;
			  }
		  }
		}

		if (!node) {
      if (!opt_quiet)
        g_print("D %s\n", remote_path);

			if (!opt_dryrun) {
				if (!mega_session_mkdir(s, remote_path, &local_err)) {
					g_printerr("ERROR: Can't create remote directory %s: %s\n", remote_path,
						   local_err->message);
					g_clear_error(&local_err);
					return FALSE;
				}
			}
		}
	}

	if (opt_delete) {
		// get list of remote files
		remote_children = mega_session_ls(s, remote_path, FALSE);
		GSList *c;

		hash_table = g_hash_table_new(&g_str_hash, &g_str_equal);

		// insert nodes into hash table indexed by name
		for (c = remote_children; c; c = c->next) {
			struct mega_node *n = c->data;
			g_hash_table_insert(hash_table, (gpointer)n->name, (gpointer)n);
		}
	}

	// sync children
	gc_object_unref GFileEnumerator *e =
		g_file_enumerate_children(file, "standard::*",
					  G_FILE_QUERY_INFO_NONE,
					  NULL, &local_err);
	if (!e) {
		g_printerr("ERROR: Can't read local directory %s: %s\n", g_file_get_relative_path(root, file),
			   local_err->message);
		g_clear_error(&local_err);
		return FALSE;
	}

	gboolean status = TRUE;
	while ((i = g_file_enumerator_next_file(e, NULL, NULL))) {
		const gchar *name = g_file_info_get_name(i);
		gc_object_unref GFile *child = g_file_get_child(file, name);
		GFileType type = g_file_query_file_type(child, 0, NULL);
		gc_free gchar *child_remote_path = g_strconcat(remote_path, "/", name, NULL);

		if (opt_delete) {
			if (!g_hash_table_remove(hash_table, (gconstpointer)name))
				if (mega_debug & MEGA_DEBUG_APP)
					g_print("New file: %s\n", name);
		}

		if (type == G_FILE_TYPE_DIRECTORY) {
			if (!up_sync_dir(root, child, child_remote_path))
				status = FALSE;
		} else if (type == G_FILE_TYPE_REGULAR) {
			if (!up_sync_file(root, child, child_remote_path))
				status = FALSE;
		} else {
			gc_free gchar* rel_path = g_file_get_relative_path(root, file);

			g_printerr("WARNING: Skipping special file %s\n", rel_path);
		}

		g_object_unref(i);
	}

	if (opt_delete) {
		// get leftovers on remote
		GList *to_delete = g_hash_table_get_keys(hash_table), *i;
		for (i = to_delete; i; i = i->next) {
			struct mega_node *n = (struct mega_node *)g_hash_table_lookup(hash_table, i->data);
			if (n) {
				gc_free gchar *node_path = mega_node_get_path_dup(n);

				if (!opt_quiet)
					g_print("R %s\n", node_path);

				if (!opt_dryrun) {
					if (!mega_session_rm(s, node_path, &local_err)) {
						g_printerr("ERROR: Can't remove %s: %s\n", node_path, local_err->message);
						g_clear_error(&local_err);
						status = FALSE;
					}
				}
			} else {
				g_print("Error, no node found for file name: %s\n", (char *)i->data);
			}
		}

		g_list_free(to_delete);
		g_slist_free(remote_children);
		g_hash_table_destroy(hash_table);
	}

	return status;
}

// helper function: delete a local directory recursively
static gboolean delete_recursively(GFile *file, GError **error)
{
	gc_object_unref GFileEnumerator *e = g_file_enumerate_children(file, "standard::*",
						G_FILE_QUERY_INFO_NONE,
						NULL, error);
  if (!e)
    return FALSE;

	GFileInfo *fi;
	while ((fi = g_file_enumerator_next_file(e, NULL, NULL))) {
		GFileType type = g_file_info_get_file_type(fi);
		gc_free gchar *child = g_strconcat(g_file_get_path(file), "/", g_file_info_get_name(fi), NULL);
		if (type == G_FILE_TYPE_DIRECTORY) {
			if (!delete_recursively(g_file_new_for_path(child), error))
				return FALSE;
		} else {
			if (mega_debug & MEGA_DEBUG_APP)
				g_print("Deleting: %s\n", child);

			if (!g_file_delete(g_file_new_for_path(child), NULL, error))
				return FALSE;
		}
	}
	if (mega_debug & MEGA_DEBUG_APP)
		g_print("Deleting: %s\n", g_file_get_path(file));

	if (!g_file_delete(file, NULL, error))
		return FALSE;

	return TRUE;
}

// download operation

static gboolean dl_sync_file(struct mega_node *node, GFile *file, const gchar *remote_path)
{
	GError *local_err = NULL;
	gchar *local_path = g_file_get_path(file);
  // if the local timestamp is not available, fall back to the upload timestamp
  glong timestamp = node->local_ts > 0 ? node->local_ts : node->timestamp;

	if (g_file_query_exists(file, NULL)) {
    GFileType file_type = g_file_query_file_type(file, G_FILE_QUERY_INFO_NONE, NULL);

    // check whether the file is a directory
	  if (!opt_force && file_type == G_FILE_TYPE_DIRECTORY) {
			g_printerr("ERROR: Target is a directory, cannot overwrite (use --force): %s\n", remote_path);
			return FALSE;
		}

    // check special file types
	  if (file_type != G_FILE_TYPE_DIRECTORY && file_type != G_FILE_TYPE_REGULAR) {
			g_printerr("ERROR: Target is not a regular file, cannot overwrite: %s\n", remote_path);
			return FALSE;
		}

		gboolean doDownload = FALSE;

		GStatBuf fileStat;
		if (!g_stat(g_file_get_path(file), &fileStat)) {
			// size check
			if (node->size != fileStat.st_size) {
				doDownload = TRUE;
				if (mega_debug & MEGA_DEBUG_APP)
					g_print("File %s: sizes differ\n", remote_path);
			}

			// timestamp check
			if (!doDownload) {
				if (fileStat.st_mtime != timestamp) {
					doDownload = TRUE;
					// get local file timestamp
					if (mega_debug & MEGA_DEBUG_APP) {
						guchar local_ts_buf[20];
						g_snprintf(&local_ts_buf[0], 20, "%lu", fileStat.st_mtime);
						guchar remote_ts_buf[20];
						g_snprintf(&remote_ts_buf[0], 20, "%lu", timestamp);
						g_print("File %s: timestamp mismatch\n", remote_path);
						g_print("  Local file timestamp is: %s\n", &local_ts_buf[0]);
						g_print("  Remote timestamp is: %s\n", &remote_ts_buf[0]);
					}
				}
			}
		} else {
			g_printerr("ERROR: Unable to stat %s\n", g_file_get_path(file));
			return FALSE;
		}

		if (!doDownload) {
			if (mega_debug & MEGA_DEBUG_APP) {
				g_print("File %s appears identical, skipping\n", remote_path);
			}
			return FALSE;
		}

    if (!opt_quiet)
      g_print("R %s\n", g_file_get_path(file));

		if (!opt_dryrun) {
	    if (file_type == G_FILE_TYPE_DIRECTORY) {
        if (!delete_recursively(file, &local_err)) {
				  g_printerr("ERROR: Can't remove %s: %s\n", g_file_get_path(file), local_err->message);
				  return FALSE;
        }
      } else {
			  if (!g_file_delete(file, NULL, &local_err)) {
				  g_printerr("ERROR: Can't delete %s: %s\n", g_file_get_path(file), local_err->message);
				  return FALSE;
			  }
      }
		}
	}

	if (!opt_quiet)
		g_print("F %s\n", local_path);

	if (!opt_dryrun) {
		g_free(cur_file);
		cur_file = g_file_get_basename(file);

		if (!mega_session_get_compat(s, g_file_get_path(file), remote_path, &local_err)) {
			if (!opt_noprogress && tool_is_stdout_tty())
				g_print("\r" ESC_CLREOL);

			g_printerr("ERROR: Download failed for %s: %s\n", remote_path, local_err->message);
			g_clear_error(&local_err);
			return FALSE;
		}

		if (timestamp > 0) {
			struct utimbuf timbuf;
			timbuf.actime = timestamp;
			timbuf.modtime = timestamp;
			if (utime(g_file_get_path(file), &timbuf)) {
				g_printerr("ERROR: Failed to set file times on %s\n", g_file_get_path(file));
			}
		}

		if (!opt_noprogress && tool_is_stdout_tty())
			g_print("\r" ESC_CLREOL);
	}

	return TRUE;
}


static gboolean dl_sync_dir(struct mega_node *node, GFile *file, const gchar *remote_path)
{
	GError *local_err = NULL;
	GFileInfo *fi;
	GHashTable *hash_table = NULL;

	gc_free gchar *local_path = g_file_get_path(file);

  GFileType file_type = g_file_query_file_type(file, 0, NULL);

  // does the file exist?
  if (file_type != G_FILE_TYPE_UNKNOWN) {
    // regular file that needs to be replaced by a directory?
    if (file_type == G_FILE_TYPE_REGULAR) {
printf("Regular file\n");
      if (!opt_quiet)
        g_print("R %s\n", local_path);

  		if (!opt_dryrun) {
		    if (!g_file_delete(file, NULL, &local_err)) {
			    g_printerr("ERROR: Can't delete %s: %s\n", g_file_get_path(file), local_err->message);
			    return FALSE;
		    }
      }
    } else if (file_type != G_FILE_TYPE_DIRECTORY) {
	    g_printerr("ERROR: Target is not a directory, cannot write here: %s\n", local_path);
	    return FALSE;
    }
  }

  if (!g_file_query_exists(file, NULL)) {
    // file does not exist, create the directory
	  if (!opt_quiet)
		  g_print("D %s\n", local_path);

	  if (!opt_dryrun) {
		  if (!g_file_make_directory(file, NULL, &local_err)) {
			  g_printerr("ERROR: Can't create local directory %s: %s\n", local_path,
				     local_err->message);
			  g_clear_error(&local_err);
			  return FALSE;
		  }
	  }
  }

	if (opt_delete) {
		// get list of local children
		gc_object_unref GFileEnumerator *e = g_file_enumerate_children(file, "standard::*",
						  G_FILE_QUERY_INFO_NONE,
						  NULL, &local_err);
		if (!e) {
			g_printerr("ERROR: Can't read local directory %s: %s\n", g_file_get_path(file),
				   local_err->message);
			g_clear_error(&local_err);
			return FALSE;
		}

		hash_table = g_hash_table_new(&g_str_hash, &g_str_equal);

		// insert local file infos into hash table
		while ((fi = g_file_enumerator_next_file(e, NULL, NULL))) {
			const gchar *name = g_file_info_get_name(fi);
			g_hash_table_insert(hash_table, (gpointer)name, fi);
		}
	}

	// sync children
	gboolean status = TRUE;
	GSList *children = mega_session_get_node_chilren(s, node), *i;
	for (i = children; i; i = i->next) {
		struct mega_node *child = i->data;
		gc_free gchar *child_remote_path = g_strconcat(remote_path, "/", child->name, NULL);
		gc_object_unref GFile *child_file = g_file_get_child(file, child->name);

		if (opt_delete) {
			fi = (GFileInfo *)g_hash_table_lookup(hash_table, child->name);
			if (!g_hash_table_remove(hash_table, (gconstpointer)child->name))
				if (mega_debug & MEGA_DEBUG_APP)
					g_print("New file: %s\n", child->name);
			if (fi)
				g_object_unref(fi);
		}

		if (child->type == MEGA_NODE_FILE) {
			if (!dl_sync_file(child, child_file, child_remote_path))
				status = FALSE;
		} else {
			if (!dl_sync_dir(child, child_file, child_remote_path))
				status = FALSE;
		}
	}

	g_slist_free(children);

	if (opt_delete) {
		// get local leftovers
		GList *to_delete = g_hash_table_get_keys(hash_table), *i;
		for (i = to_delete; i; i = i->next) {
			fi = (GFileInfo *)g_hash_table_lookup(hash_table, i->data);
			if (fi) {
				GFileType type = g_file_info_get_file_type(fi);
				gc_free gchar *local_file = g_strconcat(g_file_get_path(file), "/", g_file_info_get_name(fi), NULL);

				g_print("R %s\n", local_file);

				if (!opt_dryrun) {
					if (type == G_FILE_TYPE_DIRECTORY) {
						if (!delete_recursively(g_file_new_for_path(local_file), &local_err)) {
							g_printerr("ERROR: Can't delete local directory %s: %s\n", local_file,
								   local_err->message);
							status = FALSE;
						}
					} else if (type == G_FILE_TYPE_REGULAR) {
						if (!g_file_delete(g_file_new_for_path(local_file), NULL, &local_err)) {
							g_printerr("ERROR: Can't delete local file %s: %s\n", local_file,
								   local_err->message);
							status = FALSE;
						}
					} else
						g_printerr("WARNING: Skipping special file %s\n", g_file_info_get_name(fi));
				}
				g_object_unref(fi);
			} else {
				g_print("Error, no info found for file name: %s\n", (char *)i->data);
			}
		}

		// free memory
		g_list_free(to_delete);
		g_hash_table_destroy(hash_table);
	}

	return status;
}

// main program

static int sync_main(int ac, char *av[])
{
	gc_object_unref GFile *local_file = NULL;
	gint status = 0;

	tool_init(&ac, &av, "- synchronize a local folder with a remote mega.nz folder", entries,
		  TOOL_INIT_AUTH | TOOL_INIT_UPLOAD_OPTS | TOOL_INIT_DOWNLOAD_OPTS);

	if (!opt_local_path || !opt_remote_path) {
		g_printerr("ERROR: You must specify local and remote paths\n");
		goto err;
	}

	if (opt_quiet) {
		opt_noprogress = TRUE;
		mega_debug = 0;
	}

	s = tool_start_session(TOOL_SESSION_OPEN);
	if (!s)
		goto err;

	mega_session_watch_status(s, status_callback, NULL);

	// check remote dir existence
	struct mega_node *remote_dir = mega_session_stat(s, opt_remote_path);
	if (!remote_dir) {
		g_printerr("ERROR: Remote directory not found %s\n", opt_remote_path);
		goto err;
	} else if (!mega_node_is_container(remote_dir)) {
		g_printerr("ERROR: Remote path must be a folder: %s\n", opt_remote_path);
		goto err;
	}

	// check local dir existence
	local_file = g_file_new_for_path(opt_local_path);

	if (opt_download) {
		if (!dl_sync_dir(remote_dir, local_file, opt_remote_path))
			goto err;
	} else {
		if (g_file_query_file_type(local_file, 0, NULL) != G_FILE_TYPE_DIRECTORY) {
			g_printerr("ERROR: Local directory not found %s\n", opt_local_path);
			goto err;
		}

		if (!up_sync_dir(local_file, local_file, opt_remote_path))
			status = 1;

		mega_session_save(s, NULL);
	}

	g_free(cur_file);
	tool_fini(s);
	return status;

err:
	g_free(cur_file);
	tool_fini(s);
	return 1;
}

const struct shell_tool shell_tool_sync = {
	.name = "sync",
	.main = sync_main,
	.usages = (char*[]){
		"[-n] [-q] [--force] [--no-progress] [--delete] --local <path> --remote <remotepath>",
		"[-n] [-q] [--force] [--no-progress] [--delete] --download --local <path> --remote <remotepath>",
		NULL
	},
};
