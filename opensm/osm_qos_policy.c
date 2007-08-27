/*
 * Copyright (c) 2004-2006 Voltaire, Inc. All rights reserved.
 * Copyright (c) 2002-2005 Mellanox Technologies LTD. All rights reserved.
 * Copyright (c) 1996-2003 Intel Corporation. All rights reserved.
 *
 * This software is available to you under a choice of one of two
 * licenses.  You may choose to be licensed under the terms of the GNU
 * General Public License (GPL) Version 2, available from the file
 * COPYING in the main directory of this source tree, or the
 * OpenIB.org BSD license below:
 *
 *     Redistribution and use in source and binary forms, with or
 *     without modification, are permitted provided that the following
 *     conditions are met:
 *
 *      - Redistributions of source code must retain the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer.
 *
 *      - Redistributions in binary form must reproduce the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer in the documentation and/or other materials
 *        provided with the distribution.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 *
 */

/*
 * Abstract:
 *    OSM QoS Policy functions.
 *
 * Environment:
 *    Linux User Mode
 *
 * Author:
 *    Yevgeny Kliteynik, Mellanox
 */

#include <stdio.h>
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <opensm/osm_log.h>
#include <opensm/osm_node.h>
#include <opensm/osm_port.h>
#include <opensm/osm_partition.h>
#include <opensm/osm_qos_policy.h>

/***************************************************
 ***************************************************/

static boolean_t
__is_num_in_range_arr(uint64_t ** range_arr,
		  unsigned range_arr_len, uint64_t num)
{
	unsigned ind_1 = 0;
	unsigned ind_2 = range_arr_len - 1;
	unsigned ind_mid;

	if (!range_arr || !range_arr_len)
		return FALSE;

	while (ind_1 <= ind_2) {
	    if (num < range_arr[ind_1][0] || num > range_arr[ind_2][1])
		return FALSE;
	    else if (num <= range_arr[ind_1][1] || num >= range_arr[ind_2][0])
		return TRUE;

	    ind_mid = ind_1 + (ind_2 - ind_1 + 1)/2;

	    if (num < range_arr[ind_mid][0])
		ind_2 = ind_mid;
	    else if (num > range_arr[ind_mid][1])
		ind_1 = ind_mid;
	    else
		return TRUE;

	    ind_1++;
	    ind_2--;
	}

	return FALSE;
}

/***************************************************
 ***************************************************/

static void __free_single_element(void *p_element, void *context)
{
	if (p_element)
		free(p_element);
}

/***************************************************
 ***************************************************/

osm_qos_port_group_t *osm_qos_policy_port_group_create()
{
	osm_qos_port_group_t *p =
	    (osm_qos_port_group_t *) malloc(sizeof(osm_qos_port_group_t));
	if (!p)
		return NULL;

	memset(p, 0, sizeof(osm_qos_port_group_t));

	cl_list_init(&p->port_name_list, 10);
	cl_list_init(&p->partition_list, 10);

	return p;
}

/***************************************************
 ***************************************************/

void osm_qos_policy_port_group_destroy(osm_qos_port_group_t * p)
{
	unsigned i;

	if (!p)
		return;

	if (p->name)
		free(p->name);
	if (p->use)
		free(p->use);

	for (i = 0; i < p->guid_range_len; i++)
		free(p->guid_range_arr[i]);
	if (p->guid_range_arr)
		free(p->guid_range_arr);

	cl_list_apply_func(&p->port_name_list, __free_single_element, NULL);
	cl_list_remove_all(&p->port_name_list);
	cl_list_destroy(&p->port_name_list);

	cl_list_apply_func(&p->partition_list, __free_single_element, NULL);
	cl_list_remove_all(&p->partition_list);
	cl_list_destroy(&p->partition_list);

	free(p);
}

/***************************************************
 ***************************************************/

osm_qos_vlarb_scope_t *osm_qos_policy_vlarb_scope_create()
{
	osm_qos_vlarb_scope_t *p =
	    (osm_qos_vlarb_scope_t *) malloc(sizeof(osm_qos_sl2vl_scope_t));
	if (!p)
		return NULL;

	memset(p, 0, sizeof(osm_qos_vlarb_scope_t));

	cl_list_init(&p->group_list, 10);
	cl_list_init(&p->across_list, 10);
	cl_list_init(&p->vlarb_high_list, 10);
	cl_list_init(&p->vlarb_low_list, 10);

	return p;
}

/***************************************************
 ***************************************************/

void osm_qos_policy_vlarb_scope_destroy(osm_qos_vlarb_scope_t * p)
{
	if (!p)
		return;

	cl_list_apply_func(&p->group_list, __free_single_element, NULL);
	cl_list_apply_func(&p->across_list, __free_single_element, NULL);
	cl_list_apply_func(&p->vlarb_high_list, __free_single_element, NULL);
	cl_list_apply_func(&p->vlarb_low_list, __free_single_element, NULL);

	cl_list_remove_all(&p->group_list);
	cl_list_remove_all(&p->across_list);
	cl_list_remove_all(&p->vlarb_high_list);
	cl_list_remove_all(&p->vlarb_low_list);

	cl_list_destroy(&p->group_list);
	cl_list_destroy(&p->across_list);
	cl_list_destroy(&p->vlarb_high_list);
	cl_list_destroy(&p->vlarb_low_list);

	free(p);
}

/***************************************************
 ***************************************************/

osm_qos_sl2vl_scope_t *osm_qos_policy_sl2vl_scope_create()
{
	osm_qos_sl2vl_scope_t *p =
	    (osm_qos_sl2vl_scope_t *) malloc(sizeof(osm_qos_sl2vl_scope_t));
	if (!p)
		return NULL;

	memset(p, 0, sizeof(osm_qos_vlarb_scope_t));

	cl_list_init(&p->group_list, 10);
	cl_list_init(&p->across_from_list, 10);
	cl_list_init(&p->across_to_list, 10);

	return p;
}

/***************************************************
 ***************************************************/

void osm_qos_policy_sl2vl_scope_destroy(osm_qos_sl2vl_scope_t * p)
{
	if (!p)
		return;

	cl_list_apply_func(&p->group_list, __free_single_element, NULL);
	cl_list_apply_func(&p->across_from_list, __free_single_element, NULL);
	cl_list_apply_func(&p->across_to_list, __free_single_element, NULL);

	cl_list_remove_all(&p->group_list);
	cl_list_remove_all(&p->across_from_list);
	cl_list_remove_all(&p->across_to_list);

	cl_list_destroy(&p->group_list);
	cl_list_destroy(&p->across_from_list);
	cl_list_destroy(&p->across_to_list);

	free(p);
}

/***************************************************
 ***************************************************/

osm_qos_level_t *osm_qos_policy_qos_level_create()
{
	osm_qos_level_t *p =
	    (osm_qos_level_t *) malloc(sizeof(osm_qos_level_t));
	if (!p)
		return NULL;
	memset(p, 0, sizeof(osm_qos_level_t));
	return p;
}

/***************************************************
 ***************************************************/

void osm_qos_policy_qos_level_destroy(osm_qos_level_t * p)
{
	unsigned i;

	if (!p)
		return;

	if (p->use)
		free(p->use);

	for (i = 0; i < p->path_bits_range_len; i++)
		free(p->path_bits_range_arr[i]);
	if (p->path_bits_range_arr)
		free(p->path_bits_range_arr);

	free(p);
}

/***************************************************
 ***************************************************/

boolean_t osm_qos_level_has_pkey(IN const osm_qos_level_t * p_qos_level,
				 IN ib_net16_t pkey)
{
	if (!p_qos_level || !p_qos_level->pkey_range_len)
		return FALSE;
	return __is_num_in_range_arr(p_qos_level->pkey_range_arr,
				     p_qos_level->pkey_range_len,
				     cl_ntoh16(pkey));
}

/***************************************************
 ***************************************************/

ib_net16_t osm_qos_level_get_shared_pkey(IN const osm_qos_level_t * p_qos_level,
					 IN const osm_physp_t * p_src_physp,
					 IN const osm_physp_t * p_dest_physp)
{
	unsigned i;
	uint16_t pkey_ho = 0;

	if (!p_qos_level || !p_qos_level->pkey_range_len)
		return 0;

	/*
	 * ToDo: This approach is not optimal.
	 *       Think how to find shared pkey that also exists
	 *       in QoS level in less runtime.
	 */

	for (i = 0; i < p_qos_level->pkey_range_len; i++) {
		for (pkey_ho = p_qos_level->pkey_range_arr[i][0];
		     pkey_ho <= p_qos_level->pkey_range_arr[i][1]; pkey_ho++) {
			if (osm_physp_share_this_pkey
			    (p_src_physp, p_dest_physp, cl_hton16(pkey_ho)))
				return cl_hton16(pkey_ho);
		}
	}

	return 0;
}

/***************************************************
 ***************************************************/

osm_qos_match_rule_t *osm_qos_policy_match_rule_create()
{
	osm_qos_match_rule_t *p =
	    (osm_qos_match_rule_t *) malloc(sizeof(osm_qos_match_rule_t));
	if (!p)
		return NULL;

	memset(p, 0, sizeof(osm_qos_match_rule_t));

	cl_list_init(&p->source_list, 10);
	cl_list_init(&p->source_group_list, 10);
	cl_list_init(&p->destination_list, 10);
	cl_list_init(&p->destination_group_list, 10);

	return p;
}

/***************************************************
 ***************************************************/

void osm_qos_policy_match_rule_destroy(osm_qos_match_rule_t * p)
{
	unsigned i;

	if (!p)
		return;

	if (p->qos_level_name)
		free(p->qos_level_name);
	if (p->use)
		free(p->use);

	for (i = 0; i < p->service_id_range_len; i++)
		free(p->service_id_range_arr[i]);
	if (p->service_id_range_arr)
		free(p->service_id_range_arr);

	for (i = 0; i < p->qos_class_range_len; i++)
		free(p->qos_class_range_arr[i]);
	if (p->qos_class_range_arr)
		free(p->qos_class_range_arr);

	cl_list_apply_func(&p->source_list, __free_single_element, NULL);
	cl_list_remove_all(&p->source_list);
	cl_list_destroy(&p->source_list);

	cl_list_remove_all(&p->source_group_list);
	cl_list_destroy(&p->source_group_list);

	cl_list_apply_func(&p->destination_list, __free_single_element, NULL);
	cl_list_remove_all(&p->destination_list);
	cl_list_destroy(&p->destination_list);

	cl_list_remove_all(&p->destination_group_list);
	cl_list_destroy(&p->destination_group_list);

	free(p);
}

/***************************************************
 ***************************************************/

osm_qos_policy_t * osm_qos_policy_create()
{
	osm_qos_policy_t * p_qos_policy = (osm_qos_policy_t *)malloc(sizeof(osm_qos_policy_t));
	if (!p_qos_policy)
		return NULL;

	memset(p_qos_policy, 0, sizeof(osm_qos_policy_t));

	cl_list_construct(&p_qos_policy->port_groups);
	cl_list_init(&p_qos_policy->port_groups, 10);

	cl_list_construct(&p_qos_policy->vlarb_tables);
	cl_list_init(&p_qos_policy->vlarb_tables, 10);

	cl_list_construct(&p_qos_policy->sl2vl_tables);
	cl_list_init(&p_qos_policy->sl2vl_tables, 10);

	cl_list_construct(&p_qos_policy->qos_levels);
	cl_list_init(&p_qos_policy->qos_levels, 10);

	cl_list_construct(&p_qos_policy->qos_match_rules);
	cl_list_init(&p_qos_policy->qos_match_rules, 10);

	return p_qos_policy;
}

/***************************************************
 ***************************************************/

void osm_qos_policy_destroy(osm_qos_policy_t * p_qos_policy)
{
	cl_list_iterator_t list_iterator;
	osm_qos_port_group_t *p_port_group = NULL;
	osm_qos_vlarb_scope_t *p_vlarb_scope = NULL;
	osm_qos_sl2vl_scope_t *p_sl2vl_scope = NULL;
	osm_qos_level_t *p_qos_level = NULL;
	osm_qos_match_rule_t *p_qos_match_rule = NULL;

	if (!p_qos_policy)
		return;

	list_iterator = cl_list_head(&p_qos_policy->port_groups);
	while (list_iterator != cl_list_end(&p_qos_policy->port_groups)) {
		p_port_group =
		    (osm_qos_port_group_t *) cl_list_obj(list_iterator);
		if (p_port_group)
			osm_qos_policy_port_group_destroy(p_port_group);
		list_iterator = cl_list_next(list_iterator);
	}
	cl_list_remove_all(&p_qos_policy->port_groups);
	cl_list_destroy(&p_qos_policy->port_groups);

	list_iterator = cl_list_head(&p_qos_policy->vlarb_tables);
	while (list_iterator != cl_list_end(&p_qos_policy->vlarb_tables)) {
		p_vlarb_scope =
		    (osm_qos_vlarb_scope_t *) cl_list_obj(list_iterator);
		if (p_vlarb_scope)
			osm_qos_policy_vlarb_scope_destroy(p_vlarb_scope);
		list_iterator = cl_list_next(list_iterator);
	}
	cl_list_remove_all(&p_qos_policy->vlarb_tables);
	cl_list_destroy(&p_qos_policy->vlarb_tables);

	list_iterator = cl_list_head(&p_qos_policy->sl2vl_tables);
	while (list_iterator != cl_list_end(&p_qos_policy->sl2vl_tables)) {
		p_sl2vl_scope =
		    (osm_qos_sl2vl_scope_t *) cl_list_obj(list_iterator);
		if (p_sl2vl_scope)
			osm_qos_policy_sl2vl_scope_destroy(p_sl2vl_scope);
		list_iterator = cl_list_next(list_iterator);
	}
	cl_list_remove_all(&p_qos_policy->sl2vl_tables);
	cl_list_destroy(&p_qos_policy->sl2vl_tables);

	list_iterator = cl_list_head(&p_qos_policy->qos_levels);
	while (list_iterator != cl_list_end(&p_qos_policy->qos_levels)) {
		p_qos_level = (osm_qos_level_t *) cl_list_obj(list_iterator);
		if (p_qos_level)
			osm_qos_policy_qos_level_destroy(p_qos_level);
		list_iterator = cl_list_next(list_iterator);
	}
	cl_list_remove_all(&p_qos_policy->qos_levels);
	cl_list_destroy(&p_qos_policy->qos_levels);

	list_iterator = cl_list_head(&p_qos_policy->qos_match_rules);
	while (list_iterator != cl_list_end(&p_qos_policy->qos_match_rules)) {
		p_qos_match_rule =
		    (osm_qos_match_rule_t *) cl_list_obj(list_iterator);
		if (p_qos_match_rule)
			osm_qos_policy_match_rule_destroy(p_qos_match_rule);
		list_iterator = cl_list_next(list_iterator);
	}
	cl_list_remove_all(&p_qos_policy->qos_match_rules);
	cl_list_destroy(&p_qos_policy->qos_match_rules);

	free(p_qos_policy);

	p_qos_policy = NULL;
}

/***************************************************
 ***************************************************/

static boolean_t
__qos_policy_is_port_in_group(osm_subn_t * p_subn,
			      const osm_physp_t * p_physp,
			      osm_qos_port_group_t * p_port_group)
{
	osm_node_t *p_node = osm_physp_get_node_ptr(p_physp);
	osm_prtn_t *p_prtn = NULL;
	ib_net64_t port_guid = osm_physp_get_port_guid(p_physp);
	uint64_t port_guid_ho = cl_ntoh64(port_guid);
	uint8_t node_type = osm_node_get_type(p_node);
	cl_list_iterator_t list_iterator;
	char *partition_name;

	/* check whether this port's type matches any of group's types */

	if ((node_type == IB_NODE_TYPE_CA && p_port_group->node_type_ca) ||
	    (node_type == IB_NODE_TYPE_SWITCH && p_port_group->node_type_switch)
	    || (node_type == IB_NODE_TYPE_ROUTER
		&& p_port_group->node_type_router))
		return TRUE;

	/* check whether this port's guid is in range of this group's guids */

	if (__is_num_in_range_arr(p_port_group->guid_range_arr,
				  p_port_group->guid_range_len, port_guid_ho))
		return TRUE;

	/* check whether this port is member of this group's partitions */

	list_iterator = cl_list_head(&p_port_group->partition_list);
	while (list_iterator != cl_list_end(&p_port_group->partition_list)) {
		partition_name = (char *)cl_list_obj(list_iterator);
		if (partition_name && strlen(partition_name)) {
			p_prtn = osm_prtn_find_by_name(p_subn, partition_name);
			if (p_prtn) {
				if (osm_prtn_is_guid(p_prtn, port_guid))
					return TRUE;
			}
		}
		list_iterator = cl_list_next(list_iterator);
	}

	/* check whether this port's name matches any of group's names */

	/*
	 * TODO: check port names
	 *
	 *  char desc[IB_NODE_DESCRIPTION_SIZE + 1];
	 *  memcpy(desc, p_node->node_desc.description, IB_NODE_DESCRIPTION_SIZE);
	 *  desc[IB_NODE_DESCRIPTION_SIZE] = '\0';
	 */

	return FALSE;
}				/* __qos_policy_is_port_in_group() */

/***************************************************
 ***************************************************/

static boolean_t
__qos_policy_is_port_in_group_list(const osm_pr_rcv_t * p_rcv,
				   const osm_physp_t * p_physp,
				   cl_list_t * p_port_group_list)
{
	osm_qos_port_group_t *p_port_group;
	cl_list_iterator_t list_iterator;

	list_iterator = cl_list_head(p_port_group_list);
	while (list_iterator != cl_list_end(p_port_group_list)) {
		p_port_group =
		    (osm_qos_port_group_t *) cl_list_obj(list_iterator);
		if (p_port_group) {
			if (__qos_policy_is_port_in_group
			    (p_rcv->p_subn, p_physp, p_port_group))
				return TRUE;
		}
		list_iterator = cl_list_next(list_iterator);
	}
	return FALSE;
}

/***************************************************
 ***************************************************/

static osm_qos_match_rule_t *__qos_policy_get_match_rule_by_pr(
			 const osm_qos_policy_t * p_qos_policy,
			 const osm_pr_rcv_t * p_rcv,
			 const ib_path_rec_t * p_pr,
			 const osm_physp_t * p_src_physp,
			 const osm_physp_t * p_dest_physp,
			 ib_net64_t comp_mask)
{
	osm_qos_match_rule_t *p_qos_match_rule = NULL;
	cl_list_iterator_t list_iterator;

	if (!cl_list_count(&p_qos_policy->qos_match_rules))
		return NULL;

	/* Go over all QoS match rules and find the one that matches the request */

	list_iterator = cl_list_head(&p_qos_policy->qos_match_rules);
	while (list_iterator != cl_list_end(&p_qos_policy->qos_match_rules)) {
		p_qos_match_rule =
		    (osm_qos_match_rule_t *) cl_list_obj(list_iterator);
		if (!p_qos_match_rule) {
			list_iterator = cl_list_next(list_iterator);
			continue;
		}

		/* If a match rule has Source groups, PR request source has to be in this list */

		if (cl_list_count(&p_qos_match_rule->source_group_list)) {
			if (!__qos_policy_is_port_in_group_list(p_rcv,
								p_src_physp,
								&p_qos_match_rule->
								source_group_list))
			{
				list_iterator = cl_list_next(list_iterator);
				continue;
			}
		}

		/* If a match rule has Destination groups, PR request dest. has to be in this list */

		if (cl_list_count(&p_qos_match_rule->destination_group_list)) {
			if (!__qos_policy_is_port_in_group_list(p_rcv,
								p_dest_physp,
								&p_qos_match_rule->
								destination_group_list))
			{
				list_iterator = cl_list_next(list_iterator);
				continue;
			}
		}

		/* If a match rule has QoS classes, PR request HAS
		   to have a matching QoS class to match the rule */

		if (p_qos_match_rule->qos_class_range_len) {
			if (!(comp_mask & IB_PR_COMPMASK_QOS_CLASS)) {
				list_iterator = cl_list_next(list_iterator);
				continue;
			}

			if (!__is_num_in_range_arr
			    (p_qos_match_rule->qos_class_range_arr,
			     p_qos_match_rule->qos_class_range_len,
			     ib_path_rec_qos_class(p_pr))) {
				list_iterator = cl_list_next(list_iterator);
				continue;
			}

		}

		/* If a match rule has Service IDs, PR request HAS
		   to have a matching Service ID to match the rule */

		if (p_qos_match_rule->service_id_range_len) {
			if (!(comp_mask & IB_PR_COMPMASK_SERVICEID)) {
				list_iterator = cl_list_next(list_iterator);
				continue;
			}

			if (!__is_num_in_range_arr
			    (p_qos_match_rule->service_id_range_arr,
			     p_qos_match_rule->service_id_range_len,
			     p_pr->service_id)) {
				list_iterator = cl_list_next(list_iterator);
				continue;
			}

		}

		/* If a match rule has PKeys, PR request HAS
		   to have a matching PKey to match the rule */

		if (p_qos_match_rule->pkey_range_len) {
			if (!(comp_mask & IB_PR_COMPMASK_PKEY)) {
				list_iterator = cl_list_next(list_iterator);
				continue;
			}

			if (!__is_num_in_range_arr
			    (p_qos_match_rule->pkey_range_arr,
			     p_qos_match_rule->pkey_range_len,
			     ib_path_rec_qos_class(p_pr))) {
				list_iterator = cl_list_next(list_iterator);
				continue;
			}

		}

		/* if we got here, then this match-rule matched this PR request */
		break;
	}

	if (list_iterator == cl_list_end(&p_qos_policy->qos_match_rules))
		return NULL;

	return p_qos_match_rule;
}				/* __qos_policy_get_match_rule_by_pr() */

/***************************************************
 ***************************************************/

static osm_qos_level_t *__qos_policy_get_qos_level_by_name(osm_qos_policy_t * p_qos_policy,
							   char *name)
{
	osm_qos_level_t *p_qos_level = NULL;
	cl_list_iterator_t list_iterator;

	list_iterator = cl_list_head(&p_qos_policy->qos_levels);
	while (list_iterator != cl_list_end(&p_qos_policy->qos_levels)) {
		p_qos_level = (osm_qos_level_t *) cl_list_obj(list_iterator);
		if (!p_qos_level)
			continue;

		/* names are case INsensitive */
		if (strcasecmp(name, p_qos_level->name) == 0)
			return p_qos_level;

		list_iterator = cl_list_next(list_iterator);
	}

	return NULL;
}

/***************************************************
 ***************************************************/

static osm_qos_port_group_t *__qos_policy_get_port_group_by_name(osm_qos_policy_t * p_qos_policy,
								 const char *const name)
{
	osm_qos_port_group_t *p_port_group = NULL;
	cl_list_iterator_t list_iterator;

	list_iterator = cl_list_head(&p_qos_policy->port_groups);
	while (list_iterator != cl_list_end(&p_qos_policy->port_groups)) {
		p_port_group =
		    (osm_qos_port_group_t *) cl_list_obj(list_iterator);
		if (!p_port_group)
			continue;

		/* names are case INsensitive */
		if (strcasecmp(name, p_port_group->name) == 0)
			return p_port_group;

		list_iterator = cl_list_next(list_iterator);
	}

	return NULL;
}

/***************************************************
 ***************************************************/

int osm_qos_policy_validate(osm_qos_policy_t * p_qos_policy,
			    osm_log_t *p_log)
{
	cl_list_iterator_t match_rules_list_iterator;
	cl_list_iterator_t list_iterator;
	osm_qos_port_group_t *p_port_group = NULL;
	osm_qos_match_rule_t *p_qos_match_rule = NULL;
	char *str;
	unsigned i;
	int res = 0;

	OSM_LOG_ENTER(p_log, osm_qos_policy_validate);

	/* set default qos level */

	p_qos_policy->p_default_qos_level =
	    __qos_policy_get_qos_level_by_name(p_qos_policy, OSM_QOS_POLICY_DEFAULT_LEVEL_NAME);
	if (!p_qos_policy->p_default_qos_level) {
		osm_log(p_log, OSM_LOG_ERROR,
			"osm_qos_policy_validate: ERR AC10: "
			"Default qos-level (%s) not defined.\n",
			OSM_QOS_POLICY_DEFAULT_LEVEL_NAME);
		res = 1;
		goto Exit;
	}

	/* scan all the match rules, and fill the lists of pointers to
	   relevant qos levels and port groups to speed up PR matching */

	i = 1;
	match_rules_list_iterator =
	    cl_list_head(&p_qos_policy->qos_match_rules);
	while (match_rules_list_iterator !=
	       cl_list_end(&p_qos_policy->qos_match_rules)) {
		p_qos_match_rule =
		    (osm_qos_match_rule_t *)
		    cl_list_obj(match_rules_list_iterator);
		CL_ASSERT(p_qos_match_rule);

		/* find the matching qos-level for each match-rule */

		p_qos_match_rule->p_qos_level =
		    __qos_policy_get_qos_level_by_name(p_qos_policy,
						       p_qos_match_rule->qos_level_name);

		if (!p_qos_match_rule->p_qos_level) {
			osm_log(p_log, OSM_LOG_ERROR,
				"osm_qos_policy_validate: ERR AC11: "
				"qos-match-rule num %u: qos-level '%s' not found\n",
				i, p_qos_match_rule->qos_level_name);
			res = 1;
			goto Exit;
		}

		/* find the matching port-group for element of source_list */

		if (cl_list_count(&p_qos_match_rule->source_list)) {
			list_iterator =
			    cl_list_head(&p_qos_match_rule->source_list);
			while (list_iterator !=
			       cl_list_end(&p_qos_match_rule->source_list)) {
				str = (char *)cl_list_obj(list_iterator);
				CL_ASSERT(str);

				p_port_group =
				    __qos_policy_get_port_group_by_name(p_qos_policy, str);
				if (!p_port_group) {
					osm_log(p_log,
						OSM_LOG_ERROR,
						"osm_qos_policy_validate: ERR AC12: "
						"qos-match-rule num %u: source port-group '%s' not found\n",
						i, str);
					res = 1;
					goto Exit;
				}

				cl_list_insert_tail(&p_qos_match_rule->
						    source_group_list,
						    p_port_group);

				list_iterator = cl_list_next(list_iterator);
			}
		}

		/* find the matching port-group for element of destination_list */

		if (cl_list_count(&p_qos_match_rule->destination_list)) {
			list_iterator =
			    cl_list_head(&p_qos_match_rule->destination_list);
			while (list_iterator !=
			       cl_list_end(&p_qos_match_rule->
					   destination_list)) {
				str = (char *)cl_list_obj(list_iterator);
				CL_ASSERT(str);

				p_port_group =
				    __qos_policy_get_port_group_by_name(p_qos_policy,str);
				if (!p_port_group) {
					osm_log(p_log,
						OSM_LOG_ERROR,
						"osm_qos_policy_validate: ERR AC13: "
						"qos-match-rule num %u: destination port-group '%s' not found\n",
						i, str);
					res = 1;
					goto Exit;
				}

				cl_list_insert_tail(&p_qos_match_rule->
						    destination_group_list,
						    p_port_group);

				list_iterator = cl_list_next(list_iterator);
			}
		}

		/* done with the current match-rule */

		match_rules_list_iterator =
		    cl_list_next(match_rules_list_iterator);
		i++;
	}

      Exit:
	OSM_LOG_EXIT(p_log);
	return res;
}				/* osm_qos_policy_validate() */

/***************************************************
 ***************************************************/

void osm_qos_policy_get_qos_level_by_pr(IN const osm_qos_policy_t * p_qos_policy,
					IN const osm_pr_rcv_t * p_rcv,
					IN const ib_path_rec_t * p_pr,
					IN const osm_physp_t * p_src_physp,
					IN const osm_physp_t * p_dest_physp,
					IN ib_net64_t comp_mask,
					OUT osm_qos_level_t ** pp_qos_level)
{
	osm_qos_match_rule_t *p_qos_match_rule = NULL;
	osm_qos_level_t *p_qos_level = NULL;

	OSM_LOG_ENTER(p_rcv->p_log, osm_qos_policy_get_qos_level_by_pr);

	*pp_qos_level = NULL;

	if (!p_qos_policy)
		goto Exit;

	p_qos_match_rule = __qos_policy_get_match_rule_by_pr(p_qos_policy,
							     p_rcv,
							     p_pr,
							     p_src_physp,
							     p_dest_physp,
							     comp_mask);

	if (p_qos_match_rule)
		p_qos_level = p_qos_match_rule->p_qos_level;
	else
		p_qos_level = p_qos_policy->p_default_qos_level;

	osm_log(p_rcv->p_log, OSM_LOG_DEBUG,
		"osm_qos_policy_get_qos_level_by_pr: "
		"PathRecord request:"
		"Src port 0x%016" PRIx64 ", "
		"Dst port 0x%016" PRIx64 "\n",
		cl_ntoh64(osm_physp_get_port_guid(p_src_physp)),
		cl_ntoh64(osm_physp_get_port_guid(p_dest_physp)));
	osm_log(p_rcv->p_log, OSM_LOG_DEBUG,
		"osm_qos_policy_get_qos_level_by_pr: "
		"Applying QoS Level %s (%s)\n",
		p_qos_level->name,
		(p_qos_level->use) ? p_qos_level->use : "no description");

	*pp_qos_level = p_qos_level;

      Exit:
	OSM_LOG_EXIT(p_rcv->p_log);
}				/* osm_qos_policy_get_qos_level_by_pr() */

/***************************************************
 ***************************************************/