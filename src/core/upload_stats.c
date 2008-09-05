/*
 * $Id$
 *
 * Copyright (c) 2001-2003, Raphael Manfredi
 * Copyright (c) 2002, Michael Tesch
 *
 *----------------------------------------------------------------------
 * This file is part of gtk-gnutella.
 *
 *  gtk-gnutella is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  gtk-gnutella is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with gtk-gnutella; if not, write to the Free Software
 *  Foundation, Inc.:
 *      59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *----------------------------------------------------------------------
 */

/**
 * @ingroup core
 * @file
 *
 * Keep track of which files we send away, and how often.
 *
 * Statistics are kept by _FILENAME_ and file size, not by actual path,
 * so two files with the same name and size will be counted in the same
 * bin. I don't see this as a limitation because the user wouldn't be able
 * to differentiate the files anyway. This could be extended to keep the
 * entire path to each file and optionally show the entire path, but..
 *
 * The 'upload_history' file has the following format:
 *
 *	- "<url-escaped filename> <file size> <attempts> <completions>"
 *
 * @todo
 * TODO: Add a check to make sure that all of the files still exist(?)
 *       grey them out if they dont, optionally remove them from the
 *       stats list (when 'Clear Non-existent Files' is clicked).
 *
 * @author Michael Tesch
 * @date 2002
 * @author Raphael Manfredi
 * @date 2001-2003
 * @version 1.6
 */

#include "common.h"

RCSID("$Id$")

#include "upload_stats.h"
#include "share.h"

#include "if/bridge/c2ui.h"

#include "lib/atoms.h"
#include "lib/ascii.h"
#include "lib/file.h"
#include "lib/hashlist.h"
#include "lib/tm.h"
#include "lib/url.h"
#include "lib/urn.h"
#include "lib/utf8.h"
#include "lib/walloc.h"

#include "lib/override.h"		/* Must be the last header included */

static gboolean dirty = FALSE;
static gchar *stats_file;
static hash_list_t *upload_stats_list;
static GHashTable *upload_stats_by_sha1;

static gboolean 
ul_stats_eq(gconstpointer p, gconstpointer q)
{
	const struct ul_stats *a = p, *b = q;

	/* pathname is an atom */
	return a->pathname == b->pathname && b->size == b->size;
}

static guint
ul_stats_hash(gconstpointer p)
{
	const struct ul_stats *s = p;
  
	return g_str_hash(s->pathname) ^ s->size;
}

/**
 * Locate statistics structure for the file.
 *
 * If a SHA1 is given, we search by SHA1. Otherwise we search by (name, size)
 * and if the record is missing the SHA1, probably because it was not
 * available at the time of insertion, then it is added to the structure
 * and recorded as such.
 */
static struct ul_stats *
upload_stats_find(const struct sha1 *sha1, const gchar *pathname, guint64 size)
{
	struct ul_stats *s = NULL;

	if (upload_stats_list) {
		static const struct ul_stats zero_stats;
		struct ul_stats key;
		gconstpointer orig_key;

		g_assert(upload_stats_by_sha1);

		if (sha1) {
			s = g_hash_table_lookup(upload_stats_by_sha1, sha1);
			if (s)
				return s;		/* Found it by SHA1 */
		}

		key = zero_stats;
		key.pathname = atom_str_get(pathname);
		key.size = size;

		if (hash_list_contains(upload_stats_list, &key, &orig_key))
			s = deconstify_gpointer(orig_key);
		atom_str_free_null(&key.pathname);

		if (s && sha1) {
			/* Was missing from the by-SHA1 table? */
			g_assert(NULL == s->sha1);	/* Only possible when SHA1 unknown */

			s->sha1 = atom_sha1_get(sha1);
			gm_hash_table_insert_const(upload_stats_by_sha1, s->sha1, s);
		}
	}

	/* We guarantee the SHA1 is present in the record if known */
	g_assert(!(s && sha1) || s->sha1);

	return s;
}

static void
upload_stats_add(const gchar *pathname, filesize_t size, const char *name,
	guint32 attempts, guint32 complete, guint64 ul_bytes,
	time_t rtime, time_t dtime, const struct sha1 *sha1)
{
	static const struct ul_stats zero_stats;
	struct ul_stats *s;

	s = walloc(sizeof *s);
	*s = zero_stats;
	s->pathname = atom_str_get(pathname);
	s->filename = atom_str_get(name);
	s->size = size;
	s->attempts = attempts;
	s->complete = complete;
	s->norm = size > 0 ? 1.0 * ul_bytes / size : 0.0;
	s->bytes_sent = ul_bytes;
	s->rtime = rtime;
	s->dtime = dtime;
	s->sha1 = sha1 ? atom_sha1_get(sha1) : NULL;

	if (!upload_stats_list) {
		g_assert(!upload_stats_by_sha1);
		upload_stats_list = hash_list_new(ul_stats_hash, ul_stats_eq);
		upload_stats_by_sha1 = g_hash_table_new(sha1_hash, sha1_eq);
	}
	hash_list_append(upload_stats_list, s);
	if (s->sha1)
		gm_hash_table_insert_const(upload_stats_by_sha1, s->sha1, s);
	gcu_upload_stats_gui_add(s);
}

void
upload_stats_load_history(const gchar *ul_history_file_name)
{
	FILE *upload_stats_file;
	gchar line[FILENAME_MAX + 64];
	guint lineno = 0;

	gcu_upload_stats_gui_freeze();
	
	stats_file = g_strdup(ul_history_file_name);

	/* open file for reading */
	upload_stats_file = file_fopen_missing(ul_history_file_name, "r");
	if (upload_stats_file == NULL)
		goto done;

	/* parse, insert names into ul_stats_clist */
	while (fgets(line, sizeof(line), upload_stats_file)) {
		static const struct ul_stats zero_item;
		struct ul_stats item;
		struct sha1 sha1_buf;
		const char *p;
		size_t i;

		lineno++;
		if (line[0] == '#' || line[0] == '\n')
			continue;

		p = strchr(line, '\t');
		if (NULL == p)
			goto corrupted;

		line[p - line] = '\0';		/* line is now the URL-escaped file name */
		p++;

		/* URL-unescape in-place */
		if (!url_unescape(line, TRUE))
			goto corrupted;

		item = zero_item;
		item.pathname = line;

		for (i = 0; i < 8; i++) {
			guint64 v;
			int error;
			const gchar *endptr;

			p = skip_ascii_spaces(p);

			/* SVN versions up to 15322 had only 6 fields in the history */
			if (5 == i && '\0' == *p)
				break;

			switch (i) {
			case 7:
				/* We have a SHA1 or '*' if none known */
				if ('*' != *p) {
					size_t len = clamp_strlen(p, SHA1_BASE32_SIZE);
					
					error = !parse_base32_sha1(p, len, &sha1_buf);
					item.sha1 = error ? NULL : &sha1_buf;
				} else {
					error = FALSE;
				}
				p = skip_ascii_non_spaces(p);
				v = 0;
				break;
			default:
				v = parse_uint64(p, &endptr, 10, &error);
				p = deconstify_gchar(endptr);
			}

			if (error || !is_ascii_space(*endptr))
				goto corrupted;

			switch (i) {
			case 0: item.size = v; break;
			case 1: item.attempts = v; break;
			case 2: item.complete = v; break;
			case 3: item.bytes_sent |= ((guint64) (guint32) v) << 32; break;
			case 4: item.bytes_sent |= (guint32) v; break;
			case 5: item.rtime = MIN(v + (time_t) 0, TIME_T_MAX + (guint64) 0);
			case 6: item.dtime = MIN(v + (time_t) 0, TIME_T_MAX + (guint64) 0); 
			case 7: break;	/* Already stored above */
			default:
				g_assert_not_reached();
				goto corrupted;
			}
		}

		/* 
		 * We store the filenames UTF-8 encoded but the file might have been
		 * edited or corrupted.
		 */
		if (is_absolute_path(item.pathname)) {
			item.filename = lazy_filename_to_utf8_normalized(
						filepath_basename(item.pathname), UNI_NORM_NFC);
		} else {
			item.filename = lazy_unknown_to_utf8_normalized(
						filepath_basename(item.pathname), UNI_NORM_NFC, NULL);
		}

		if (upload_stats_find(NULL, item.pathname, item.size)) {
			g_warning("upload_stats_load_history():"
				" Ignoring line %u due to duplicate file.", lineno);
		} else if (upload_stats_find(item.sha1, item.pathname, item.size)) {
			g_warning("upload_stats_load_history():"
				" Ignoring line %u due to duplicate file.", lineno);
		} else {
			upload_stats_add(item.pathname, item.size, item.filename,
				item.attempts, item.complete, item.bytes_sent,
				item.rtime, item.dtime, item.sha1);
		}
		continue;

	corrupted:
		g_warning("upload statistics file corrupted at line %u.", lineno);
	}

	/* close file */
	fclose(upload_stats_file);

done:
	gcu_upload_stats_gui_thaw();
	return;
}

static void
upload_stats_dump_item(gpointer p, gpointer user_data)
{
	const struct shared_file *sf;
	FILE *out = user_data;
	struct ul_stats *s = p;
	char rtime_buf[TIME_T_DEC_BUFLEN];
	char dtime_buf[TIME_T_DEC_BUFLEN];
	const char *pathname;
	char *escaped;

	g_assert(NULL != s);

	sf = s->sha1 ? shared_file_by_sha1(s->sha1) : NULL;
	sf = SHARE_REBUILDING != sf ? sf : NULL;
	if (sf) {
		pathname = shared_file_path(sf);
	} else {
		pathname = s->pathname;
	}
	escaped = url_escape_cntrl(pathname);

	time_t_to_string_buf(s->rtime, rtime_buf, sizeof rtime_buf);
	time_t_to_string_buf(s->dtime, dtime_buf, sizeof dtime_buf);

	fprintf(out, "%s\t%s\t%u\t%u\t%lu\t%lu\t%s\t%s\t%s\n",
		escaped,
		uint64_to_string(s->size),
		s->attempts,
		s->complete,
		(unsigned long) (s->bytes_sent >> 32),
		(unsigned long) (s->bytes_sent & 0xffffffff),
		rtime_buf,
		dtime_buf,
		s->sha1 ? sha1_base32(s->sha1) : "*");

	if (escaped != pathname) {		/* File had escaped chars */
		G_FREE_NULL(escaped);
	}
}

/**
 * Save upload statistics to file.
 */
static void
upload_stats_dump_history(const gchar *ul_history_file_name)
{
	FILE *out;
	time_t now = tm_time();

	g_return_if_fail(ul_history_file_name);

	/* open file for writing */
	out = file_fopen(ul_history_file_name, "w");
	if (NULL == out) {
		return;
	}

	fprintf(out,
		"# THIS FILE IS AUTOMATICALLY GENERATED -- DO NOT EDIT\n"
		"#\n"
		"# Upload statistics saved on %s\n"
		"#\n"
		"\n"
		"#\n"
		"# Format is:\n"
		"#    File basename <TAB> size <TAB> attempts <TAB> completed\n"
		"#        <TAB> bytes_sent-high <TAB> bytes_sent-low\n"
		"#        <TAB> time of last request <TAB> time of last served chunk\n"
		"#        <TAB> SHA1 (\"*\" if unknown)\n"
		"#\n"
		"\n",
		timestamp_to_string(now));

	/*
	 * Don't check this sooner so that the file is cleared, if the user
	 * cleared the history.
	 */
	if (upload_stats_list) {
		/* for each element in uploads_stats_list, write out to hist file */
		hash_list_foreach(upload_stats_list, upload_stats_dump_item, out);
	}

	/* close file */
	fclose(out);
}

/**
 * Called on a periodic basis to flush the statistics to disk if changed
 * since last call.
 */
void
upload_stats_flush_if_dirty(void)
{
	if (!dirty)
		return;

	dirty = FALSE;

	if (NULL != stats_file)
		upload_stats_dump_history(stats_file);
	else
		g_warning("can't save upload statistics: no file name recorded");
}

/**
 * Make sure the filename associated to a SHA1 is given the name of
 * the shared file and no longer bears the name of the partial file.
 * This can happen when the partial file is seeded then the file is
 * renamed and shared.
 */
void
upload_stats_enforce_local_filename(const struct shared_file *sf)
{
	struct ul_stats *s;
	const struct sha1 *sha1;
	const gchar *name;

	if (!upload_stats_by_sha1)
		return;		/* Nothing known by SHA1 yet */

	sha1 = sha1_hash_available(sf) ? shared_file_sha1(sf) : NULL;

	if (!sha1)
		return;		/* File's SHA1 not known yet, nothing to do here */

	s = g_hash_table_lookup(upload_stats_by_sha1, sha1);

	if (NULL == s)
		return;							/* SHA1 not in stats, nothing to do */

	name = shared_file_name_nfc(sf);
	if (name == s->filename)			/* Both are string atoms */
		return;							/* Everything is fine */

	/*
	 * We need to update the filename to match the shared file.
	 */

	hash_list_remove(upload_stats_list, s);
	atom_str_change(&s->pathname, shared_file_path(sf));
	atom_str_change(&s->filename, name);
	hash_list_append(upload_stats_list, s);

	gcu_upload_stats_gui_update_name(s);
}

/**
 * Called when an upload starts.
 */
void
upload_stats_file_begin(const struct shared_file *sf)
{
	struct ul_stats *s;
	const gchar *pathname;
	filesize_t size;
	const struct sha1 *sha1;

	g_return_if_fail(sf);
	pathname = shared_file_path(sf);
	size = shared_file_size(sf);
	sha1 = sha1_hash_available(sf) ? shared_file_sha1(sf) : NULL;

	/* find this file in the ul_stats_clist */
	s = upload_stats_find(sha1, pathname, size);

	/* increment the attempted counter */
	if (NULL == s) {
		upload_stats_add(pathname, size, shared_file_name_nfc(sf),
			1, 0, 0, tm_time(), 0, sha1);
	} else {
		s->attempts++;
		s->rtime = tm_time();
		gcu_upload_stats_gui_update(s);
	}

	dirty = TRUE;		/* Request asynchronous save of stats */
}

/**
 * Add `comp' to the current completed count, and update the amount of
 * bytes transferred.  Note that `comp' can be zero.
 * When `update_dtime' is TRUE, we update the "done time", otherwise we
 * change the "last request time".
 *
 * If the row does not exist (race condition: deleted since upload started),
 * recreate one.
 */
static void
upload_stats_file_add(
	const struct shared_file *sf,
	gint comp, guint64 sent, gboolean update_dtime)
{
	const gchar *pathname = shared_file_path(sf);
	filesize_t size = shared_file_size(sf);
	struct ul_stats *s;
	const struct sha1 *sha1;

	g_assert(comp >= 0);

	sha1 = sha1_hash_available(sf) ? shared_file_sha1(sf) : NULL;

	/* find this file in the ul_stats_clist */
	s = upload_stats_find(sha1, pathname, size);

	/* increment the completed counter */
	if (NULL == s) {
		/* uh oh, row has since been deleted, add it: 1 attempt */
		upload_stats_add(pathname, size, shared_file_name_nfc(sf),
			1, comp, sent, tm_time(), tm_time(), sha1);
	} else {
		s->bytes_sent += sent;
		s->norm = 1.0 * s->bytes_sent / s->size;
		s->complete += comp;
		if (update_dtime)
			s->dtime = tm_time();
		else
			s->rtime = tm_time();
		gcu_upload_stats_gui_update(s);
	}

	dirty = TRUE;		/* Request asynchronous save of stats */
}

/**
 * Called when an upload is aborted, to update the amount of bytes transferred.
 */
void
upload_stats_file_aborted(const struct shared_file *sf, filesize_t done)
{
	g_return_if_fail(sf);

	if (done > 0)
		upload_stats_file_add(sf, 0, done, TRUE);
}

/**
 * Called when an upload completes.
 */
void
upload_stats_file_complete(const struct shared_file *sf, filesize_t done)
{
	g_return_if_fail(sf);

	upload_stats_file_add(sf, 1, done, TRUE);
}

/**
 * Called when an upload request is made.
 */
void
upload_stats_file_requested(const struct shared_file *sf)
{
	g_return_if_fail(sf);

	upload_stats_file_add(sf, 0, 0, FALSE);
}

void
upload_stats_prune_nonexistent(void)
{
	/* XXX */
	/* for each row, get the filename, check if filename is ? */
	g_warning("upload_stats_prune_nonexistent: not implemented!");
}

/**
 * Clear all the upload stats data structure.
 */
static void
upload_stats_free_all(void)
{
	if (upload_stats_list) {
		struct ul_stats *s;

		while (NULL != (s = hash_list_head(upload_stats_list))) {
			hash_list_remove(upload_stats_list, s);
			atom_str_free_null(&s->pathname);
			atom_str_free_null(&s->filename);
			if (s->sha1)
				g_hash_table_remove(upload_stats_by_sha1, s->sha1);
			atom_sha1_free_null(&s->sha1);
			wfree(s, sizeof *s);
		}
		hash_list_free(&upload_stats_list);
		g_hash_table_destroy(upload_stats_by_sha1);
		upload_stats_by_sha1 = NULL;
	}
	dirty = TRUE;
}

/**
 * Like upload_stats_free_all() but also clears the GUI.
 */
void
upload_stats_clear_all(void)
{
	gcu_upload_stats_gui_clear_all();
	upload_stats_free_all();
	if (stats_file) {
		upload_stats_dump_history(stats_file);
	}
}

/**
 * Called at shutdown time.
 */
void
upload_stats_close(void)
{
	upload_stats_dump_history(stats_file);
	upload_stats_free_all();
	G_FREE_NULL(stats_file);
}

/* vi: set ts=4 sw=4 cindent: */
