#ifndef _MODULE_INFO__
#define _MODULE_INFO__

struct module_info_{
	char *mod_name;
	int fragments;
	int memory_used;
	int real_used;
	struct module_info_ *next;
} module_info;

#endif