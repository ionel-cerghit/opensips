/*
 * Copyright (C) 2007-2008 1&1 Internet AG
 *
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
 */

/**
 * @file route_config.h
 * @brief Functions for load and save routing data from a config file.
 */

#ifndef SP_ROUTE_ROUTE_CONFIG_H
#define SP_ROUTE_ROUTE_CONFIG_H

#include "route_tree.h"

/**
 * Loads the routing data from the config file given in global
 * variable config_data and stores it in routing tree rd.
 *
 * @param rd Pointer to the route data tree where the routing data
 * shall be loaded into
 *
 * @return 0 means ok, -1 means an error occured
 *
 */
int load_config(struct rewrite_data * rd);

/**
 * Stores the routing data rd in config_file
 *
 * @param rd Pointer to the routing tree which shall be saved to file
 *
 * @return 0 means ok, -1 means an error occured
 *
 */
int save_config(struct rewrite_data * rd);

#endif
