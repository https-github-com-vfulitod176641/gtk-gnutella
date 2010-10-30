/*
 * $Id$
 *
 * Copyright (c) 2005, Raphael Manfredi
 * Copyright (c) 2005, Martijn van Oosterhout <kleptog@svana.org>
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
 * Handles downloads of THEX data.
 *
 * @author Raphael Manfredi
 * @author Martijn van Oosterhout
 * @date 2005
 */

#include "common.h"

RCSID("$Id$")

#include <libxml/parser.h>                                                      
#include <libxml/tree.h>                                                        

#include "bsched.h"
#include "dime.h"
#include "downloads.h"
#include "rx_inflate.h"
#include "thex.h"
#include "thex_download.h"

#include "if/gnet_property_priv.h"

#include "lib/atoms.h"
#include "lib/ascii.h"
#include "lib/endian.h"
#include "lib/parse.h"
#include "lib/pmsg.h"
#include "lib/stringify.h"
#include "lib/tigertree.h"
#include "lib/halloc.h"
#include "lib/walloc.h"

#include "lib/override.h"	/* Must be the last header included */

#define THEX_DOWNLOAD_DEFAULT_SIZE	4096	/* Default data buffer size */
#define THEX_DOWNLOAD_MAX_SIZE		(260 * 1024)	/* 256 KiB + overhead */

struct thex_download {
	gpointer owner;					/**< Download owning us */
	rxdrv_t *rx;					/**< RX stack top */
	gnet_host_t host;				/**< Host we're browsing, for logging */
	char *data;						/**< Where payload data is stored */
	size_t data_size;				/**< Size of data buffer */
	size_t pos;						/**< Reading position */
	size_t max_size;				/**< Max size we'll read */
	const struct sha1 *sha1;		/**< SHA1 atom; refers to described file */
	const struct tth *tth;			/**< TTH atom; refers to described file */
	struct tth *leaves;				/**< g_memdup()ed leave TTHs */
	size_t num_leaves;				/**< number of leaves */
	filesize_t filesize;			/**< filesize of the described file */
	unsigned depth;					/**< depth of the hashtree (capped) */
	gboolean finished;
};

/** Get rid of the obnoxious (xmlChar *) */
static inline char *
xml_get_string(xmlNode *node, const char *id)
{
	return (char *) xmlGetProp(node, (const xmlChar *) id);
}

/**
 * Uses this to free strings returned by xml_get_string().
 */
static inline void
xml_string_free(char **p)
{
	g_assert(p);
	if (*p) {
		xmlFree(*p);
		*p = NULL;
	}
}

static inline const xmlChar *
string_to_xmlChar(const char *p)
{
	return (const xmlChar *) p;
}

/**
 * Initialize the THEX download context.
 */
struct thex_download *
thex_download_create(gpointer owner, gnet_host_t *host,
	const struct sha1 *sha1, const struct tth *tth, filesize_t filesize)
{
	static const struct thex_download zero_ctx;
	struct thex_download *ctx;

	g_return_val_if_fail(host, NULL);
	g_return_val_if_fail(sha1, NULL);
	g_return_val_if_fail(tth, NULL);

	ctx = walloc(sizeof *ctx);
	*ctx = zero_ctx;
	ctx->owner = owner;
	ctx->host = *host;			/* Struct copy */
	ctx->sha1 = atom_sha1_get(sha1);
	ctx->tth = atom_tth_get(tth);
	ctx->filesize = filesize;

	return ctx;
}

/**
 * Read data from the message buffer we just received.
 *
 * @return TRUE if there was an error.
 */
static gboolean
thex_download_data_read(struct thex_download *ctx, pmsg_t *mb)
{
	size_t size;
	
	g_assert(ctx);
	g_assert((NULL != ctx->data) ^ (0 == ctx->data_size));
	g_assert(ctx->pos <= ctx->data_size);

	while ((size = pmsg_size(mb)) > 0) {
		if (ctx->pos + size > ctx->max_size)
			return TRUE;

		if (size > ctx->data_size - ctx->pos) {
			ctx->data_size += MAX(size, ctx->data_size);
			ctx->data = hrealloc(ctx->data, ctx->data_size);
		}
		ctx->pos += pmsg_read(mb, &ctx->data[ctx->pos], size);
	}
	return FALSE;
}

/**
 * RX data indication callback used to give us some new Gnet traffic in a
 * low-level message structure (which can contain several Gnet messages).
 *
 * @return FALSE if an error occurred.
 */
static gboolean
thex_download_data_ind(rxdrv_t *rx, pmsg_t *mb)
{
	struct thex_download *ctx = rx_owner(rx);
	struct download *d;
	gboolean error;

	d = ctx->owner;
	download_check(d);

	/*
	 * When we receive THEX data with an advertised size, the remote
	 * end will simply stop emitting data when we're done and could maintain
	 * the HTTP connection alive.  Therefore, since we don't intend to
	 * issue any more request on that connection, we must check for completion.
	 *
	 * When chunked data is received (unknown size), the last chunk will
	 * trigger completion via an RX-callback invoked from the dechunking
	 * layer, but in that case it is harmless to make the call anyway.
	 */

	error = thex_download_data_read(ctx, mb);
	if (!error) {
		download_maybe_finished(d);
		download_check(d);
	}

	pmsg_free(mb);
	return !error && DOWNLOAD_IS_RUNNING(d);
}

/* XML helper functions */
static xmlNode * 
find_element_by_name(xmlNode *start, const char *name)
{
   xmlNode *cur_node;

    for (cur_node = start; cur_node; cur_node = cur_node->next) {
        if (XML_ELEMENT_NODE == cur_node->type) {
            if (0 == xmlStrcmp(cur_node->name, string_to_xmlChar(name))) {
				return cur_node;
			}
        }
    }
    return NULL;
}

static gboolean
verify_element(xmlNode *node, const char *prop, const char *expect)
{
	gboolean success = FALSE;
	char *value;
	
	value = STRTRACK(xml_get_string(node, prop));
  	if (NULL == value) {
		if (GNET_PROPERTY(tigertree_debug)) {
			g_message("TTH couldn't find property \"%s\" of node \"%s\"",
				prop, node->name);
		}
		goto finish;
	}
	if (0 != strcmp(value, expect)) {
		if (GNET_PROPERTY(tigertree_debug)) {
			g_message("TTH property %s/%s doesn't match expected value \"%s\", "
				"got \"%s\"",
				node->name, prop, expect, value);
		}
		goto finish;
	}
	success = TRUE;

finish:
	xml_string_free(&value);
	return success;
}

static char *
thex_download_handle_xml(struct thex_download *ctx,
	const char *data, size_t size)
{
	xmlNode *root, *hashtree, *node;
	xmlDocPtr doc = NULL;
	char *hashtree_id = NULL;
	gboolean success = FALSE;

	if (size <= 0) {
		if (GNET_PROPERTY(tigertree_debug)) {
			g_message("TTH XML record has no data");
		}
		goto finish;
	}
	
	doc = xmlReadMemory(data, size, "noname.xml", NULL, 0);
	if (NULL == doc) {
		if (GNET_PROPERTY(tigertree_debug)) {
			g_message("TTH cannot parse XML record");
		}
		goto finish;
	}
	root = xmlDocGetRootElement(doc);
	
  	hashtree = find_element_by_name(root, "hashtree");
	if (NULL == hashtree) {
		if (GNET_PROPERTY(tigertree_debug)) {
			g_message("TTH couldn't find hashtree element");
		}
		goto finish;
	}
	
	node = find_element_by_name(hashtree->children, "file");
	if (node) {
		if (!verify_element(node, "size", filesize_to_string(ctx->filesize)))
			goto finish;
		if (!verify_element(node, "segmentsize", THEX_SEGMENT_SIZE))
			goto finish;
	} else {
		if (GNET_PROPERTY(tigertree_debug)) {
			g_message("TTH couldn't find hashtree/file element");
		}
		goto finish;
	}

	node = find_element_by_name(hashtree->children, "digest");
	if (node) {
		if (!verify_element(node, "algorithm", THEX_HASH_ALGO))
			goto finish;
		if (!verify_element(node, "outputsize", THEX_HASH_SIZE))
			goto finish;
	} else {
		if (GNET_PROPERTY(tigertree_debug)) {
			g_message("TTH couldn't find hashtree/digest element");
		}
    	goto finish;
	}
  
	node = find_element_by_name(hashtree->children, "serializedtree");
	if (node) {
		char *value;
		int error;
		
		if (!verify_element(node, "type", THEX_TREE_TYPE))
    		goto finish;

		value = STRTRACK(xml_get_string(node, "uri"));
		if (NULL == value) {
			if (GNET_PROPERTY(tigertree_debug)) {
				g_message("TTH couldn't find property \"uri\" of node \"%s\"",
					node->name);
			}
			goto finish;
		}
		hashtree_id = h_strdup(value);
		xml_string_free(&value);

		value = STRTRACK(xml_get_string(node, "depth"));
		if (NULL == value) {
			if (GNET_PROPERTY(tigertree_debug)) {
				g_message("TTH couldn't find property \"depth\" of node \"%s\"",
					node->name);
			}
			goto finish;
		}
		
		ctx->depth = parse_uint16(value, NULL, 10, &error);
		error |= ctx->depth > tt_full_depth(ctx->filesize);
		if (error) {
			ctx->depth = 0;
			g_message("TTH bad value for \"depth\" of node \"%s\": \"%s\"",
				node->name, value);
		}
		xml_string_free(&value);
		if (error)
			goto finish;
	} else {
		if (GNET_PROPERTY(tigertree_debug))
			g_message("TTH couldn't find hashtree/serializedtree element");
		goto finish;
	}

	success = TRUE;

finish:
	if (doc) {
		xmlFreeDoc(doc);
		doc = NULL;
	}
	if (!success) {
		HFREE_NULL(hashtree_id);
	}
	return hashtree_id;
}

static gboolean
thex_download_handle_hashtree(struct thex_download *ctx,
	const char *data, size_t size)
{
	gboolean success = FALSE;
	size_t n_nodes, n_leaves, n, start;
	unsigned good_depth;
	const struct tth *leaves;
	struct tth tth;

	if (size <= 0) {
		if (GNET_PROPERTY(tigertree_debug)) {
			g_message("TTH hashtree record has no data");
		}
		goto finish;
	}
	if (size < TTH_RAW_SIZE) {
		if (GNET_PROPERTY(tigertree_debug)) {
			g_message("TTH hashtree record is too small");
		}
		goto finish;
	}
	if (size % TTH_RAW_SIZE) {
		if (GNET_PROPERTY(tigertree_debug)) {
			g_message("TTH hashtree has bad size");
		}
		goto finish;
	}
	memcpy(tth.data, data, TTH_RAW_SIZE);
	if (!tth_eq(&tth, ctx->tth)) {
		if (GNET_PROPERTY(tigertree_debug)) {
			g_message("TTH hashtree has different root hash %s",
				tth_base32(&tth));
		}
		goto finish;
	}

	n_nodes = size / TTH_RAW_SIZE;
	n_leaves = tt_node_count_at_depth(ctx->filesize, ctx->depth);

	/* Shareaza use a fixed depth of 9, allow one level less like others */
	good_depth = tt_good_depth(ctx->filesize);

	ctx->depth = MIN(ctx->depth, good_depth);
	if (n_nodes < n_leaves * 2 - 1) {
		ctx->depth = good_depth;
		n = tt_node_count_at_depth(ctx->filesize, ctx->depth);
		n = n * 2 - 1; /* All nodes, not just leaves */
		while (n > n_nodes) {
			n = (n + 1) / 2;
			ctx->depth--;
		}
		if (GNET_PROPERTY(tigertree_debug)) {
			g_message("TTH calculated depth of hashtree: %u", ctx->depth);
		}
		n_leaves = tt_node_count_at_depth(ctx->filesize, ctx->depth);
	}

	if (ctx->depth < good_depth) {
		if (GNET_PROPERTY(tigertree_debug)) {
			g_message("TTH tree depth (%u) is below the good depth (%u)",
				ctx->depth, good_depth);
		}
	}

	start = 0;
	n = n_leaves;
	while (n > 1) {
		n = (n + 1) / 2;
		start += n;
	}

	if (n_nodes < start + n_leaves) {
		if (GNET_PROPERTY(tigertree_debug)) {
			g_message("TTH hashtree has too few nodes "
				"(filesize=%s depth=%u nodes=%lu expected=%lu)",
				filesize_to_string(ctx->filesize),
				ctx->depth,
				(unsigned long) n_nodes,
				(unsigned long) n_leaves * 2 - 1);
		}
		goto finish;
	}
	
	STATIC_ASSERT(TTH_RAW_SIZE == sizeof(struct tth));
	leaves = (const struct tth *) &data[start * TTH_RAW_SIZE];

	tth = tt_root_hash(leaves, n_leaves);
	if (!tth_eq(&tth, ctx->tth)) {
		if (GNET_PROPERTY(tigertree_debug)) {
			g_message("TTH hashtree does not match root hash %s",
				tth_base32(&tth));
		}
		goto finish;
	}

	ctx->leaves = g_memdup(leaves, TTH_RAW_SIZE * n_leaves);
	ctx->num_leaves = n_leaves;
	success = TRUE;

finish:
	return success;
}

static void
thex_dump_dime_records(const GSList *records)
{
	const GSList *iter;

	for (iter = records; NULL != iter; iter = g_slist_next(iter)) {
		const struct dime_record *record;

		record = iter->data;
		g_assert(record);
		dump_hex(stderr, "THEX DIME record type",
			dime_record_type(record), dime_record_type_length(record));
		dump_hex(stderr, "THEX DIME record ID",
			dime_record_id(record), dime_record_id_length(record));
	}
}

static const struct dime_record *
dime_find_record(const GSList *records, const char *type, const char *id)
{
	size_t type_length, id_length;
	const GSList *iter;

	g_return_val_if_fail(type, NULL);

	type_length = type ? strlen(type) : 0;
	g_return_val_if_fail(type_length > 0, NULL);

	id_length = id ? strlen(id) : 0;
	
	for (iter = records; NULL != iter; iter = g_slist_next(iter)) {
		const struct dime_record *record;
		
		record = iter->data;
		g_assert(record);
		
		if (dime_record_type_length(record) != type_length)
			continue;
		if (0 != ascii_strncasecmp(dime_record_type(record), type, type_length))
			continue;
		if (id) {
			if (dime_record_id_length(record) != id_length)
				continue;
			if (0 != strncasecmp(dime_record_id(record), id, id_length))
				continue;
		}
		return record;
	}

	if (GNET_PROPERTY(tigertree_debug)) {
		g_message("TTH could not find record (type=\"%s\", id=%s%s%s)",
			type,
			id ? "\"" : "",
			id ? id : "<none>",
			id ? "\"" : "");
		thex_dump_dime_records(records);
	}

	return NULL;
}

gboolean
thex_download_finished(struct thex_download *ctx)
{
	GSList *records;
	gboolean success = FALSE;

	g_return_val_if_fail(ctx, FALSE);
	g_return_val_if_fail(!ctx->finished, FALSE);

	ctx->finished = TRUE;

	g_assert(ctx->pos <= ctx->data_size);
	ctx->data_size = ctx->pos;	/* Amount which is actually valid */

	records = dime_parse_records(ctx->data, ctx->data_size);
	if (records) {
		const struct dime_record *record;
		const char *data;
		char *hashtree_id;
		size_t size;
		
		record = dime_find_record(records, "text/xml", NULL);
		if (NULL == record) {
			if (GNET_PROPERTY(tigertree_debug)) {
				dump_hex(stderr, "THEX data", ctx->data, ctx->data_size);
			}
			goto finish;
		}

		data = dime_record_data(record);
		size = dime_record_data_length(record);
		hashtree_id = thex_download_handle_xml(ctx, data, size);
		if (NULL == hashtree_id) {
			if (GNET_PROPERTY(tigertree_debug)) {
				g_message("TTH could not determine hashtree ID");
				dump_hex(stderr, "THEX data", ctx->data, ctx->data_size);
			}
			/* Bug workaround:
			 * Try without an ID. GnucDNA 1.1.1.4 sends truncated XML with
			 * a missing closing tag.
			 */
		}

		record = dime_find_record(records, THEX_TREE_TYPE, hashtree_id);
		if (NULL == record && NULL != hashtree_id) {
			/* Bug workaround:
			 * Ignore the ID and fetch the first record with a matching
			 * type. BearShare 5.2 prepends a bogus double-quote to the ID.
			 */
			record = dime_find_record(records, THEX_TREE_TYPE, NULL);
		}
		HFREE_NULL(hashtree_id);

		if (NULL == record) {
			if (GNET_PROPERTY(tigertree_debug)) {
				dump_hex(stderr, "THEX data", ctx->data, ctx->data_size);
			}
			goto finish;
		}

		data = dime_record_data(record);
		size = dime_record_data_length(record);

		if (!thex_download_handle_hashtree(ctx, data, size))
			goto finish;

	} else {
		if (GNET_PROPERTY(tigertree_debug)) {
			g_message("TTH could not parse DIME records");
			dump_hex(stderr, "THEX data", ctx->data, ctx->data_size);
		}
		goto finish;
	}
	success = TRUE;

finish:
	dime_list_free(&records);
	HFREE_NULL(ctx->data);
	return success;
}

/***
 *** RX link callbacks
 ***/

static void
thex_rx_given(gpointer o, ssize_t r)
{
	struct thex_download *ctx = o;

	download_data_received(ctx->owner, r);
}

static G_GNUC_PRINTF(2, 3) void
thex_rx_error(gpointer o, const char *reason, ...)
{
	struct thex_download *ctx = o;
	va_list args;

	va_start(args, reason);
	download_stop_v(ctx->owner, GTA_DL_ERROR, reason, args);
	va_end(args);
}

static void
thex_rx_got_eof(gpointer o)
{
	struct thex_download *ctx = o;

	download_got_eof(ctx->owner);
}

static void
thex_rx_done(gpointer o)
{
	struct thex_download *ctx = o;

	download_rx_done(ctx->owner);
}

static const struct rx_link_cb thex_rx_link_cb = {
	thex_rx_given,		/* add_rx_given */
	thex_rx_error,		/* read_error */
	thex_rx_got_eof,	/* got_eof */
};

static const struct rx_chunk_cb thex_rx_chunk_cb = {
	thex_rx_error,		/* chunk_error */
	thex_rx_done,		/* chunk_end */
};

static const struct rx_inflate_cb thex_rx_inflate_cb = {
	NULL,				/* add_rx_inflated */
	thex_rx_error,		/* inflate_error */
};

/**
 * Prepare reception of THEX data by building an appropriate RX stack.
 *
 * @return TRUE if we may continue with the download.
 */
gboolean
thex_download_receive(struct thex_download *ctx,
	filesize_t content_length,
	gnet_host_t *host, struct wrap_io *wio, guint32 flags)
{
	g_assert(ctx != NULL);

	ctx->host = *host;				/* Struct copy */

	/*
	 * Freeing of the RX stack must be asynchronous: each time we establish
	 * a new connection, dismantle the previous stack.  Otherwise the RX
	 * stack will be freed when the corresponding download structure is
	 * reclaimed.
	 */

	if (ctx->rx != NULL) {
		rx_free(ctx->rx);
		ctx->rx = NULL;
	}

	/*
	 * If there is a Content-Length indication in the HTTP reply, it is
	 * supplied here and will be used as a limit of the data we'll read.
	 *
	 * If there was none (for instance if the output is chunked), then 0
	 * is given and we'll use a hardwired maximum.
	 */

	if (content_length > MAX_INT_VAL(size_t))
		return FALSE;

	ctx->max_size = content_length ?
		(size_t) content_length : THEX_DOWNLOAD_MAX_SIZE;

	{
		struct rx_link_args args;

		args.cb = &thex_rx_link_cb;
		args.bws = bsched_in_select_by_addr(gnet_host_get_addr(&ctx->host));
		args.wio = wio;

		ctx->rx = rx_make(ctx, &ctx->host, rx_link_get_ops(), &args);
	}

	if (flags & THEX_DOWNLOAD_F_CHUNKED) {
		struct rx_chunk_args args;

		args.cb = &thex_rx_chunk_cb;

		ctx->rx = rx_make_above(ctx->rx, rx_chunk_get_ops(), &args);
	}

	if (flags & THEX_DOWNLOAD_F_INFLATE) {
		struct rx_inflate_args args;

		args.cb = &thex_rx_inflate_cb;

		ctx->rx = rx_make_above(ctx->rx, rx_inflate_get_ops(), &args);
	}

	rx_set_data_ind(ctx->rx, thex_download_data_ind);
	rx_enable(ctx->rx);

	return TRUE;
}

/**
 * Fetch the I/O source of the RX stack.
 */
struct bio_source *
thex_download_io_source(struct thex_download *ctx)
{
	g_assert(ctx != NULL);
	g_assert(ctx->rx != NULL);

	return rx_bio_source(ctx->rx);
}

/**
 * Received data from outside the RX stack.
 */
void
thex_download_write(struct thex_download *ctx, char *data, size_t len)
{
	pdata_t *db;
	pmsg_t *mb;

	g_assert(ctx->rx != NULL);

	/*
	 * Prepare data buffer to feed the RX stack.
	 */

	db = pdata_allocb_ext(data, len, pdata_free_nop, NULL);
	mb = pmsg_alloc(PMSG_P_DATA, db, 0, len);

	/*
	 * The message is given to the RX stack, and it will be freed by
	 * the last function consuming it.
	 */

	rx_recv(rx_bottom(ctx->rx), mb);
}

/**
 * Disable the RX stack.
 */
void
thex_download_close(struct thex_download *ctx)
{
	g_assert(ctx != NULL);
	if (ctx->rx) {
		rx_disable(ctx->rx);
	}
}

/**
 * Terminate THEX download.
 */
void
thex_download_free(struct thex_download **ptr)
{
	struct thex_download *ctx = *ptr;

	if (ctx) {
		if (ctx->rx) {
			rx_free(ctx->rx);
			ctx->rx = NULL;
		}
		HFREE_NULL(ctx->data);
		G_FREE_NULL(ctx->leaves);
		atom_sha1_free_null(&ctx->sha1);
		atom_tth_free_null(&ctx->tth);
		wfree(ctx, sizeof *ctx);
		*ptr = NULL;
	}
}

const struct sha1 *
thex_download_get_sha1(const struct thex_download *ctx)
{
	g_return_val_if_fail(ctx, NULL);
	g_return_val_if_fail(ctx->sha1, NULL);
	return ctx->sha1;
}

const struct tth *
thex_download_get_tth(const struct thex_download *ctx)
{
	g_return_val_if_fail(ctx, NULL);
	g_return_val_if_fail(ctx->tth, NULL);
	return ctx->tth;
}

size_t
thex_download_get_leaves(const struct thex_download *ctx,
	const struct tth **leaves_ptr)
{
	g_return_val_if_fail(ctx, 0);
	g_return_val_if_fail(ctx->finished, 0);
	g_assert((0 != ctx->num_leaves) ^ (NULL == ctx->leaves));

	if (leaves_ptr) {
		*leaves_ptr = ctx->leaves;
	}
	return ctx->num_leaves;
}

/* vi: set ts=4 sw=4 cindent: */
