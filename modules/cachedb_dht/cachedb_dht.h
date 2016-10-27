/*
 * cluster cache system module
 *
 * Copyright (C) 2016 Cerghit Ionel
 *
 * This file is part of opensips, a free SIP server.
 *
 * opensips is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version
 *
 * opensips is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301  USA
 *
 * History:
 * --------
 *  2016-10-01  initial version (Cerghit Ionel)
 */

#ifndef _DHT_CACHE_
#define _DHT_CACHE_

#include "../../cachedb/cachedb.h"
#include "../../cachedb/cachedb_cap.h"
#include "../../bin_interface.h"
#include "../clusterer/api.h"


#define BIN_VERSION 1
#define INSERT_RING 1

#define RING_TABLE_SIZE 1024


struct machines_list {
   str val;
   time_t expires;
   struct machines_list *next;
};

struct register_cell
{
   str attr;
   struct machines_list* nodes;
   struct register_cell *next;
};

struct registers_table
{
   int size;
   struct register_cell **buckets;
   gen_lock_set_t *locks;
};

typedef struct {
	struct cachedb_id *id;
	unsigned int ref;
	struct cachedb_pool_con_t *next;
} dht_cache_con;

#endif
