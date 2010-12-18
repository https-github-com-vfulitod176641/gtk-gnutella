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
 * Name / Value pairs and tables.
 *
 * The name is necessary a string.
 * The value is an arbitrary data buffer.
 *
 * @author Raphael Manfredi
 * @date 2010
 */

#ifndef _nv_h_
#define _nv_h_

#include "common.h"

struct nv_pair;
typedef struct nv_pair nv_pair_t;

struct nv_table;
typedef struct nv_table nv_table_t;

/*
 * Public interface.
 */

nv_pair_t *nv_pair_make(char *name, const void *value, size_t length);
nv_pair_t *nv_pair_make_nocopy(char *name, const void *value, size_t length);
const char *nv_pair_name(const nv_pair_t *nvp);
void *nv_pair_value(const nv_pair_t *nvp);
const char *nv_pair_value_str(const nv_pair_t *nvp);
void *nv_pair_value_len(const nv_pair_t *nvp, size_t *retlen);
void nv_pair_free(nv_pair_t *nvp);
void nv_pair_free_null(nv_pair_t **nvp_ptr);
nv_pair_t *nv_pair_refcnt_inc(nv_pair_t *nvp);

nv_table_t *nv_table_make(void);
void nv_table_free(nv_table_t *nvt);
void nv_table_free_null(nv_table_t **nvt_ptr);
void nv_table_insert_pair(const nv_table_t *nvt, nv_pair_t *nvp);
void nv_table_insert(const nv_table_t *nvt, const char *, const void *, size_t);
void nv_table_insert_nocopy(const nv_table_t *nvt,
	const char *name, const void *value, size_t length);
gboolean nv_table_remove(const nv_table_t *nvt, const char *name);
nv_pair_t *nv_table_lookup(const nv_table_t *nvt, const char *name);
size_t nv_table_count(const nv_table_t *nvt);

#endif /* _nv_h_ */

/* vi: set ts=4 sw=4 cindent: */
