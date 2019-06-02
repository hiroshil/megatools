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
#include <sys/xattr.h>
#include <errno.h>

#include "tools.h"
#include "shell.h"
#include "sjson.h"

static gchar *opt_remote_path;
static gchar *opt_local_path;
static gboolean opt_always;
static gboolean opt_delete;
static gboolean opt_delete_only;
static gboolean opt_download;
static gboolean opt_noprogress;
static gboolean opt_dryrun;
static gboolean opt_force;
static gboolean opt_ignore_errors;
static struct mega_session *s;

static int files_processed;
static int files_with_errors;
static int folders_processed;
static int elements_deleted;
static long int bytes_transferred;

static GOptionEntry entries[] = {
	{ "remote", 'r', 0, G_OPTION_ARG_STRING, &opt_remote_path, "Remote directory", "PATH" },
	{ "local", 'l', 0, G_OPTION_ARG_STRING, &opt_local_path, "Local directory", "PATH" },
	{ "always", 'a', 0, G_OPTION_ARG_NONE, &opt_always, "Always overwrite files on target", NULL},
	{ "delete", '\0', 0, G_OPTION_ARG_NONE, &opt_delete, "Delete missing files on target", NULL },
	{ "delete-only", '\0', 0, G_OPTION_ARG_NONE, &opt_delete_only, "Only delete missing files on target, do not copy", NULL },
	{ "download", 'd', 0, G_OPTION_ARG_NONE, &opt_download, "Download files from mega", NULL },
	{ "no-progress", '\0', 0, G_OPTION_ARG_NONE, &opt_noprogress, "Disable progress bar", NULL },
	{ "dryrun", 'n', 0, G_OPTION_ARG_NONE, &opt_dryrun, "Don't perform any actual changes", NULL },
	{ "force", '\0', 0, G_OPTION_ARG_NONE, &opt_force, "Overwrite directories on target with files", NULL },
	{ "ignore-errors", '\0', 0, G_OPTION_ARG_NONE, &opt_ignore_errors, "Ignore errors and continue with the next operation", NULL },
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
	// FALSE signals an error to the caller
	
	GError *local_err = NULL;
	gc_free gchar *local_path = g_file_get_path(file);

	files_processed++;

	GStatBuf st;
	if (g_stat(local_path, &st)) {
		tool_print_err("Unable to stat %s\n", local_path);
		files_with_errors++;
		return FALSE;
	}

	if (st.st_size <= 0) {
		tool_print_debug("Ignoring empty file %s\n", local_path);
		return TRUE;
	}

	struct mega_node *node = mega_session_stat(s, remote_path);
	if (node) {
		// check whether the node represents a directory
		if (!opt_force && node->type == MEGA_NODE_FOLDER) {
			tool_print_err("Target is a directory, cannot overwrite (use --force): %s\n", remote_path);
			files_with_errors++;
			return FALSE;
		}

		// if the local timestamp is not available, fall back to the upload timestamp
		glong timestamp = node->local_ts > 0 ? node->local_ts : node->timestamp;

		gboolean do_upload = opt_always;

		// size check
		if (!do_upload && node->size != st.st_size) {
			do_upload = TRUE;

			tool_print_debug("File %s: sizes differ\n", local_path);
		}

		// timestamp check
		if (!do_upload && st.st_mtime != timestamp) {
			do_upload = TRUE;

			tool_print_debug("File %s: timestamp mismatch\n", local_path);
			tool_print_debug("  Local file timestamp is: %lu\n", st.st_mtime);
			tool_print_debug("  Remote timestamp is: %lu\n", timestamp);
		}

		if (!do_upload) {
			tool_print_debug("File %s appears identical, skipping\n", local_path);
			return TRUE;
		}

		tool_print_info("R %s\n", remote_path);

		if (!opt_dryrun) {
			if (!mega_session_rm(s, remote_path, &local_err)) {
				tool_print_err("Can't remove %s: %s\n", remote_path, local_err->message);
				g_clear_error(&local_err);
				files_with_errors++;
				return FALSE;
			}
		}
	}

	tool_print_info("F %s\n", local_path);

	if (!opt_dryrun) {
		g_free(cur_file);
		cur_file = g_file_get_basename(file);

		if (!mega_session_put_compat(s, remote_path, local_path, &local_err)) {
			if (!opt_noprogress && tool_is_stdout_tty())
				g_print("\r" ESC_CLREOL);

			tool_print_err("Upload failed for %s: %s\n", local_path, local_err->message);
			g_clear_error(&local_err);
			files_with_errors++;
			return FALSE;
		}

		bytes_transferred += st.st_size;

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
	gc_free gchar *local_path = g_file_get_path(file);

	tool_print_debug("Synchronizing local folder %s to %s\n", local_path, remote_path);

	folders_processed++;

	if (!opt_delete_only && root != file) {
		struct mega_node *node = mega_session_stat(s, remote_path);
		// if the remote node is a file, it is deleted and replaced by the directory
		if (node && node->type == MEGA_NODE_FILE) {
			tool_print_info("R %s\n", remote_path);

			if (!opt_dryrun) {
				if (!mega_session_rm(s, remote_path, &local_err)) {
					tool_print_err("Can't remove %s: %s\n", remote_path, local_err->message);
					g_clear_error(&local_err);
					return FALSE;
				}
			}
		}

		if (!node) {
			tool_print_info("D %s\n", local_path);

			if (!opt_dryrun) {
				if (!mega_session_mkdir(s, remote_path, &local_err)) {
					tool_print_err("Can't create remote directory %s: %s\n", remote_path,
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
		hash_table = g_hash_table_new(&g_str_hash, &g_str_equal);

		// insert nodes into hash table indexed by name
		GSList *c;
		for (c = remote_children; c; c = c->next) {
			struct mega_node *n = c->data;
			g_hash_table_insert(hash_table, (gpointer)n->name, (gpointer)n);
		}
	}

	// sync children
	gc_object_unref GFileEnumerator *e =
		g_file_enumerate_children(file, "standard::*", G_FILE_QUERY_INFO_NONE, NULL, &local_err);
	if (!e) {
		tool_print_err("Can't read local directory %s: %s\n", local_path, local_err->message);
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
				tool_print_debug("New file: %s\n", name);
		}

		if (type == G_FILE_TYPE_DIRECTORY) {
			if (!up_sync_dir(root, child, child_remote_path))
				status = opt_ignore_errors;
		} else if (type == G_FILE_TYPE_REGULAR) {
			if (!opt_delete_only)
				if (!up_sync_file(root, child, child_remote_path))
					status = opt_ignore_errors;
		} else {
			gc_free gchar* child_path = g_file_get_path(child);
			tool_print_warn("Skipping special file %s\n", child_path);
		}

		g_object_unref(i);
		
		if (!status)
			break;
	}

	GList *to_delete = NULL;
	if (opt_delete && status) {
		// get leftovers on remote
		to_delete = g_hash_table_get_keys(hash_table);
		GList *i;
		for (i = to_delete; i; i = i->next) {
			struct mega_node *n = (struct mega_node *)g_hash_table_lookup(hash_table, i->data);
			if (n) {
				gc_free gchar *node_path = mega_node_get_path_dup(n);

				tool_print_info("R %s\n", node_path);

				if (!opt_dryrun) {
					if (!mega_session_rm(s, node_path, &local_err)) {
						tool_print_err("Can't remove %s: %s\n", node_path,
							local_err->message);
						g_clear_error(&local_err);
						status = opt_ignore_errors;
					} else
						elements_deleted++;
				}
			} else {
				tool_print_err("no node found for file name: %s\n", (char *)i->data);
			}
			
			if (!status)
				break;
		}
	}
	
	if (opt_delete) {
		g_list_free(to_delete);
		g_slist_free(remote_children);
		g_hash_table_destroy(hash_table);
	}

	return status;
}

// helper function: delete a local directory recursively
static gboolean delete_recursively(GFile *file, GError **error)
{
	gc_free gchar *file_path = g_file_get_path(file);

	gc_object_unref GFileEnumerator *e =
		g_file_enumerate_children(file, "standard::*", G_FILE_QUERY_INFO_NONE, NULL, error);
	if (!e)
		return FALSE;

	GFileInfo *fi;
	while ((fi = g_file_enumerator_next_file(e, NULL, NULL))) {
		GFileType type = g_file_info_get_file_type(fi);

		const gchar *file_name = g_file_info_get_name(fi);
		gc_free gchar *child = g_strconcat(file_path, "/", file_name, NULL);
		gc_object_unref GFile *child_file = g_file_new_for_path(child);

		if (type == G_FILE_TYPE_DIRECTORY) {
			if (!delete_recursively(child_file, error))
				return FALSE;
		} else {
			tool_print_debug("Deleting: %s\n", child);

			if (!g_file_delete(child_file, NULL, error))
				return FALSE;
		}
	}

	tool_print_debug("Deleting: %s\n", file_path);

	if (!g_file_delete(file, NULL, error))
		return FALSE;

	return TRUE;
}

// download operation

static gboolean dl_sync_file(struct mega_node *node, GFile *file, const gchar *remote_path)
{
	GError *local_err = NULL;
	gc_free gchar *local_path = g_file_get_path(file);

	files_processed++;

	// if the local timestamp is not available, fall back to the upload timestamp
	glong timestamp = node->local_ts > 0 ? node->local_ts : node->timestamp;

	if (g_file_query_exists(file, NULL)) {
		GFileType file_type = g_file_query_file_type(file, G_FILE_QUERY_INFO_NONE, NULL);

		// check whether the file is a directory
		if (!opt_force && file_type == G_FILE_TYPE_DIRECTORY) {
			tool_print_err("Target is a directory, cannot overwrite (use --force): %s\n", local_path);
			files_with_errors++;
			return FALSE;
		}

		// check special file types
		if (file_type != G_FILE_TYPE_DIRECTORY && file_type != G_FILE_TYPE_REGULAR) {
			tool_print_err("Target is not a regular file, cannot overwrite: %s\n", local_path);
			files_with_errors++;
			return FALSE;
		}

		gboolean do_download = opt_always;

		GStatBuf st;
		if (!g_stat(local_path, &st)) {
			// size check
			if (!do_download && node->size != st.st_size) {
				do_download = TRUE;
				tool_print_debug("File %s: sizes differ\n", remote_path);
			}

			// timestamp check
			if (!do_download && st.st_mtime != timestamp) {
				do_download = TRUE;
				tool_print_debug("File %s: timestamp mismatch\n", remote_path);
				tool_print_debug("  Local file timestamp is: %lu\n", st.st_mtime);
				tool_print_debug("  Remote timestamp is: %lu\n", timestamp);
			}
		} else {
			tool_print_err("Unable to stat %s\n", local_path);
			files_with_errors++;
			return FALSE;
		}

		if (!do_download) {
			tool_print_debug("File %s appears identical, skipping\n", remote_path);
			return TRUE;
		}

		tool_print_info("R %s\n", local_path);

		if (!opt_dryrun) {
			if (file_type == G_FILE_TYPE_DIRECTORY) {
				if (!delete_recursively(file, &local_err)) {
					tool_print_err("ERROR: Can't remove %s: %s\n", local_path, local_err->message);
					g_clear_error(&local_err);
					files_with_errors++;
					return FALSE;
				}
			} else {
				if (!g_file_delete(file, NULL, &local_err)) {
					tool_print_err("ERROR: Can't delete %s: %s\n", local_path, local_err->message);
					g_clear_error(&local_err);
					files_with_errors++;
					return FALSE;
				}
			}
		}
	}

	tool_print_info("F %s\n", remote_path);

	if (!opt_dryrun) {
		g_free(cur_file);
		cur_file = g_file_get_basename(file);

		if (!mega_session_get_compat(s, local_path, remote_path, &local_err)) {
			if (!opt_noprogress && tool_is_stdout_tty())
				g_print("\r" ESC_CLREOL);

			tool_print_err("Download failed for %s: %s\n", remote_path, local_err->message);
			g_clear_error(&local_err);
			files_with_errors++;
			return FALSE;
		}

		bytes_transferred += node->size;

		if (timestamp > 0) {
			struct utimbuf timbuf;
			timbuf.actime = timestamp;
			timbuf.modtime = timestamp;
			if (utime(local_path, &timbuf)) {
				tool_print_warn("Failed to set file times on %s: %s\n", local_path, g_strerror(errno));
			}
		}

		// set extended attributes if available
		if (node->xattrs) {
			const gchar *xattrs_node = s_json_get_member(node->xattrs, "xattrs");
			if (xattrs_node && s_json_get_type(xattrs_node) == S_JSON_TYPE_ARRAY) {
				gc_free gchar** f_elems = s_json_get_elements(xattrs_node);
				gchar** f_elem = f_elems;

				while (*f_elem) {
					if (s_json_get_type(*f_elem) == S_JSON_TYPE_OBJECT) {
						gc_free gchar *xattr_name = s_json_get_member_string(*f_elem, "n");
						if (xattr_name && strlen(xattr_name) > 0) {
							gc_free gchar *xattr_value_encoded = s_json_get_member_string(*f_elem, "v");
							gsize value_len;
							guchar* xattr_value = g_base64_decode(xattr_value_encoded, &value_len);

							if (setxattr(local_path, xattr_name, xattr_value, value_len, 0)) {
								// ignore this error if the file system does not support extended attributes
								if (errno != ENOTSUP) {
									tool_print_err("Failed to set extended attributes on %s: %s\n", local_path, g_strerror(errno));
									g_free(xattr_value);
									return FALSE;
								}
							}
							g_free(xattr_value);
						}
					}

					f_elem++;
				}
			} else {
				tool_print_info("%s: No extended attributes found\n", local_path);
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

	tool_print_debug("Synchronizing remote folder %s to %s\n", remote_path, local_path);

	folders_processed++;

	GFileType file_type = g_file_query_file_type(file, 0, NULL);

	// does the file exist?
	if (!opt_delete_only && file_type != G_FILE_TYPE_UNKNOWN) {
		// regular file that needs to be replaced by a directory?
		if (file_type == G_FILE_TYPE_REGULAR) {
			tool_print_info("R %s\n", local_path);

			if (!opt_dryrun) {
				if (!g_file_delete(file, NULL, &local_err)) {
					tool_print_err("Can't delete %s: %s\n", local_path,
							local_err->message);
					g_clear_error(&local_err);
					return FALSE;
				}
			}
		} else if (file_type != G_FILE_TYPE_DIRECTORY) {
			tool_print_err("Target is not a directory, cannot write here: %s\n", local_path);
			return FALSE;
		}
	}

	if (!opt_delete_only && !g_file_query_exists(file, NULL)) {
		// file does not exist, create the directory
		tool_print_info("D %s\n", remote_path);

		if (!opt_dryrun) {
			if (!g_file_make_directory(file, NULL, &local_err)) {
				tool_print_err("Can't create local directory %s: %s\n", local_path,
						local_err->message);
				g_clear_error(&local_err);
				return FALSE;
			}
		}
	}

	// can only check for files to delete when the folder exists
	gboolean check_delete = opt_delete && g_file_query_exists(file, NULL);

	if (check_delete) {
		// get list of local children
		gc_object_unref GFileEnumerator *e =
			g_file_enumerate_children(file, "standard::*", G_FILE_QUERY_INFO_NONE, NULL, &local_err);
		if (!e) {
			tool_print_err("Can't read local directory %s: %s\n", local_path,
				local_err->message);
			g_clear_error(&local_err);
			return FALSE;
		}

		hash_table = g_hash_table_new(&g_str_hash, &g_str_equal);

		// insert local file infos into hash table
		while ((fi = g_file_enumerator_next_file(e, NULL, NULL))) {
			const gchar *name = g_file_info_get_name(fi);
			g_hash_table_insert(hash_table, (gpointer)name, fi);
			// file info fi is owned by hash table, will be freed later on
			// name is a pointer inside fi, so will need no explicit freeing
		}

		g_file_enumerator_close(e, NULL, &local_err);
		if (local_err) {
			tool_print_err("ERROR: Unable to close file enumerator on %s: %s\n", local_path,
				local_err->message);
			g_clear_error(&local_err);
			return FALSE;
		}
	}

	// sync children
	gboolean status = TRUE;
	GSList *children = mega_session_get_node_chilren(s, node), *i;
	for (i = children; i; i = i->next) {
		struct mega_node *child = i->data;
		gc_free gchar *child_remote_path = g_strconcat(remote_path, "/", child->name, NULL);
		gc_object_unref GFile *child_file = g_file_get_child(file, child->name);

		if (check_delete) {
			fi = (GFileInfo *)g_hash_table_lookup(hash_table, child->name);
			if (!g_hash_table_remove(hash_table, (gconstpointer)child->name))
				tool_print_debug("New file: %s\n", child->name);
			if (fi)
				g_object_unref(fi);
		}

		if (child->type == MEGA_NODE_FILE) {
			if (!opt_delete_only)
				if (!dl_sync_file(child, child_file, child_remote_path))
					status = opt_ignore_errors;
		} else {
			if (!dl_sync_dir(child, child_file, child_remote_path))
				status = opt_ignore_errors;
		}
		
		if (!status)
			break;
	}

	g_slist_free(children);

	GList *to_delete = NULL;
	if (check_delete && status) {
		// get local leftovers
		to_delete = g_hash_table_get_keys(hash_table);
		GList *i;
		for (i = to_delete; i; i = i->next) {
			fi = (GFileInfo *)g_hash_table_lookup(hash_table, i->data);
			if (fi) {
				gc_free gchar *local_file =
					g_strconcat(local_path, "/", g_file_info_get_name(fi), NULL);

				tool_print_info("R %s\n", local_file);

				if (!opt_dryrun) {
					GFileType type = g_file_info_get_file_type(fi);
					gc_object_unref GFile *delete_file = g_file_new_for_path(local_file);
					if (type == G_FILE_TYPE_DIRECTORY) {
						if (!delete_recursively(delete_file, &local_err)) {
							tool_print_err("Can't delete local directory %s: %s\n",
								local_file, local_err->message);
							g_clear_error(&local_err);
							status = opt_ignore_errors;
						} else
							elements_deleted++;
					} else if (type == G_FILE_TYPE_REGULAR) {
						if (!g_file_delete(delete_file, NULL, &local_err)) {
							tool_print_err("Can't delete local file %s: %s\n",
								local_file, local_err->message);
							g_clear_error(&local_err);
							status = opt_ignore_errors;
						} else
							elements_deleted++;
					} else
						tool_print_warn("Skipping special file %s\n", local_file);
				}
				g_object_unref(fi);
			} else {
				tool_print_err("no info found for file name: %s\n", (char *)i->data);
			}
			
			if (!status)
				break;
		}
	}
	
	if (check_delete) {
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
	gint status = 1;
	clock_t p_start, p_end;

	tool_init(&ac, &av, "- synchronize a local directory with a remote one", entries,
			TOOL_INIT_AUTH | TOOL_INIT_UPLOAD_OPTS | TOOL_INIT_DOWNLOAD_OPTS);

	if (!opt_local_path || !opt_remote_path) {
		tool_print_err("You must specify local and remote paths\n");
		goto out;
	}

	if (opt_delete_only && opt_always) {
		tool_print_err("Options --delete-only and -a (--always) cannot be used at the same time\n");
		goto out;
	}

	p_start = g_get_monotonic_time();

	s = tool_start_session(TOOL_SESSION_OPEN);
	if (!s)
		goto out;

	mega_session_watch_status(s, status_callback, NULL);

	// check remote dir existence
	struct mega_node *remote_dir = mega_session_stat(s, opt_remote_path);
	if (!remote_dir) {
		tool_print_err("Remote directory not found %s\n", opt_remote_path);
		goto out;
	} else if (!mega_node_is_container(remote_dir)) {
		tool_print_err("Remote path must point to a directory: %s\n", opt_remote_path);
		goto out;
	}

	// check local dir existence
	local_file = g_file_new_for_path(opt_local_path);

	// --delete-only implies --delete
	if (opt_delete_only)
		opt_delete = TRUE;

	if (opt_download) {
		if (dl_sync_dir(remote_dir, local_file, opt_remote_path))
			status = 0;
	} else {
		if (g_file_query_file_type(local_file, 0, NULL) != G_FILE_TYPE_DIRECTORY) {
			tool_print_err("Local directory not found or not a directory: %s\n", opt_local_path);
			goto out;
		}

		if (up_sync_dir(local_file, local_file, opt_remote_path))
			status = 0;

		mega_session_save(s, NULL);
	}

	p_end = g_get_monotonic_time();
	long int duration_s = ((double)(p_end - p_start)) / 1000000;
	if (duration_s < 1)
		duration_s = 1;
	int avg_bytes_per_s = ((double)bytes_transferred) / duration_s;

	tool_print_info("Processed %d file(s) in %d folder(s). %d file(s) had errors.\n", files_processed, folders_processed, files_with_errors);
	if (elements_deleted > 0)
		tool_print_info("Deleted %d file(s) or folder(s).\n", elements_deleted);
	tool_print_info("Transferred %lu bytes in %lu second(s) (avg. %d bytes/s).\n", bytes_transferred, duration_s, avg_bytes_per_s);

	if (files_with_errors > 0)
		tool_print_info("One or more error(s) occurred. Please see previous output.\n");

out:
	g_free(cur_file);
	tool_fini(s);
	return status;
}

const struct shell_tool shell_tool_sync = {
	.name = "sync",
	.main = sync_main,
	.usages = (char *[]){
		"[-n] [--force] [--no-progress] [--delete] [--delete-only | --always] [--ignore-errors] --local <path> --remote <remotepath>",
		"[-n] [--force] [--no-progress] [--delete] [--delete-only | --always] [--ignore-errors] --download --local <path> --remote <remotepath>",
		NULL
	},
};
