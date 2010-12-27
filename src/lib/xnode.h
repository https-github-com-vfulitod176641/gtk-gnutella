/*
 * $Id$
 *
 * Copyright (c) 2010, Raphael Manfredi
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
 * @ingroup lib
 * @file
 *
 * XML nodes, as items of an XML tree.
 *
 * @author Raphael Manfredi
 * @date 2010
 */

#ifndef _xnode_h_
#define _xnode_h_

#include "common.h"

/**
 * XML node types.
 */
typedef enum xnode_type {
	XNODE_T_ELEMENT = 0,		/**< An element markup */
	XNODE_T_COMMENT,			/**< A comment */
	XNODE_T_PI,					/**< A processing instruction */
	XNODE_T_TEXT,				/**< Text */
	XNODE_T_MAX
} xnode_type_t;

struct xnode;
typedef struct xnode xnode_t;

/**
 * Node traversal "leave" callback signature.
 */
typedef void (*xnode_cb_t)(xnode_t *xn, void *data);

/**
 * Node traversal "entry" callback signature.
 */
typedef gboolean (*xnode_cbe_t)(xnode_t *xn, void *data);


/*
 * Public interface.
 */

xnode_type_t xnode_type(const xnode_t *xn);
xnode_t *xnode_parent(const xnode_t *xn);
xnode_t *xnode_first_child(const xnode_t *xn);
xnode_t *xnode_next_sibling(const xnode_t *xn);
gboolean xnode_has_text(const xnode_t *xn);
gboolean xnode_is_text(const xnode_t *xn);
gboolean xnode_is_comment(const xnode_t *xn);
gboolean xnode_is_element(const xnode_t *xn);
gboolean xnode_is_processing_instruction(const xnode_t *xn);
const char *xnode_text(const xnode_t *xn);
const char *xnode_element_name(const xnode_t *xn);
const char *xnode_element_ns(const xnode_t *xn);
const char *xnode_pi_name(const xnode_t *xn);
const char *xnode_pi_target(const xnode_t *xn);

xnode_t *xnode_new_element(xnode_t *parent, const char *ns, const char *name);
xnode_t *xnode_new_comment(xnode_t *parent, const char *text);
xnode_t *xnode_new_text(xnode_t *parent, const char *text, gboolean verbatim);

void xnode_add_child(xnode_t *parent, xnode_t *node);
void xnode_add_first_child(xnode_t *parent, xnode_t *node);
void xnode_add_sibling(xnode_t *previous, xnode_t *node);

void xnode_add_namespace(xnode_t *e, const char *prefix, const char *uri);

gboolean xnode_prop_ns_set(xnode_t *element,
	const char *uri, const char *name, const char *value);
gboolean xnode_prop_set(xnode_t *element, const char *name, const char *value);
gboolean xnode_prop_unset(xnode_t *element, const char *name);
gboolean xnode_prop_ns_unset(xnode_t *element, const char *uri, const char *n);

gboolean xnode_prop_printf(xnode_t *element, const char *name,
	const char *fmt, ...) G_GNUC_PRINTF(3, 4);
gboolean xnode_prop_ns_printf(xnode_t *element,
	const char *uri, const char *name, const char *fmt, ...)
	G_GNUC_PRINTF(4, 5);

void xnode_tree_foreach(xnode_t *root, xnode_cb_t func, void *data);
void xnode_tree_enter_leave(xnode_t *root,
	xnode_cbe_t enter, xnode_cb_t leave, void *data);

void xnode_tree_free(xnode_t *root);
void xnode_tree_free_null(xnode_t **root_ptr);

#endif /* _xnode_h_ */

/* vi: set ts=4 sw=4 cindent: */
