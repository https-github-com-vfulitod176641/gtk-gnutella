/*
 * $Id$
 *
 * Copyright (c) 2004, Raphael Manfredi
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
 * @file
 *
 * Support for geographic (country-level) IP mapping.
 */

#include "common.h"

RCSID("$Id$");

#include "geo_ip.h"
#include "settings.h"

#include "lib/atoms.h"
#include "lib/file.h"
#include "lib/misc.h"
#include "lib/glib-missing.h"
#include "lib/iprange.h"
#include "lib/walloc.h"
#include "lib/watcher.h"

#include "if/gnet_property_priv.h"
#include "if/bridge/c2ui.h"

#include "lib/override.h"		/* Must be the last header included */

static const gchar gip_file[] = "geo-ip.txt";
static const gchar gip_what[] = "Geographic IP mappings";

static gpointer geo_db;		/* The database of bogus CIDR ranges */

/*
 * Context used during ip_range_split() calls.
 */
struct range_context {
	guint32 ip1;				/* Original lower IP in global range */
	guint32 ip2;				/* Original upper IP in global range */
	gchar *country;				/* Country code (pointer within line) */
	gint count;					/* Amount of ranges we added, for stats */
	gchar *line;				/* The line from the input file */
	gint linenum;				/* Line number in input file, for errors */
};

/**
 * Free callback for values we insert in the database (country codes).
 */
static void
gip_free_value(gpointer value, gpointer udata)
{
	gchar *ccode = (gchar *) value;

	atom_str_free(ccode);
}

/**
 * Clone callback for values we insert.
 */
static gpointer
gip_clone_value(gpointer value)
{
	return atom_str_get(value);
}

/**
 * ip_range_split() callback.
 *
 * Insert IP range in database, linking it to the proper country code.
 */
static void
gip_add_cidr(guint32 ip, guint8 bits, gpointer udata)
{
	struct range_context *ctx = (struct range_context *) udata;
	iprange_err_t error;
	gchar *ccode;

	if (dbg > 4)
		printf("GEO adding %s/%d for \"%s\"\n",
			ip_to_gchar(ip), bits, ctx->line);

	ccode = atom_str_get(ctx->country);
	error = iprange_add_cidr(geo_db, ip, bits, ccode);

	switch (error) {
	case IPR_ERR_OK:
		break;
	case IPR_ERR_RANGE_OVERLAP:
		error = iprange_add_cidr_force(geo_db, ip, bits, ccode, NULL);
		if (error == IPR_ERR_OK) {
			g_warning("%s, line %d: "
				"entry \"%s\" (%s/%d) superseded earlier smaller range",
				gip_file, ctx->linenum, ctx->line, ip_to_gchar(ip), bits);
			break;
		}
		/* FALL THROUGH */
	default:
		g_warning("%s, line %d: rejected entry \"%s\" (%s/%d): %s",
			gip_file, ctx->linenum, ctx->line, ip_to_gchar(ip), bits,
			iprange_strerror(error));
		atom_str_free(ccode);
		return;
	}

	ctx->count++;
}

/**
 * Load geographic IP data from the supplied FILE.
 * Returns the amount of entries loaded.
 */
static gint
gip_load(FILE *f)
{
	gchar line[1024];
	gchar *p;
	gint linenum = 0;
	gchar *end;
	gint c;
	struct range_context ctx;

	geo_db = iprange_make(gip_free_value, gip_clone_value);
	ctx.count = 0;

	while (fgets(line, sizeof(line), f)) {
		linenum++;
		if (*line == '\0' || *line == '#')
			continue;

		/*
		 * Remove all trailing spaces in string.
		 * Otherwise, lines which contain only spaces would cause a warning.
		 */
	
		p = strchr(line, '\0');	
		while (--p >= line) {
			guchar c = (guchar) *p;
			if (!is_ascii_space(c))
				break;
			*p = '\0';
		}
		if ('\0' == *line)
			continue;

		/*
		 * Each line looks like:
		 *
		 *    15.0.0.0 - 15.130.191.255 fr
		 *
		 * So we don't have to parse the two IP addresses, and compute
		 * all the ranges they cover in order to insert them into
		 * the IP database.
		 */

		end = strchr(line, '-');
		if (end == NULL) {
			g_warning("%s, line %d: no IP address separator in \"%s\"",
				gip_file, linenum, line);
			continue;
		}

		if (!gchar_to_ip_strict(line, &ctx.ip1, NULL)) {
			g_warning("%s, line %d: invalid first IP in \"%s\"",
				gip_file, linenum, line);
			continue;
		}

		/*
		 * Skip spaces until the second IP.
		 */

		end++;			/* Go past the minus, parsing the second IP */

		while ((c = *end)) {
			if (!is_ascii_space(c))
				break;
			end++;
		}

		if (!gchar_to_ip_strict(end, &ctx.ip2, (gchar const **) &end)) {
			g_warning("%s, line %d: invalid second IP in \"%s\"",
				gip_file, linenum, line);
			continue;
		}

		/*
		 * Make sure the IP addresses are ordered correctly
		 */

		if (ctx.ip1 > ctx.ip2) {
			g_warning("%s, line %d: invalid IP order in \"%s\"",
				gip_file, linenum, line);
			continue;
		}

		/*
		 * Skip spaces until the country code.
		 */

		while ((c = *end)) {
			if (!is_ascii_space(c))
				break;
			end++;
		}

		if (c == '\0') {
			g_warning("%s, line %d: missing country code in \"%s\"",
				gip_file, linenum, line);
			continue;
		}

		if (strlen(end) != 2) {
			g_warning("%s, line %d: bad country code in \"%s\"",
				gip_file, linenum, line);
			continue;
		}

		ctx.country = end;
		ctx.line = line;
		ctx.linenum = linenum;

		/*
		 * Now compute the CIDR ranges between the ip1 and ip2 addresses
		 * and insert each range into the database, linking it to the
		 * country code.
		 */

		ip_range_split(ctx.ip1, ctx.ip2, gip_add_cidr, &ctx);
	}

	if (dbg) {
		iprange_stats_t stats;

		iprange_get_stats(geo_db, &stats);

		g_message("loaded %d geographical IP ranges", ctx.count);
		g_message("geo IP stats: count=%d level2=%d heads=%d enlisted=%d",
			stats.count, stats.level2, stats.heads, stats.enlisted);
	}

	return ctx.count;
}

/**
 * Watcher callback, invoked when the file from which we read the
 * geographic IP mappings changed.
 */
static void
gip_changed(const gchar *filename, gpointer udata)
{
	FILE *f;
	gchar buf[80];
	gint count;

	f = file_fopen(filename, "r");
	if (f == NULL)
		return;

	gip_close();
	count = gip_load(f);

	gm_snprintf(buf, sizeof(buf), "Reloaded %d geographic IP ranges.", count);
	gcu_statusbar_message(buf);
}

/**
 * Loads the geo-ip.txt into memory, choosing the first file we find
 * among the several places we look at, typically:
 *
 *    ~/.gtk-gnutella/geo-ip.txt
 *    /usr/share/gtk-gnutella/geo-ip.txt
 *    /home/src/gtk-gnutella/geo-ip.txt
 *
 * The selected file will then be monitored and a reloading will occur
 * shortly after a modification.
 */
static void
gip_retrieve(void)
{
	FILE *f;
	gint idx;
	gchar *filename;
#ifndef OFFICIAL_BUILD 
	file_path_t fp[3];
#else
	file_path_t fp[2];
#endif

	file_path_set(&fp[0], settings_config_dir(), gip_file);
	file_path_set(&fp[1], PRIVLIB_EXP, gip_file);
#ifndef OFFICIAL_BUILD 
	file_path_set(&fp[2], PACKAGE_SOURCE_DIR, gip_file);
#endif

	f = file_config_open_read_norename_chosen(
			gip_what, fp, G_N_ELEMENTS(fp), &idx);

	if (!f)
	   return;

	filename = make_pathname(fp[idx].dir, fp[idx].name);
	watcher_register(filename, gip_changed, NULL);
	G_FREE_NULL(filename);

	gip_load(f);
}

/**
 * Called on startup. Loads the bogons.txt into memory.
 */
void
gip_init(void)
{
	gip_retrieve();
}

/**
 * Frees all entries in the hostiles
 */
void
gip_close(void)
{
	iprange_free_each(geo_db, NULL);
	geo_db = NULL;
}

/**
 * Returns the country mapped to this IP address, as a two-letter ISO code,
 * or "??" when unknown.
 */
const gchar *
gip_country(guint32 ip)
{
	const gchar *code;

	code = iprange_get(geo_db, ip);

	return code == NULL ? "??" : code;
}

/* vi: set ts=4: */
