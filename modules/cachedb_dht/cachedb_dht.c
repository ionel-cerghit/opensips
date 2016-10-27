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

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "cachedb_dht.h"
#include "../../sr_module.h"
#include "../../dprint.h"
#include "../../timer.h"
#include "../../error.h"
#include "../../ut.h"
#include "../../mem/mem.h"
#include "../../mem/shm_mem.h"
#include "../../mod_fix.h"
#include "../../mi/tree.h"
#include "../../hash_func.h"

static int mod_init(void);
static int child_init(int rank);
static void destroy(void);
int init_register_table(void);
int receive_partial_ucontact_insert(void);
void receive_binary_packet(enum clusterer_event ev, int packet_type,
				struct receive_info *ri, int cluster_id, int src_id, int dest_id);
int is_local_record(str *aor);
int dht_single_fetch(cachedb_con *con,str* attr, str* res);
int dht_insert(cachedb_con *con,str* attr, str* value, int expires);
static int create_table(void);

str dht_mod_name = str_init("dht_cluster");
int ring_size = 5;
int replication_factor = 2;
int my_id = -1;
uint64_t last_updated;
int refresh_interval = 0;
int cluster_id = 0;
int repl_auth_check = 0;
struct registers_table *ring_table;
struct clusterer_binds clusterer_api;
clusterer_node_t **ordered_nodes;
clusterer_node_t *cluster_list;

#define REPLICATION_OFFSET HEADER_SIZE + 2 * LEN_FIELD_SIZE + dht_mod_name.len + CMD_FIELD_SIZE 
#define KEY_OFFSET HEADER_SIZE +  LEN_FIELD_SIZE + dht_mod_name.len + CMD_FIELD_SIZE

static param_export_t params[]={
	{ "dht_ring_size",   INT_PARAM, &ring_size },
	{ "replication_factor", INT_PARAM, &replication_factor },
	{ "refresh_interval",     INT_PARAM, &refresh_interval },
	{ "cluster_id",     INT_PARAM, &cluster_id },
	{ "dht_repl_auth_check",     INT_PARAM, &repl_auth_check },
	{0,0,0}
};

static dep_export_t deps = {
	{ /* OpenSIPS module dependencies */
		{ MOD_TYPE_DEFAULT, "clusterer", DEP_ABORT },
		{ MOD_TYPE_NULL, NULL, 0 },
	},
	{ /* modparam dependencies */
		{ NULL, NULL },
	},
};


/** module exports */
struct module_exports exports= {
	"cachedb_dht",               /* module name */
	MOD_TYPE_CACHEDB,/* class of this module */
	MODULE_VERSION,
	DEFAULT_DLFLAGS,            /* dlopen flags */
	&deps,            /* OpenSIPS module dependencies */
	0,                       /* exported functions */
	0,                          /* exported async functions */
	params,                     /* exported parameters */
	0,                          /* exported statistics */
	0,                    /* exported MI functions */
	0,                          /* exported pseudo-variables */
	0,                          /* extra processes */
	mod_init,                   /* module initialization function */
	(response_function) 0,      /* response handling function */
	(destroy_function) destroy, /* destroy function */
	child_init                  /* per-child init function */
};



dht_cache_con* dht_new_connection(struct cachedb_id* id)
{
	dht_cache_con *con;

	if (id == NULL) {
		LM_ERR("null db_id\n");
		return 0;
	}

	if (id->flags != CACHEDB_ID_NO_URL) {
		LM_ERR("bogus url for local cachedb\n");
		return 0;
	}

	con = pkg_malloc(sizeof(dht_cache_con));
	if (con == NULL) {
		LM_ERR("no more pkg\n");
		return 0;
	}

	memset(con,0,sizeof(dht_cache_con));
	con->id = id;
	con->ref = 1;

	return con;
}

cachedb_con *dht_cache_init(str *url)
{
	return cachedb_do_init(url,(void *)dht_new_connection);
}

void dht_free_connection(cachedb_pool_con *con)
{
	pkg_free(con);
}

void dht_cache_destroy(cachedb_con *con)
{	
	cachedb_do_close(con,dht_free_connection);
}


/**
 * init module function
 */
static int mod_init(void)
{
	cachedb_engine cde;
	cachedb_con *con;
	str url=str_init("dht://");
	str name=str_init("dht");

	if(init_register_table() < 0)
	{
		LM_ERR("failed to initialize dht table\n");
		return -1;
	}

	/* register the cache system */
	cde.name = dht_mod_name;

	cde.cdb_func.init = dht_cache_init;
	cde.cdb_func.destroy = dht_cache_destroy;
	cde.cdb_func.get = dht_single_fetch;
	cde.cdb_func.set = dht_insert;

	/*if(cache_clean_period <= 0 )
	{
		LM_ERR("Wrong parameter cache_clean_period - need a positive value\n");
		return -1;
	}*/

	if( register_cachedb(&cde)< 0)
	{
		LM_ERR("failed to register to core memory store interface\n");
		return -1;
	}

	/* insert connection for script */
	con = dht_cache_init(&url);
	if (con == NULL) {
		LM_ERR("failed to init connection for script\n");
		return -1;
	}

	if (cachedb_put_connection(&name,con) < 0) {
		LM_ERR("failed to insert connection for script\n");
		return -1;
	}

	/* register timer to delete the expired entries */
	//register_timer("dhtcache-expire",localcache_clean, 0,
	//	cache_clean_period, TIMER_FLAG_DELAY_ON_DELAY);

	if( !load_clusterer_api(&clusterer_api)!=0){
		LM_DBG("failed to find clusterer API - is clusterer module loaded?\n");
		return -1;
	}

	if ( clusterer_api.register_module(dht_mod_name.s,
		receive_binary_packet, repl_auth_check, &cluster_id, 1) < 0) {
		LM_ERR("cannot register binary packet callback to clusterer module!\n");
		return -1;
	}

	return 0;
}

/**
 * Initialize children
 */
static int child_init(int rank)
{
	return 0;
}

/*
 * destroy function
 */
static void destroy(void)
{
	//TODO dealoc entries
	lock_set_dealloc(ring_table->locks);
	shm_free(ring_table->buckets);
	shm_free(ring_table);

}

void print_register_table (void) {
	char buff[10000], aux[100];
	int i;
	struct register_cell *cell;
	struct machines_list *nod;

	buff[0] = 0;

	for(i = 0; i < ring_table->size; i++){
		if(!ring_table->buckets[i])
			continue;
		strcat(buff, "cell [");
		for (cell = ring_table->buckets[i]; cell; cell = cell->next) {
			sprintf(aux,"(%.*s) -> [", cell->attr.len, cell->attr.s);
			strcat(buff,aux);
			for (nod = cell->nodes; nod; nod=nod->next) {
				sprintf(aux,"(%.*s) ", nod->val.len, nod->val.s);
				strcat(buff,aux);
			}
			strcat(buff, "]\n");
		}
		strcat(buff, "]\n");
	}

	LM_DBG("XXX %s", buff);
}

int init_register_table(void) {
	int size = RING_TABLE_SIZE;
	ring_table = shm_malloc(sizeof(struct registers_table));

	if(!ring_table){
		LM_ERR("cannot allocate shared memory\n");
		return -1;
	}

	ring_table->size = size;
	ring_table->buckets = shm_malloc(size * sizeof(struct register_cell *));
	if (!ring_table->buckets) {
		LM_ERR("cannot allocate shared memory\n");
		shm_free(ring_table);
		return -1;
	}

	memset(ring_table->buckets, 0 , size * sizeof(struct register_cell *));

	if ( ((ring_table->locks = lock_set_alloc(size)) == 0) || !lock_set_init(ring_table->locks)){
		LM_ERR("cannot allocate locks\n");
		if (ring_table->locks){
			lock_set_dealloc(ring_table->locks);
		}
		shm_free(ring_table->buckets);
		shm_free(ring_table);
		return -1;
	}

	return 0;
}

#define min(a,b)  ((a)<(b))?(a):(b)

static int str_cmp(str s1, str s2)
{
	int ret;

	ret = strncmp( s1.s, s2.s, min( s1.len, s2.len) );

	if( ret == 0)
		ret =  s1.len -  s2.len;

	return ret;
}

int insert_new_machine_register(time_t expires, str*  val, struct register_cell *cell){
	struct machines_list *nod;
	nod = shm_malloc(sizeof(struct machines_list));

	if(!nod){
		LM_ERR("no more shared memory\n");
		return -1;
	}

	shm_str_dup(&nod->val, val);
	nod->expires = expires;
	nod->next = cell->nodes;
	cell->nodes = nod;
	return 0;
}

int find_local_ring(str *attr, str *val) {
	unsigned int hash, found = 0;
	struct register_cell *cell;
	struct machines_list *nod;

	hash = core_hash(attr, 0, 0);
	hash = hash % ring_table->size;

	lock_set_get(ring_table->locks, hash);
	for (cell = ring_table->buckets[hash]; cell; cell = cell->next) {
		if (str_cmp(cell->attr, *attr) == 0) {
			found = 1;
			for (nod = cell->nodes; nod; nod=nod->next)
				if(nod->expires > time(0)){
					pkg_str_dup(&nod->val, val);
					break;
				}
				else {
					//TODO delete the thing while were at it
				}
			break;
		}
	}

	lock_set_release(ring_table->locks, hash);

	if(found) 
		return 0;
	else
		return -1;
}

int insert_local_ring(str *attr, time_t expires, str *val) {
	unsigned int hash, found = 0;
	struct register_cell *cell;
	struct machines_list *nod;

	hash = core_hash(attr, 0, 0);
	hash = hash % ring_table->size;
	LM_DBG("HHH hash is: %u\n", hash);

	lock_set_get(ring_table->locks, hash);
	for (cell = ring_table->buckets[hash]; cell; cell = cell->next) {
		if (str_cmp(cell->attr, *attr) == 0){
			found = 1;
			break;
		}
	}

	if (found) {	
		//check if we have a newer register from same machine and update it
		for (nod = cell->nodes; nod; nod=nod->next) {
			if (str_cmp(nod->val, *val) == 0) {
				nod->expires = expires;
				break;
			}
		}

		if (!nod) { // a new val for the key
			if (insert_new_machine_register(expires, val, cell)) {
				goto err;
			}
		}
	} else {
		ring_table->buckets[hash] = shm_malloc(sizeof(struct register_cell));
		if (!ring_table->buckets[hash]) {
			goto err;
		}
		cell = ring_table->buckets[hash];
		shm_str_dup(&cell->attr, attr);
		cell->next = NULL;
		if(insert_new_machine_register(expires, val, cell)){
			goto err;
		}
	}
	lock_set_release(ring_table->locks, hash);
	print_register_table();

	return 0;

err:
	LM_ERR("no more shared memory");
	lock_set_release(ring_table->locks, hash);
	return -1;
}

int is_local_record(str *attr){
	unsigned int hash;
	int i;

	if (!attr || !attr->s || !attr->len){
		LM_DBG("attr field not initialized");
		return 0;
	}

	hash = core_hash(attr,0,0);
	hash = hash%ring_size;
	if (!ordered_nodes){ 
		create_table();
	}
	
	if(my_id < 0){
		my_id =clusterer_api.get_my_id();
	}
	LM_DBG("MMM hash %d, me %d\n", hash, my_id);
	for(i = 0; i < replication_factor; i++){
		if(ordered_nodes[hash]->node_id == my_id){
			LM_DBG("MMM rep %d me %d\n", i, my_id);
			return 1;
		}
		hash = ordered_nodes[hash]->node_id;
		hash = (hash + 1)%ring_size;
	}

	return 0;
}

static void print_destination_vector(void){
	char buf[1000];
	char gigi[100];
	int i;

	memset(buf, 0, 1000);
	for (i = 0; i < ring_size; i++){
		sprintf(gigi,"(%d->%d)",i, ordered_nodes[i]->node_id);
		strcat(buf,gigi);
	}
	LM_DBG("XXX: %s\n",buf);
}

static int create_table(void) {
	clusterer_node_t *value, *my_node;
	int i;

	cluster_list =  clusterer_api.get_nodes(cluster_id);
	if (!cluster_list) {
		LM_ERR("could not get the list of nodes");
	}
	
	/*
	if(!ring_size){
		indexed_nodes->size = aproximate_ring(info->value);
	}*/

	if(!ordered_nodes)
		ordered_nodes = pkg_malloc(ring_size * sizeof(clusterer_node_t *));
	memset(ordered_nodes, 0, ring_size * sizeof(clusterer_node_t *));

	value = cluster_list;
	while (value != NULL) {
		if (value->node_id >= ring_size) {
			LM_ERR("node_id: %d from culster %d, cannot be part of a ring of size %d\n",
					value->node_id,cluster_id , ring_size);
			return -1;
		}
		if (ordered_nodes[value->node_id]) {
			LM_ERR("node_id: %d from culster %d, is a dublicate \n", value->node_id,cluster_id);
			return -1;
		}
		ordered_nodes[value->node_id] = value;
		value = value->next;
	}

	/* get_nodes does not include me, add me separately */
	if(my_id < 0)
		my_id = clusterer_api.get_my_id();

	my_node = pkg_malloc(sizeof(clusterer_node_t));
	my_node->node_id = my_id;
	ordered_nodes[my_id] = my_node;
	/* after all nodes from the ring that have coresponding machines ids from the clusterer where set
	   set the empty vector positions with the correspondig nodes
	*/
	for (i = 0; i < ring_size; i++)
		if(ordered_nodes[i])
			break;

	value = ordered_nodes[i];
	for(i = ring_size - 1; i  >= 0; i--)
		if(ordered_nodes[i])
			value = ordered_nodes[i];
		else
			ordered_nodes[i] = value;

	last_updated = time(0);
	LM_DBG("XXX:remade the table of destinations\n");
	print_destination_vector();
	return 0;
}

static int send_to_ring(unsigned int hash)
{
	str send_buffer;
	int messages,first, idx, ok;


	bin_get_buffer(&send_buffer);


	if (!ordered_nodes) {
		create_table();
	} else if (refresh_interval == 0 || time(0) - last_updated > refresh_interval) {
		clusterer_api.free_nodes(cluster_list);
		pkg_free(ordered_nodes[my_id]);
		create_table();
	}

	idx = hash % ring_size;
	first = ordered_nodes[idx]->node_id;
	idx = first;
	messages = 0;
	ok = 0;

	*(int *)(send_buffer.s + KEY_OFFSET) = hash % ring_size;
	while (messages < replication_factor) {
		if(idx == my_id) {
			LM_DBG("XXX: im rensposible for this packet :P\n");
			idx = (idx + 1) % ring_size;
			idx = ordered_nodes[idx]->node_id;
			messages++;
			continue;
		}

		if (messages >= replication_factor)
			break;

		/* set the replication factor field */
		*(int *)(send_buffer.s + REPLICATION_OFFSET) = messages;
		if (clusterer_api.send_to(cluster_id, ordered_nodes[idx]->node_id)) {
			LM_ERR("XXX:message could not be send to machine id:%d\n", ordered_nodes[idx]->node_id);
			/* TODO!! retake the list from clusterer, maybe tell him somehow that the send failed
			*/
			last_updated = time(0) - refresh_interval - 1;
		} else {
			LM_DBG("XXX:send content of hash %d, and replication_factor %d, to node with id %d\n",hash % ring_size, messages , idx);
			messages++;
			ok = 1;
		}
		/* go to the index of the node we are pointing at and look at nexts*/
		idx = (idx + 1) % ring_size;
		idx = ordered_nodes[idx]->node_id;
		if (idx == first) {
			LM_DBG("XXX:got trough all the tabel of nodes");
			break;
		}
	}

	if(!ok)
		LM_WARN("XXX: messages could not be forwarded anywhere");
	else if (messages < replication_factor) {
		LM_WARN("XXX:replication factor not fulfilled, send %d / %d messages", messages, replication_factor);
	}

	return ok;
}

static int is_between(int dest, int start, int end)
{
    if (start < end)
		return start <= dest && dest < end;
    else
		return start <= dest || dest < end;
}

void receive_binary_packet(enum clusterer_event ev, int packet_type,
				struct receive_info *ri, int cluster_id, int src_id, int dest_id)
{
	int rc, i;
	int key, repl_factor, my_dest;
	str send_buffer;

	if (ev == CLUSTER_NODE_DOWN) {
		//down_event_node(dest_id);
		return;
	}
	else if (ev == CLUSTER_NODE_UP) {
		//up_event_node(dest_id);
		return;
	}
	else if (ev == CLUSTER_ROUTE_FAILED) {
		LM_INFO("Failed to route replication packet of type %d from node id: %d "
			"to node id: %d in cluster: %d\n", cluster_id, packet_type, src_id, dest_id);
		return;
	}

	LM_DBG("received a binary packet [%d]!\n", packet_type);

	bin_pop_int(&key);
	bin_pop_int(&repl_factor);

	LM_DBG("XXX:packet key %d, repl_factor %d!\n", key, repl_factor);

	if(my_id < 0){
		my_id =clusterer_api.get_my_id();
	}

	if (!ordered_nodes) {
		create_table();
	} else if (refresh_interval == 0 || time(0) - last_updated > refresh_interval) {
		clusterer_api.free_nodes(cluster_list);
		pkg_free(ordered_nodes[my_id]);
		create_table();
	}

	my_dest = ordered_nodes[key]->node_id;
	for (i = 0; i < repl_factor; i++) {
		my_dest = (my_dest + 1)% ring_size;
		my_dest = ordered_nodes[my_dest]->node_id;
		if(!is_between(my_dest, key, my_id)){
			my_dest= my_id;
			break;
		}
	}

	LM_DBG("XXX: our buffer len %d\n", send_buffer.len);

	while (my_id != my_dest){
		if (is_between(my_dest, key, my_id)) {
			LM_DBG("XXX trying to send the thing???\n");
			bin_get_recv_buffer(&send_buffer);
			bin_set_send_buffer(send_buffer);
			if (clusterer_api.send_to(cluster_id, ordered_nodes[my_dest]->node_id)) {
				LM_ERR("XXX: message could not be send to machine id:%d\n", ordered_nodes[my_dest]->node_id);
				last_updated = time(0) - refresh_interval - 1;
			} else {
				LM_DBG("XXX: redirected packet with hash %d and replication factor %d, to %d\n", key, repl_factor, my_dest);
				return;
			}
		} else {
			break;
		}
		my_dest = (my_dest + 1)% ring_size;
		my_dest = ordered_nodes[my_dest]->node_id;
	}	

	LM_DBG("XXX: keeping packet with hash %d and replication factor %d", key, repl_factor);
	
	switch (packet_type) {
	case INSERT_RING:
		rc = receive_partial_ucontact_insert();
		break;
	default:
		rc = -1;
		LM_ERR("invalid usrloc binary packet type: %d\n", packet_type);
	}

	if (rc != 0)
		LM_ERR("failed to process a binary packet!\n");
}

int receive_partial_ucontact_insert(void) {
	str attr, value;
	int expires;
	bin_pop_str(&attr);
	bin_pop_str(&value);
	bin_pop_int(&expires);

	if (insert_local_ring(&attr, expires, &value) < 0)
		return -1;

	return 0;

}

int dht_insert(cachedb_con *con,str* attr, str* value, int expires) {
	if (bin_init(&dht_mod_name, INSERT_RING, BIN_VERSION) != 0) {
		LM_ERR("failed to replicate this event\n");
		return -1;
	}

	//space for replication factor and key
	bin_push_int(200);
	bin_push_int(200);

	bin_push_str(attr);
	bin_push_str(value);
	bin_push_int(expires);

	send_to_ring(core_hash(attr, 0, 0));

	if (is_local_record(attr))
		if (insert_local_ring(attr, expires, value) < 0)
			return -1;

	return 0;
}

int dht_single_fetch(cachedb_con *con,str* attr, str* res) {
	if(is_local_record(attr)) {
		return find_local_ring(attr, res);
	}

	LM_WARN("we are not responsible for the record\n");
	return -2;
}



