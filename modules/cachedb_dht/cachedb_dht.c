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
void cache_clean(unsigned int ticks,void *param);
static int create_table(void);
void bulk_send(int start, int end, int dest);
void down_event_node(int node);
void up_event_node(int dest_id);
int receive_bulk_insert(void);
int init_new_nodes(void);
int received_delete_key();
int dht_remove(cachedb_con *con,str* attr);
int remove_local_ring(str* attr);

str dht_mod_name = str_init("dht_cluster");
int ring_size = 5;
int replication_factor = 2;
int my_id = -1;
uint64_t last_updated;
int refresh_interval = 0;
int cluster_id = 0;
int repl_auth_check = 0;
int clean_period = 10;
struct registers_table *ring_table;
struct clusterer_binds clusterer_api;
clusterer_node_t **ordered_nodes;
clusterer_node_t *cluster_list;
int *new_nodes;
unsigned int local_table_size = RING_TABLE_SIZE;
static gen_lock_t *global_lock;

#define NR_RECORDS_OFFSET HEADER_SIZE + 3 * LEN_FIELD_SIZE + dht_mod_name.len + CMD_FIELD_SIZE 
#define REPLICATION_OFFSET HEADER_SIZE + 2 * LEN_FIELD_SIZE + dht_mod_name.len + CMD_FIELD_SIZE 
#define KEY_OFFSET HEADER_SIZE +  LEN_FIELD_SIZE + dht_mod_name.len + CMD_FIELD_SIZE

static param_export_t params[]={
	{ "ring_size",   INT_PARAM, &ring_size },
	{ "replication_factor", INT_PARAM, &replication_factor },
	{ "refresh_interval",     INT_PARAM, &refresh_interval },
	{ "cluster_id",     INT_PARAM, &cluster_id },
	{ "dht_repl_auth_check",     INT_PARAM, &repl_auth_check },
	{ "clean_period",     INT_PARAM, &clean_period },
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
	cde.name = name;

	cde.cdb_func.init = dht_cache_init;
	cde.cdb_func.destroy = dht_cache_destroy;
	cde.cdb_func.get = dht_single_fetch;
	cde.cdb_func.set = dht_insert;
	cde.cdb_func.remove = dht_remove;

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
	register_timer("dhtcache-expire",cache_clean, 0,
		clean_period, TIMER_FLAG_DELAY_ON_DELAY);

	if(load_clusterer_api(&clusterer_api)!=0){
		LM_DBG("failed to find clusterer API - is clusterer module loaded?\n");
		return -1;
	}

	if ( clusterer_api.register_module(dht_mod_name.s,
		receive_binary_packet, repl_auth_check, &cluster_id, 1) < 0) {
		LM_ERR("cannot register binary packet callback to clusterer module!\n");
		return -1;
	}

	if(init_new_nodes() != 0){
		LM_ERR("Failed to init new nodes list\n");
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
	//TODO needs deperate rework
	char buff[1000000], aux[10000];
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
	int n;

	for( n=0 ; n < 8 * sizeof(unsigned int) ; n++) {
		if (local_table_size==(1<<n))
			break;
		if (local_table_size<(1<<n)) {
			LM_WARN("hash_size is not a power "
					"of 2 as it should be -> rounding from %d to %d\n",
					local_table_size, 1<<(n-1));
			local_table_size = 1<<(n-1);
		}
	}

	ring_table = shm_malloc(sizeof(struct registers_table));

	if(!ring_table){
		LM_ERR("cannot allocate shared memory\n");
		return -1;
	}

	ring_table->size = local_table_size;
	ring_table->buckets = shm_malloc(local_table_size * sizeof(struct register_cell *));
	if (!ring_table->buckets) {
		LM_ERR("cannot allocate shared memory\n");
		shm_free(ring_table);
		return -1;
	}

	memset(ring_table->buckets, 0 , local_table_size * sizeof(struct register_cell *));

	ring_table->locks = shm_malloc(local_table_size * sizeof(rw_lock_t *));
	if (!ring_table->locks) {
		LM_ERR("cannot allocate shared memory\n");
		shm_free(ring_table->buckets);
		shm_free(ring_table);
		return -1;
	}

	for (n = 0; n < local_table_size; n++) {
		ring_table->locks[n] = lock_init_rw();

		if(!ring_table->locks[n]){
			LM_ERR("cannot allocate rw_lock\n");

			for(n = n - 1; n >= 0; n--)
				lock_destroy_rw(ring_table->locks[n]);

			shm_free(ring_table->locks);
			shm_free(ring_table->buckets);
			shm_free(ring_table);
			return -1;
		}
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

	lock_start_read(ring_table->locks[hash]);
	for (cell = ring_table->buckets[hash]; cell; cell = cell->next) {
		if (str_cmp(cell->attr, *attr) == 0) {
			found = 1;
			for (nod = cell->nodes; nod; nod=nod->next)
				if(nod->expires > time(0) || nod->expires == 0){
					pkg_str_dup(val,&nod->val);
					break;
				}
			break;
		}
	}

	lock_stop_read(ring_table->locks[hash]);

	if (found) {
		return 0;
	}
	else {
		return -1;
	}
}

int insert_local_ring(str *attr, time_t expires, str *val) {
	unsigned int hash, found = 0;
	struct register_cell *cell;

	hash = core_hash(attr, 0, 0);
	hash = hash % ring_table->size;
	LM_DBG("HHH hash is: %u\n", hash);

	expires += time(0);

	lock_start_write(ring_table->locks[hash]);
	for (cell = ring_table->buckets[hash]; cell; cell = cell->next) {
		if (str_cmp(cell->attr, *attr) == 0){
			found = 1;
			break;
		}
	}

	if (found) {	
		//overwrite the last insertion
		LM_DBG("XXX found key\n");
		shm_free(cell->nodes->val.s);
		shm_free(cell->nodes);
		cell->nodes = NULL;

		if (insert_new_machine_register(expires, val, cell)) {
			goto err;
		}

	} else {
		LM_DBG("XXX NOt found key\n");
		cell = shm_malloc(sizeof(struct register_cell));
		if (!cell) {
			goto err;
		}
		cell->next = ring_table->buckets[hash];
		shm_str_dup(&cell->attr, attr);
		ring_table->buckets[hash] = cell;
		cell->nodes = NULL;
		if(insert_new_machine_register(expires, val, cell)){
			goto err;
		}
	}
	lock_stop_write(ring_table->locks[hash]);
	//print_register_table();

	return 0;

err:
	LM_ERR("no more shared memory");
	lock_stop_write(ring_table->locks[hash]);
	return -1;
}

int is_local_record(str *attr) {
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
	//TODO needs desperate rework
	char buf[100000];
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
	//print_destination_vector();
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
    else if (start > end)
		return start <= dest || dest < end;
	else
		return start == dest;
}

void receive_binary_packet(enum clusterer_event ev, int packet_type,
				struct receive_info *ri, int cluster_id, int src_id, int dest_id)
{
	int rc, i;
	int key, repl_factor, my_dest;
	str send_buffer;

	if (ev == CLUSTER_NODE_DOWN) {
		down_event_node(dest_id);
		return;
	}
	else if (ev == CLUSTER_NODE_UP) {
		up_event_node(dest_id);
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
	case BULK_NODES_INSERT:
		rc = receive_bulk_insert();
		break;
	case DELETE_RING:
		rc = received_delete_key();
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

int received_delete_key(void) {
	str attr;
	bin_pop_str(&attr);

	remove_local_ring(&attr);

	return 0;

}

int receive_bulk_insert(void) {
	str attr, value;
	int expires, nr_records, nr_vals, i, j;

	bin_pop_int(&nr_records);

	LM_DBG("YYY received %d bulk contacts\n", nr_records);
                           
	for (i = 0; i < nr_records; i++) {
		LM_DBG("YYY poped 1 i\n");
		bin_pop_str(&attr);
		bin_pop_int(&nr_vals);
		for (j = 0; j < nr_vals; j++) {
			LM_DBG("YYY poped 1 j\n");
			bin_pop_str(&value);
			bin_pop_int(&expires);
			if (insert_local_ring(&attr, expires, &value) < 0)
				return -1;
		}

	}

	return 0;

}

int dht_insert(cachedb_con *con,str* attr, str* value, int expires) {
	if (bin_init(&dht_mod_name, INSERT_RING, BIN_VERSION) != 0) {
		LM_ERR("failed to replicate this event\n");
		return -1;
	}

	//space for replication factor and key
	//if cu daca e factor de replicare 1
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
	//pus atribut si de ce nu e
	LM_WARN("we are not responsible for the record\n");
	return -2;
}

void cache_clean(unsigned int ticks,void *param) {
	unsigned int i;
	struct register_cell *cell, *prev_cell, *aux_cell;
	struct machines_list *nod, *prev_node, *aux;
	int writing, old;
	LM_DBG("XXX started cleaning dht local\n");

	for (i = 0; i < RING_TABLE_SIZE; i++) {
		writing = 0;
		lock_start_read(ring_table->locks[i]);
again:	prev_cell = NULL;
		for (cell = ring_table->buckets[i]; cell;){
			for (prev_node = NULL, nod = cell->nodes; nod;) {
				if(nod->expires < time(0) && nod->expires != 0){
					if(!writing){
						writing = 1;
						lock_switch_write(ring_table->locks[i], old);
						goto again;
					}
					shm_free(nod->val.s);
					if(prev_node)
						prev_node->next = nod->next;
					else
						cell->nodes = nod->next;
					aux = nod;
					nod = nod->next;
					shm_free(aux);
					LM_DBG("XXX cleaned a node\n");
				} else {
					prev_node = nod;
					nod = nod->next;
				}
			}

			if (!cell->nodes){
				shm_free(cell->attr.s);
				if(prev_cell)
						prev_cell->next = cell->next;
					else
						ring_table->buckets[i] = cell->next;
				aux_cell = cell;
				cell = cell->next;
				shm_free(aux_cell);
				LM_DBG("XXX cleaned a cell\n");
				} else {
					prev_cell = cell;
					cell = cell->next;
			}
		}
		if(!writing)
			lock_stop_read(ring_table->locks[i]);
		else {
			lock_switch_read(ring_table->locks[i], old);
			lock_stop_read(ring_table->locks[i]);
		}
	}

	LM_DBG("finished cleaning dht local\n");

	//print_register_table();
}

void decrease_ring(int *nr){
	if (*nr == 0)
		*nr = ring_size - 1;
	else
		*nr = *nr - 1;
}

int nsuccesor_ring(int id, int n){
	int i;
	for(i = 0; i < n; i++){
		id = (id + 1) % ring_size;
		id = ordered_nodes[id]->node_id;
	}
	return id;
}

int calculate_range(int id){
	int limit = id;
	decrease_ring(&limit);
	while (limit != id && ordered_nodes[limit]->node_id == id) {
		decrease_ring(&limit);
	}

	if (limit == id) {
		LM_DBG("YYY the only node left in the ring\n");
		decrease_ring(&id);
		return id;
	}

	return (limit + 1)%ring_size;
}

int nodes_left(void){
	int nr = 1; //the list doesnt contain me
	clusterer_node_t *value;
	value = cluster_list;

	for(value = cluster_list; value; value=value->next)
		nr++;

	LM_DBG("YYY nodes left %d\n", nr);

	return nr;
}

static int create_table_aux(int node_aux) {
	//TODO simplify the function actually only need to add node_aux artificially to the vector
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

	my_node = pkg_malloc(sizeof(clusterer_node_t));
	my_node->node_id = node_aux;
	ordered_nodes[node_aux] = my_node;
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
	//print_destination_vector();
	return 0;
}

void down_event_node(int dest_id) {
	int i, id, to_replicate = 0;

	LM_DBG("YYY node with id %d is now down\n",dest_id);

	if (my_id < 0){
		my_id =clusterer_api.get_my_id();
	}

	create_table_aux(dest_id);

	if(nodes_left() < replication_factor){
		LM_WARN("YYY not enough nodes to ensure the replication factor anymore\n");
		return;
	}

	id = my_id;
	for(i = 1; i < replication_factor; i++) {
		id = (id + 1) % ring_size;
		id = ordered_nodes[id]->node_id;
		if (id == dest_id){
			LM_DBG("YYY key we are directly responsible key that were copied at the fallen node"
					"we need to resend them elsewere\n");
			to_replicate = 1;
			break;
		}
	}

	if(!to_replicate){
		id = (dest_id + 1) % ring_size;
		id = ordered_nodes[id]->node_id;
		if(id == my_id){
			LM_DBG("YYY our predecessor is down, we need to make copy of his data\n");
			to_replicate = 2;
		}
	}



	if (to_replicate == 1) {
		bulk_send(calculate_range(my_id), my_id, nsuccesor_ring(my_id, replication_factor));
	} else if (to_replicate == 2) {
		bulk_send(calculate_range(dest_id), dest_id, nsuccesor_ring(my_id, replication_factor - 1));
	}

	create_table();
}

int check_aux_space(void) {
	static char junk[300];
	str s;
	s.len = 300;
	s.s = junk;
	if (bin_push_str(&s) < 0) {
		return -1;
	}
	bin_remove_bytes_send_buffer(s.len + LEN_FIELD_SIZE);

	return 0;
}


void partial_send(str *send_buffer, int *nr_records, int dest, int last_record_len) {
	LM_DBG("ZZZ: the bin_buffer is full send everything and continuing\n");
	if (check_aux_space() < 0) {
		LM_DBG("ZZZ: removed a record for clusterer: \n");
		bin_remove_bytes_send_buffer(last_record_len);
		*nr_records = *nr_records - 1;
	}
	*(int *)(send_buffer->s + NR_RECORDS_OFFSET) = *nr_records;
	LM_DBG("YYY: records number %d\n",*nr_records);

	if (clusterer_api.send_to(cluster_id, dest)) {
		LM_ERR("YYY: bulk could not be send to machine id:%d\n", dest);
		return;
	}
	*nr_records = 0;
	if (bin_init(&dht_mod_name, BULK_NODES_INSERT, BIN_VERSION) != 0) {
		LM_ERR("failed to reinit bin buffer\n");
		return;
	}
	bin_get_buffer(send_buffer);

	//space for replication factor and key
	bin_push_int(dest);
	bin_push_int(0);

	//space for the number of records send
	bin_push_int(200);
}


void bulk_send(int start, int end, int dest) {
	//TODO find a way to remove the send from inside the lock
	int i, nr_vals, hash, nr_records = 0;
	int reset_len, last_record_len;
	struct register_cell *cell;
	struct machines_list *nod;
	str send_buffer;

	LM_DBG("YYY: sending hashes from %d to %d to node %d\n",start, end, dest);

	if (bin_init(&dht_mod_name, BULK_NODES_INSERT, BIN_VERSION) != 0) {
		LM_ERR("failed to init bin buffer\n");
		return;
	}

	bin_get_buffer(&send_buffer);

	//space for replication factor and key
	bin_push_int(dest);
	bin_push_int(0);

	//space for the number of records send
	bin_push_int(200);

	for (i = 0; i < RING_TABLE_SIZE;) {
		lock_start_read(ring_table->locks[i]);
		for (cell = ring_table->buckets[i]; cell;cell=cell->next) {
			hash = core_hash(&cell->attr, 0, 0) % ring_size;
			if (is_between(hash, start, end)  || hash == end) {
				LM_DBG("YYY its acceped hash %d\n", hash);
last_send:		nr_vals = 0;
				reset_len = 0;

				if (bin_push_str(&cell->attr) < 0) {
					partial_send(&send_buffer, &nr_records, dest, last_record_len);
					goto last_send;
				}
				reset_len += cell->attr.len + LEN_FIELD_SIZE;

				if (bin_push_int(nr_vals) < 0) {
					bin_remove_bytes_send_buffer(reset_len);
					partial_send(&send_buffer, &nr_records, dest, last_record_len);
					goto last_send;
				}
				reset_len += LEN_FIELD_SIZE;
				for (nod = cell->nodes; nod;nod=nod->next) 
					if (nod->expires > time(0) || nod->expires == 0) {
						if(bin_push_str(&nod->val) < 0){
							bin_remove_bytes_send_buffer(reset_len);
							partial_send(&send_buffer, &nr_records, dest, last_record_len);
							goto last_send;
						}
						reset_len += nod->val.len + LEN_FIELD_SIZE;
						if(bin_push_int(nod->expires) < 0){
							bin_remove_bytes_send_buffer(reset_len);
							partial_send(&send_buffer, &nr_records, dest, last_record_len);
							goto last_send;
						}
						reset_len += LEN_FIELD_SIZE;
						nr_vals++;
					}

				if (nr_vals) {
					bin_remove_bytes_send_buffer(reset_len - cell->attr.len - LEN_FIELD_SIZE);
					bin_push_int(nr_vals);
					bin_skip_bytes_send_buffer(reset_len - LEN_FIELD_SIZE - cell->attr.len - LEN_FIELD_SIZE);
					nr_records++;
					last_record_len = reset_len;
				} else {	
					bin_remove_bytes_send_buffer(reset_len);
				}
			}
		}
		lock_stop_read(ring_table->locks[i]);
		i++;
	}

	if (nr_records) {
		if (check_aux_space() < 0) {
			LM_DBG("ZZZ removed last record\n");
			bin_remove_bytes_send_buffer(last_record_len);
			nr_records--;
		}
		*(int *)(send_buffer.s + NR_RECORDS_OFFSET) = nr_records;
		LM_DBG("YYY: records number %d\n",nr_records);
		if (clusterer_api.send_to(cluster_id, dest)) {
			LM_ERR("YYY: bulk could not be send to machine id:%d\n", dest);
		}
	}

}

int init_new_nodes(void){
	new_nodes = shm_malloc(ring_size * sizeof(int));
	if(!new_nodes){
		LM_ERR("no more shared memory\n");
		return -1;
	}

	memset(new_nodes, 0 , sizeof(int));

	global_lock = lock_alloc();

	if (global_lock == NULL) {
		LM_ERR("Failed to allocate lock \n");
		return -1;
	}

	if (lock_init(global_lock) == NULL) {
		LM_ERR("Failed to init lock \n");
		return -1;
	}

	return 0;
}

int npredeccesor_ring (int id, int n){
	int i, val, start = id;
	val = ordered_nodes[id]->node_id;
	decrease_ring(&id);
	for(i = 0; i < n; i++){
		while(id != start && val == ordered_nodes[id]->node_id)
			decrease_ring(&id);
		val = ordered_nodes[id]->node_id;
		if(id == start){
			LM_DBG("exit 1 pred of id %d(%d) = %d\n", start, n, ordered_nodes[(start + 1) % ring_size]->node_id);
			return ordered_nodes[(start + 1) % ring_size]->node_id;
		}
	}
	LM_DBG("exit 2 pred of id %d(%d) = %d\n", start, n, val);
	return val;
}

void delete_hashes(int start, int end) {
	unsigned int i, hash;
	struct register_cell *cell, *prev_cell, *aux_cell;
	struct machines_list *nod, *aux;
	int writing, old;

	LM_DBG("XXX clearing hashes from %d to %d\n", start, end);

	for (i = 0; i < RING_TABLE_SIZE; i++) {
		writing = 0;
		lock_start_read(ring_table->locks[i]);
restart:
		prev_cell = NULL;
		for (cell = ring_table->buckets[i]; cell;){
			hash = core_hash(&cell->attr, 0, 0) % ring_size;
			if (is_between(hash, start, end)  || hash == end) {
				if (!writing) {
					writing = 1;
					lock_switch_write(ring_table->locks[i], old);
					goto restart;
				}
				for (nod = cell->nodes; nod;) {
						shm_free(nod->val.s);
						cell->nodes = nod->next;
						aux = nod;
						nod = nod->next;
						shm_free(aux);
						LM_DBG("XXX cleaned a node\n");
				}

				shm_free(cell->attr.s);
				if (prev_cell)
					prev_cell->next = cell->next;
				else
					ring_table->buckets[i] = cell->next;
				aux_cell = cell;
				cell = cell->next;
				shm_free(aux_cell);
				LM_DBG("XXX cleaned a cell\n");
			} else {
				prev_cell = cell;
				cell = cell->next;
			}
		}
		if(!writing)
			lock_stop_read(ring_table->locks[i]);
		else {
			lock_switch_read(ring_table->locks[i], old);
			lock_stop_read(ring_table->locks[i]);
		}
	}

	LM_DBG("finished cleaning dht local\n");

	print_register_table();
}


void up_event_node(int dest_id) {
	int i, id, total_nodes;

	LM_DBG("YYY node with id %d is now up\n",dest_id);

	if (my_id < 0){
		my_id =clusterer_api.get_my_id();
	}

	create_table();

	total_nodes = nodes_left();

	id = nsuccesor_ring(dest_id, 1);

	//TODO change the lock to atomic operations
	lock_get(global_lock);
	new_nodes[dest_id] = 1;
	lock_release(global_lock);

	if(id == my_id){
		LM_DBG("YYY the new node is our predecessor, we need to populate his local table\n");
		bulk_send(calculate_range(npredeccesor_ring(dest_id, replication_factor - 1)), dest_id, dest_id);
	}

	if(total_nodes > replication_factor){
		for(i = 0; i < replication_factor && id != dest_id; i++) {
			if (id == my_id){
				id = npredeccesor_ring(my_id, replication_factor);
				delete_hashes(calculate_range(id), id);
				return;
			}
			id = nsuccesor_ring(id, 1);
		}
	}

}

int remove_local_ring(str* attr) {
	unsigned int hash;
	struct register_cell *cell, *prev_cell;

	hash = core_hash(attr, 0, 0);
	hash = hash % ring_table->size;
	LM_DBG("HHH hash is: %u\n", hash);

	lock_start_write(ring_table->locks[hash]);
	for (prev_cell = NULL, cell = ring_table->buckets[hash]; cell; prev_cell = cell, cell = cell->next) {
		if (str_cmp(cell->attr, *attr) == 0){
			shm_free(cell->nodes->val.s);

			if(prev_cell)
				prev_cell->next = cell->next;
			else
				ring_table->buckets[hash] = cell->next;

			LM_DBG("YYY removed key %.*s\n", attr->len, attr->s);
			shm_free(cell->attr.s);
			shm_free(cell);
			break;
		}
	}
	lock_stop_write(ring_table->locks[hash]);

	//print_register_table();

	return 0;
}

int dht_remove(cachedb_con *con,str* attr) {
	if(is_local_record(attr)) {
		remove_local_ring(attr);
	}
	if (bin_init(&dht_mod_name, DELETE_RING, BIN_VERSION) != 0) {
		LM_ERR("failed to replicate this event\n");
		return -1;
	}

	//space for replication factor and key
	bin_push_int(200);
	bin_push_int(200);

	bin_push_str(attr);

	send_to_ring(core_hash(attr, 0, 0));

	return 0;
}
