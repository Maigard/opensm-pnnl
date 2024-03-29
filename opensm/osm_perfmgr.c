/*
 * Copyright (c) 2007 The Regents of the University of California.
 * Copyright (c) 2007-2009 Voltaire, Inc. All rights reserved.
 * Copyright (c) 2009,2010 HNR Consulting. All rights reserved.
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
 *    Implementation of osm_perfmgr_t.
 * This object implements an IBA performance manager.
 *
 * Author:
 *    Ira Weiny, LLNL
 */

#if HAVE_CONFIG_H
#  include <config.h>
#endif				/* HAVE_CONFIG_H */

#ifdef ENABLE_OSM_PERF_MGR
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <poll.h>
#include <errno.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <float.h>
#include <arpa/inet.h>
#include <iba/ib_types.h>
#include <complib/cl_debug.h>
#include <complib/cl_thread.h>
#include <opensm/osm_file_ids.h>
#define FILE_ID OSM_FILE_PERFMGR_C
#include <vendor/osm_vendor_api.h>
#include <opensm/osm_perfmgr.h>
#include <opensm/osm_log.h>
#include <opensm/osm_node.h>
#include <opensm/osm_opensm.h>
#include <opensm/osm_helper.h>

#define PERFMGR_INITIAL_TID_VALUE 0xcafe

#ifdef ENABLE_OSM_PERF_MGR_PROFILE
struct {
	double fastest_us;
	double slowest_us;
	double avg_us;
	uint64_t num;
} perfmgr_mad_stats = {
fastest_us: DBL_MAX, slowest_us: DBL_MIN, avg_us: 0, num:0};

/* diff must be something which can fit in a susecond_t */
static inline void update_mad_stats(struct timeval *diff)
{
	double new = (diff->tv_sec * 1000000) + diff->tv_usec;
	if (new < perfmgr_mad_stats.fastest_us)
		perfmgr_mad_stats.fastest_us = new;
	if (new > perfmgr_mad_stats.slowest_us)
		perfmgr_mad_stats.slowest_us = new;

	perfmgr_mad_stats.avg_us =
	    ((perfmgr_mad_stats.avg_us * perfmgr_mad_stats.num) + new)
	    / (perfmgr_mad_stats.num + 1);
	perfmgr_mad_stats.num++;
}

static inline void clear_mad_stats(void)
{
	perfmgr_mad_stats.fastest_us = DBL_MAX;
	perfmgr_mad_stats.slowest_us = DBL_MIN;
	perfmgr_mad_stats.avg_us = 0;
	perfmgr_mad_stats.num = 0;
}

/* after and diff can be the same struct */
static inline void diff_time(struct timeval *before, struct timeval *after,
			     struct timeval *diff)
{
	struct timeval tmp = *after;
	if (tmp.tv_usec < before->tv_usec) {
		tmp.tv_sec--;
		tmp.tv_usec += 1000000;
	}
	diff->tv_sec = tmp.tv_sec - before->tv_sec;
	diff->tv_usec = tmp.tv_usec - before->tv_usec;
}
#endif

/**********************************************************************
 * Internal helper functions.
 **********************************************************************/
static void init_monitored_nodes(osm_perfmgr_t * pm)
{
	cl_qmap_init(&pm->monitored_map);
	pm->remove_list = NULL;
	cl_event_construct(&pm->sig_query);
	cl_event_init(&pm->sig_query, FALSE);
}

static void mark_for_removal(osm_perfmgr_t * pm, monitored_node_t * node)
{
	if (pm->remove_list) {
		node->next = pm->remove_list;
		pm->remove_list = node;
	} else {
		node->next = NULL;
		pm->remove_list = node;
	}
}

static void remove_marked_nodes(osm_perfmgr_t * pm)
{
	while (pm->remove_list) {
		monitored_node_t *next = pm->remove_list->next;

		cl_qmap_remove_item(&pm->monitored_map,
				    (cl_map_item_t *) (pm->remove_list));

		if (pm->rm_nodes)
			perfmgr_db_delete_entry(pm->db, pm->remove_list->guid);
		else
			perfmgr_db_mark_active(pm->db, pm->remove_list->guid, FALSE);

		if (pm->remove_list->name)
			free(pm->remove_list->name);
		free(pm->remove_list);
		pm->remove_list = next;
	}
}

static inline void decrement_outstanding_queries(osm_perfmgr_t * pm)
{
	cl_atomic_dec(&pm->outstanding_queries);
	cl_event_signal(&pm->sig_query);
}

/**********************************************************************
 * Receive the MAD from the vendor layer and post it for processing by
 * the dispatcher.
 **********************************************************************/
static void perfmgr_mad_recv_callback(osm_madw_t * p_madw, void *bind_context,
				      osm_madw_t * p_req_madw)
{
	osm_perfmgr_t *pm = (osm_perfmgr_t *) bind_context;

	OSM_LOG_ENTER(pm->log);

	osm_madw_copy_context(p_madw, p_req_madw);
	osm_mad_pool_put(pm->mad_pool, p_req_madw);

	decrement_outstanding_queries(pm);

	/* post this message for later processing. */
	if (cl_disp_post(pm->pc_disp_h, OSM_MSG_MAD_PORT_COUNTERS,
			 p_madw, NULL, NULL) != CL_SUCCESS) {
		OSM_LOG(pm->log, OSM_LOG_ERROR, "ERR 5401: "
			"PerfMgr Dispatcher post failed\n");
		osm_mad_pool_put(pm->mad_pool, p_madw);
	}
	OSM_LOG_EXIT(pm->log);
}

/**********************************************************************
 * Process MAD send errors.
 **********************************************************************/
static void perfmgr_mad_send_err_callback(void *bind_context,
					  osm_madw_t * p_madw)
{
	osm_perfmgr_t *pm = (osm_perfmgr_t *) bind_context;
	osm_madw_context_t *context = &p_madw->context;
	uint64_t node_guid = context->perfmgr_context.node_guid;
	uint8_t port = context->perfmgr_context.port;
	cl_map_item_t *p_node;
	monitored_node_t *p_mon_node;
	ib_net16_t orig_lid;

	OSM_LOG_ENTER(pm->log);

	/*
	 * get the monitored node struct to have the printable name
	 * for log messages
	 */
	if ((p_node = cl_qmap_get(&pm->monitored_map, node_guid)) ==
	    cl_qmap_end(&pm->monitored_map)) {
		OSM_LOG(pm->log, OSM_LOG_ERROR, "ERR 5415: GUID 0x%016"
			PRIx64 " not found in monitored map\n", node_guid);
		goto Exit;
	}
	p_mon_node = (monitored_node_t *) p_node;

	OSM_LOG(pm->log, OSM_LOG_ERROR, "ERR 5402: %s (0x%" PRIx64
		") port %u LID %u TID 0x%" PRIx64 "\n",
		p_mon_node->name, p_mon_node->guid, port,
		cl_ntoh16(p_madw->mad_addr.dest_lid),
		cl_ntoh64(p_madw->p_mad->trans_id));

	if (pm->subn->opt.perfmgr_redir && p_madw->status == IB_TIMEOUT) {
		/* First, find the node in the monitored map */
		cl_plock_acquire(&pm->osm->lock);
		/* Now, validate port number */
		if (port >= p_mon_node->num_ports) {
			cl_plock_release(&pm->osm->lock);
			OSM_LOG(pm->log, OSM_LOG_ERROR, "ERR 5416: "
				"Invalid port num %u for %s (GUID 0x%016"
				PRIx64 ") num ports %u\n", port,
				p_mon_node->name, p_mon_node->guid,
				p_mon_node->num_ports);
			goto Exit;
		}
		/* Clear redirection info for this port except orig_lid */
		orig_lid = p_mon_node->port[port].orig_lid;
		memset(&p_mon_node->port[port], 0, sizeof(monitored_port_t));
		p_mon_node->port[port].orig_lid = orig_lid;
		p_mon_node->port[port].valid = TRUE;
		cl_plock_release(&pm->osm->lock);
	}

Exit:
	osm_mad_pool_put(pm->mad_pool, p_madw);

	decrement_outstanding_queries(pm);

	OSM_LOG_EXIT(pm->log);
}

/**********************************************************************
 * Bind the PerfMgr to the vendor layer for MAD sends/receives
 **********************************************************************/
ib_api_status_t osm_perfmgr_bind(osm_perfmgr_t * pm, ib_net64_t port_guid)
{
	osm_bind_info_t bind_info;
	ib_api_status_t status = IB_SUCCESS;

	OSM_LOG_ENTER(pm->log);

	if (pm->bind_handle != OSM_BIND_INVALID_HANDLE) {
		OSM_LOG(pm->log, OSM_LOG_ERROR,
			"ERR 5403: Multiple binds not allowed\n");
		status = IB_ERROR;
		goto Exit;
	}

	bind_info.port_guid = pm->port_guid = port_guid;
	bind_info.mad_class = IB_MCLASS_PERF;
	bind_info.class_version = 1;
	bind_info.is_responder = FALSE;
	bind_info.is_report_processor = FALSE;
	bind_info.is_trap_processor = FALSE;
	bind_info.recv_q_size = OSM_PM_DEFAULT_QP1_RCV_SIZE;
	bind_info.send_q_size = OSM_PM_DEFAULT_QP1_SEND_SIZE;
	bind_info.timeout = pm->subn->opt.transaction_timeout;
	bind_info.retries = pm->subn->opt.transaction_retries;

	OSM_LOG(pm->log, OSM_LOG_VERBOSE,
		"Binding to port GUID 0x%" PRIx64 "\n", cl_ntoh64(port_guid));

	pm->bind_handle = osm_vendor_bind(pm->vendor, &bind_info, pm->mad_pool,
					  perfmgr_mad_recv_callback,
					  perfmgr_mad_send_err_callback, pm);

	if (pm->bind_handle == OSM_BIND_INVALID_HANDLE) {
		status = IB_ERROR;
		OSM_LOG(pm->log, OSM_LOG_ERROR,
			"ERR 5404: Vendor specific bind failed (%s)\n",
			ib_get_err_str(status));
	}

Exit:
	OSM_LOG_EXIT(pm->log);
	return status;
}

/**********************************************************************
 * Unbind the PerfMgr from the vendor layer for MAD sends/receives
 **********************************************************************/
static void perfmgr_mad_unbind(osm_perfmgr_t * pm)
{
	OSM_LOG_ENTER(pm->log);
	if (pm->bind_handle == OSM_BIND_INVALID_HANDLE) {
		OSM_LOG(pm->log, OSM_LOG_ERROR, "ERR 5405: No previous bind\n");
		goto Exit;
	}
	osm_vendor_unbind(pm->bind_handle);
Exit:
	OSM_LOG_EXIT(pm->log);
}

/**********************************************************************
 * Given a monitored node and a port, return the qp
 **********************************************************************/
static ib_net32_t get_qp(monitored_node_t * mon_node, uint8_t port)
{
	ib_net32_t qp = IB_QP1;

	if (mon_node && mon_node->num_ports && port < mon_node->num_ports &&
	    mon_node->port[port].redirection && mon_node->port[port].qp)
		qp = mon_node->port[port].qp;

	return qp;
}

static ib_net16_t get_base_lid(osm_node_t * p_node, uint8_t port)
{
	switch (p_node->node_info.node_type) {
	case IB_NODE_TYPE_CA:
	case IB_NODE_TYPE_ROUTER:
		return osm_node_get_base_lid(p_node, port);
	case IB_NODE_TYPE_SWITCH:
		return osm_node_get_base_lid(p_node, 0);
	default:
		return 0;
	}
}

/**********************************************************************
 * Given a node, a port, and an optional monitored node,
 * return the lid appropriate to query that port
 **********************************************************************/
static ib_net16_t get_lid(osm_node_t * p_node, uint8_t port,
			  monitored_node_t * mon_node)
{
	if (mon_node && mon_node->num_ports && port < mon_node->num_ports &&
	    mon_node->port[port].lid)
		return mon_node->port[port].lid;

	return get_base_lid(p_node, port);
}

/**********************************************************************
 * Form and send the Port Counters MAD for a single port.
 **********************************************************************/
static ib_api_status_t perfmgr_send_pc_mad(osm_perfmgr_t * perfmgr,
					   ib_net16_t dest_lid,
					   ib_net32_t dest_qp, uint16_t pkey_ix,
					   uint8_t port, uint8_t mad_method,
					   osm_madw_context_t * p_context)
{
	ib_api_status_t status = IB_SUCCESS;
	ib_port_counters_t *port_counter = NULL;
	ib_perfmgt_mad_t *pm_mad = NULL;
	osm_madw_t *p_madw = NULL;

	OSM_LOG_ENTER(perfmgr->log);

	p_madw = osm_mad_pool_get(perfmgr->mad_pool, perfmgr->bind_handle,
				  MAD_BLOCK_SIZE, NULL);
	if (p_madw == NULL)
		return IB_INSUFFICIENT_MEMORY;

	pm_mad = osm_madw_get_perfmgt_mad_ptr(p_madw);

	/* build the mad */
	pm_mad->header.base_ver = 1;
	pm_mad->header.mgmt_class = IB_MCLASS_PERF;
	pm_mad->header.class_ver = 1;
	pm_mad->header.method = mad_method;
	pm_mad->header.status = 0;
	pm_mad->header.class_spec = 0;
	pm_mad->header.trans_id =
	    cl_hton64((uint64_t) cl_atomic_inc(&perfmgr->trans_id) &
		      (uint64_t) (0xFFFFFFFF));
	if (perfmgr->trans_id == 0)
		pm_mad->header.trans_id =
		    cl_hton64((uint64_t) cl_atomic_inc(&perfmgr->trans_id) &
			      (uint64_t) (0xFFFFFFFF));
	pm_mad->header.attr_id = IB_MAD_ATTR_PORT_CNTRS;
	pm_mad->header.resv = 0;
	pm_mad->header.attr_mod = 0;

	port_counter = (ib_port_counters_t *) & pm_mad->data;
	memset(port_counter, 0, sizeof(*port_counter));
	port_counter->port_select = port;
	port_counter->counter_select = 0xFFFF;

	p_madw->mad_addr.dest_lid = dest_lid;
	p_madw->mad_addr.addr_type.gsi.remote_qp = dest_qp;
	p_madw->mad_addr.addr_type.gsi.remote_qkey =
	    cl_hton32(IB_QP1_WELL_KNOWN_Q_KEY);
	p_madw->mad_addr.addr_type.gsi.pkey_ix = pkey_ix;
	p_madw->mad_addr.addr_type.gsi.service_level = 0;
	p_madw->mad_addr.addr_type.gsi.global_route = FALSE;
	p_madw->resp_expected = TRUE;

	if (p_context)
		p_madw->context = *p_context;

	status = osm_vendor_send(perfmgr->bind_handle, p_madw, TRUE);

	if (status == IB_SUCCESS) {
		/* pause thread if there are too many outstanding requests */
		cl_atomic_inc(&(perfmgr->outstanding_queries));
		while (perfmgr->outstanding_queries >
		       (int32_t)perfmgr->max_outstanding_queries) {
			perfmgr->sweep_state = PERFMGR_SWEEP_SUSPENDED;
			cl_event_wait_on(&perfmgr->sig_query, EVENT_NO_TIMEOUT,
					 TRUE);
		}
		perfmgr->sweep_state = PERFMGR_SWEEP_ACTIVE;
	}

	OSM_LOG_EXIT(perfmgr->log);
	return status;
}

/**********************************************************************
 * sweep the node_guid_tbl and collect the node guids to be tracked
 **********************************************************************/
static void collect_guids(cl_map_item_t * p_map_item, void *context)
{
	osm_node_t *node = (osm_node_t *) p_map_item;
	uint64_t node_guid = cl_ntoh64(node->node_info.node_guid);
	osm_perfmgr_t *pm = (osm_perfmgr_t *) context;
	monitored_node_t *mon_node = NULL;
	uint32_t num_ports;
	int port;

	OSM_LOG_ENTER(pm->log);

	if (cl_qmap_get(&pm->monitored_map, node_guid)
	    == cl_qmap_end(&pm->monitored_map)) {

		if (pm->ignore_cas &&
		    (node->node_info.node_type == IB_NODE_TYPE_CA))
			goto Exit;

		/* if not already in map add it */
		num_ports = osm_node_get_num_physp(node);
		mon_node = malloc(sizeof(*mon_node) +
				  sizeof(monitored_port_t) * num_ports);
		if (!mon_node) {
			OSM_LOG(pm->log, OSM_LOG_ERROR, "PerfMgr: ERR 5406: "
				"malloc failed: not handling node %s"
				"(GUID 0x%" PRIx64 ")\n", node->print_desc,
				node_guid);
			goto Exit;
		}
		memset(mon_node, 0,
		       sizeof(*mon_node) + sizeof(monitored_port_t) * num_ports);
		mon_node->guid = node_guid;
		mon_node->name = strdup(node->print_desc);
		mon_node->num_ports = num_ports;
		/* check for enhanced switch port 0 */
		mon_node->esp0 = (node->sw &&
				  ib_switch_info_is_enhanced_port0(&node->sw->
								   switch_info));
		for (port = mon_node->esp0 ? 0 : 1; port < num_ports; port++) {
			mon_node->port[port].orig_lid = 0;
			mon_node->port[port].valid = FALSE;
			if (osm_physp_is_valid(&node->physp_table[port])) {
				mon_node->port[port].orig_lid = get_base_lid(node, port);
				mon_node->port[port].valid = TRUE;
			}
		}

		cl_qmap_insert(&pm->monitored_map, node_guid,
			       (cl_map_item_t *) mon_node);
	}

Exit:
	OSM_LOG_EXIT(pm->log);
}

/**********************************************************************
 * query the Port Counters of all the nodes in the subnet.
 **********************************************************************/
static void perfmgr_query_counters(cl_map_item_t * p_map_item, void *context)
{
	ib_api_status_t status = IB_SUCCESS;
	osm_perfmgr_t *pm = context;
	osm_node_t *node = NULL;
	monitored_node_t *mon_node = (monitored_node_t *) p_map_item;
	osm_madw_context_t mad_context;
	uint64_t node_guid = 0;
	ib_net32_t remote_qp;
	uint8_t port, num_ports = 0;

	OSM_LOG_ENTER(pm->log);

	cl_plock_acquire(&pm->osm->lock);
	node = osm_get_node_by_guid(pm->subn, cl_hton64(mon_node->guid));
	if (!node) {
		OSM_LOG(pm->log, OSM_LOG_ERROR,
			"ERR 5407: Node \"%s\" (guid 0x%" PRIx64
			") no longer exists so removing from PerfMgr monitoring\n",
			mon_node->name, mon_node->guid);
		mark_for_removal(pm, mon_node);
		goto Exit;
	}

	num_ports = osm_node_get_num_physp(node);
	node_guid = cl_ntoh64(node->node_info.node_guid);

	/* make sure there is a database object ready to store this info */
	if (perfmgr_db_create_entry(pm->db, node_guid, mon_node->esp0,
				    num_ports, node->print_desc) !=
	    PERFMGR_EVENT_DB_SUCCESS) {
		OSM_LOG(pm->log, OSM_LOG_ERROR,
			"ERR 5408: DB create entry failed for 0x%"
			PRIx64 " (%s) : %s\n", node_guid, node->print_desc,
			strerror(errno));
		goto Exit;
	}

	perfmgr_db_mark_active(pm->db, node_guid, TRUE);

	/* issue the query for each port */
	for (port = mon_node->esp0 ? 0 : 1; port < num_ports; port++) {
		ib_net16_t lid;

		if (!osm_node_get_physp_ptr(node, port))
			continue;

		if (!mon_node->port[port].valid)
			continue;

		lid = get_lid(node, port, mon_node);
		if (lid == 0) {
			OSM_LOG(pm->log, OSM_LOG_DEBUG, "WARN: node 0x%" PRIx64
				" port %d (%s): port out of range, skipping\n",
				cl_ntoh64(node->node_info.node_guid), port,
				node->print_desc);
			continue;
		}

		remote_qp = get_qp(mon_node, port);

		mad_context.perfmgr_context.node_guid = node_guid;
		mad_context.perfmgr_context.port = port;
		mad_context.perfmgr_context.mad_method = IB_MAD_METHOD_GET;
#ifdef ENABLE_OSM_PERF_MGR_PROFILE
		gettimeofday(&mad_context.perfmgr_context.query_start, NULL);
#endif
		OSM_LOG(pm->log, OSM_LOG_VERBOSE, "Getting stats for node 0x%"
			PRIx64 " port %d (lid %u) (%s)\n", node_guid, port,
			cl_ntoh16(lid), node->print_desc);
		status = perfmgr_send_pc_mad(pm, lid, remote_qp,
					     mon_node->port[port].pkey_ix,
					     port, IB_MAD_METHOD_GET,
					     &mad_context);
		if (status != IB_SUCCESS)
			OSM_LOG(pm->log, OSM_LOG_ERROR, "ERR 5409: "
				"Failed to issue port counter query for node 0x%"
				PRIx64 " port %d (%s)\n",
				node->node_info.node_guid, port,
				node->print_desc);
	}
Exit:
	cl_plock_release(&pm->osm->lock);
	OSM_LOG_EXIT(pm->log);
}

/**********************************************************************
 * Discovery stuff.
 * This code should not be here, but merged with main OpenSM
 **********************************************************************/
extern int wait_for_pending_transactions(osm_stats_t * stats);
extern void osm_drop_mgr_process(IN osm_sm_t * sm);

static int sweep_hop_1(osm_sm_t * sm)
{
	ib_api_status_t status = IB_SUCCESS;
	osm_madw_context_t context;
	osm_node_t *p_node;
	osm_port_t *p_port;
	osm_dr_path_t hop_1_path;
	ib_net64_t port_guid;
	uint8_t port_num;
	uint8_t path_array[IB_SUBNET_PATH_HOPS_MAX];
	uint8_t num_ports;
	osm_physp_t *p_ext_physp;

	port_guid = sm->p_subn->sm_port_guid;

	p_port = osm_get_port_by_guid(sm->p_subn, port_guid);
	if (!p_port) {
		OSM_LOG(sm->p_log, OSM_LOG_ERROR,
			"ERR 5481: No SM port object\n");
		return -1;
	}

	p_node = p_port->p_node;
	port_num = ib_node_info_get_local_port_num(&p_node->node_info);

	OSM_LOG(sm->p_log, OSM_LOG_DEBUG,
		"Probing hop 1 on local port %u\n", port_num);

	memset(path_array, 0, sizeof(path_array));
	/* the hop_1 operations depend on the type of our node.
	 * Currently - legal nodes that can host SM are SW and CA */
	switch (osm_node_get_type(p_node)) {
	case IB_NODE_TYPE_CA:
	case IB_NODE_TYPE_ROUTER:
		memset(&context, 0, sizeof(context));
		context.ni_context.node_guid = osm_node_get_node_guid(p_node);
		context.ni_context.port_num = port_num;

		path_array[1] = port_num;

		osm_dr_path_init(&hop_1_path, 1, path_array);
		CL_PLOCK_ACQUIRE(sm->p_lock);
		status = osm_req_get(sm, &hop_1_path, IB_MAD_ATTR_NODE_INFO, 0,
				     CL_DISP_MSGID_NONE, &context);
		CL_PLOCK_RELEASE(sm->p_lock);

		if (status != IB_SUCCESS)
			OSM_LOG(sm->p_log, OSM_LOG_ERROR, "ERR 5482: "
				"Request for NodeInfo failed\n");
		break;

	case IB_NODE_TYPE_SWITCH:
		/* Need to go over all the ports of the switch, and send a node_info
		 * from them. This doesn't include the port 0 of the switch, which
		 * hosts the SM.
		 * Note: We'll send another switchInfo on port 0, since if no ports
		 * are connected, we still want to get some response, and have the
		 * subnet come up.
		 */
		num_ports = osm_node_get_num_physp(p_node);
		for (port_num = 0; port_num < num_ports; port_num++) {
			/* go through the port only if the port is not DOWN */
			p_ext_physp = osm_node_get_physp_ptr(p_node, port_num);
			if (!p_ext_physp || ib_port_info_get_port_state
			    (&p_ext_physp->port_info) <= IB_LINK_DOWN)
				continue;

			memset(&context, 0, sizeof(context));
			context.ni_context.node_guid =
			    osm_node_get_node_guid(p_node);
			context.ni_context.port_num = port_num;

			path_array[1] = port_num;

			osm_dr_path_init(&hop_1_path, 1, path_array);
			CL_PLOCK_ACQUIRE(sm->p_lock);
			status = osm_req_get(sm, &hop_1_path,
					     IB_MAD_ATTR_NODE_INFO, 0,
					     CL_DISP_MSGID_NONE, &context);
			CL_PLOCK_RELEASE(sm->p_lock);

			if (status != IB_SUCCESS)
				OSM_LOG(sm->p_log, OSM_LOG_ERROR, "ERR 5484: "
					"Request for NodeInfo failed\n");
		}
		break;

	default:
		OSM_LOG(sm->p_log, OSM_LOG_ERROR,
			"ERR 5483: Unknown node type %d\n",
			osm_node_get_type(p_node));
	}

	return status;
}

static unsigned is_sm_port_down(osm_sm_t * sm)
{
	ib_net64_t port_guid;
	osm_port_t *p_port;

	port_guid = sm->p_subn->sm_port_guid;
	if (port_guid == 0)
		return 1;

	CL_PLOCK_ACQUIRE(sm->p_lock);
	p_port = osm_get_port_by_guid(sm->p_subn, port_guid);
	if (!p_port) {
		CL_PLOCK_RELEASE(sm->p_lock);
		OSM_LOG(sm->p_log, OSM_LOG_ERROR, "ERR 5485: "
			"SM port with GUID:%016" PRIx64 " is unknown\n",
			cl_ntoh64(port_guid));
		return 1;
	}
	CL_PLOCK_RELEASE(sm->p_lock);

	if (p_port->p_node->sw &&
	    !ib_switch_info_is_enhanced_port0(&p_port->p_node->sw->switch_info))
		return 0;	/* base SP0 */

	return osm_physp_get_port_state(p_port->p_physp) == IB_LINK_DOWN;
}

static int sweep_hop_0(osm_sm_t * sm)
{
	ib_api_status_t status;
	osm_dr_path_t dr_path;
	osm_bind_handle_t h_bind;
	uint8_t path_array[IB_SUBNET_PATH_HOPS_MAX];

	memset(path_array, 0, sizeof(path_array));

	h_bind = osm_sm_mad_ctrl_get_bind_handle(&sm->mad_ctrl);
	if (h_bind == OSM_BIND_INVALID_HANDLE) {
		OSM_LOG(sm->p_log, OSM_LOG_DEBUG, "No bound ports\n");
		return -1;
	}

	osm_dr_path_init(&dr_path, 0, path_array);
	CL_PLOCK_ACQUIRE(sm->p_lock);
	status = osm_req_get(sm, &dr_path, IB_MAD_ATTR_NODE_INFO, 0,
			     CL_DISP_MSGID_NONE, NULL);
	CL_PLOCK_RELEASE(sm->p_lock);

	if (status != IB_SUCCESS)
		OSM_LOG(sm->p_log, OSM_LOG_ERROR,
			"ERR 5486: Request for NodeInfo failed\n");

	return status;
}

static void reset_node_count(cl_map_item_t * p_map_item, void *cxt)
{
	osm_node_t *p_node = (osm_node_t *) p_map_item;
	p_node->discovery_count = 0;
}

static void reset_port_count(cl_map_item_t * p_map_item, void *cxt)
{
	osm_port_t *p_port = (osm_port_t *) p_map_item;
	p_port->discovery_count = 0;
}

static void reset_switch_count(cl_map_item_t * p_map_item, void *cxt)
{
	osm_switch_t *p_sw = (osm_switch_t *) p_map_item;
	p_sw->need_update = 0;
}

static int perfmgr_discovery(osm_opensm_t * osm)
{
	int ret;

	CL_PLOCK_ACQUIRE(&osm->lock);
	cl_qmap_apply_func(&osm->subn.node_guid_tbl, reset_node_count, NULL);
	cl_qmap_apply_func(&osm->subn.port_guid_tbl, reset_port_count, NULL);
	cl_qmap_apply_func(&osm->subn.sw_guid_tbl, reset_switch_count, NULL);
	CL_PLOCK_RELEASE(&osm->lock);

	osm->subn.in_sweep_hop_0 = TRUE;

	ret = sweep_hop_0(&osm->sm);
	if (ret)
		goto _exit;

	if (wait_for_pending_transactions(&osm->stats))
		goto _exit;

	if (is_sm_port_down(&osm->sm)) {
		OSM_LOG(&osm->log, OSM_LOG_VERBOSE, "SM port is down\n");
		goto _drop;
	}

	osm->subn.in_sweep_hop_0 = FALSE;

	ret = sweep_hop_1(&osm->sm);
	if (ret)
		goto _exit;

	if (wait_for_pending_transactions(&osm->stats))
		goto _exit;

_drop:
	osm_drop_mgr_process(&osm->sm);

_exit:
	return ret;
}

/**********************************************************************
 * Main PerfMgr processor - query the performance counters.
 **********************************************************************/
void osm_perfmgr_process(osm_perfmgr_t * pm)
{
#ifdef ENABLE_OSM_PERF_MGR_PROFILE
	struct timeval before, after;
#endif

	if (pm->state != PERFMGR_STATE_ENABLED)
		return;

	if (pm->sweep_state == PERFMGR_SWEEP_ACTIVE ||
	    pm->sweep_state == PERFMGR_SWEEP_SUSPENDED)
		return;

	pm->sweep_state = PERFMGR_SWEEP_ACTIVE;

	if (pm->subn->sm_state == IB_SMINFO_STATE_STANDBY ||
	    pm->subn->sm_state == IB_SMINFO_STATE_NOTACTIVE)
		perfmgr_discovery(pm->subn->p_osm);

	/* if redirection enabled, determine local port */
	if (pm->subn->opt.perfmgr_redir && pm->local_port == -1) {
		osm_node_t *p_node;
		osm_port_t *p_port;

		CL_PLOCK_ACQUIRE(pm->sm->p_lock);
		p_port = osm_get_port_by_guid(pm->subn, pm->port_guid);
		if (p_port) {
			p_node = p_port->p_node;
			CL_ASSERT(p_node);
			pm->local_port =
			    ib_node_info_get_local_port_num(&p_node->node_info);
		} else
			OSM_LOG(pm->log, OSM_LOG_ERROR,
				"ERR 5487: No PerfMgr port object\n");
		CL_PLOCK_RELEASE(pm->sm->p_lock);
	}

#ifdef ENABLE_OSM_PERF_MGR_PROFILE
	gettimeofday(&before, NULL);
#endif
	/* With the global lock held, collect the node guids */
	/* FIXME we should be able to track SA notices
	 * and not have to sweep the node_guid_tbl each pass
	 */
	OSM_LOG(pm->log, OSM_LOG_VERBOSE, "Gathering PerfMgr stats\n");
	cl_plock_acquire(&pm->osm->lock);
	cl_qmap_apply_func(&pm->subn->node_guid_tbl, collect_guids, pm);
	cl_plock_release(&pm->osm->lock);

	/* then for each node query their counters */
	cl_qmap_apply_func(&pm->monitored_map, perfmgr_query_counters, pm);

	/* clean out any nodes found to be removed during the sweep */
	remove_marked_nodes(pm);

#ifdef ENABLE_OSM_PERF_MGR_PROFILE
	/* spin on outstanding queries */
	while (pm->outstanding_queries > 0)
		cl_event_wait_on(&pm->sig_sweep, 1000, TRUE);

	gettimeofday(&after, NULL);
	diff_time(&before, &after, &after);
	osm_log_v2(pm->log, OSM_LOG_INFO, FILE_ID,
		   "PerfMgr total sweep time : %ld.%06ld s\n"
		   "        fastest mad      : %g us\n"
		   "        slowest mad      : %g us\n"
		   "        average mad      : %g us\n",
		   after.tv_sec, after.tv_usec, perfmgr_mad_stats.fastest_us,
		   perfmgr_mad_stats.slowest_us, perfmgr_mad_stats.avg_us);
	clear_mad_stats();
#endif

	pm->sweep_state = PERFMGR_SWEEP_SLEEP;
}

/**********************************************************************
 * PerfMgr timer - loop continuously and signal SM to run PerfMgr
 * processor if enabled.
 **********************************************************************/
static void perfmgr_sweep(void *arg)
{
	osm_perfmgr_t *pm = arg;

	osm_sm_signal(pm->sm, OSM_SIGNAL_PERFMGR_SWEEP);
	cl_timer_start(&pm->sweep_timer, pm->sweep_time_s * 1000);
}

void osm_perfmgr_shutdown(osm_perfmgr_t * pm)
{
	OSM_LOG_ENTER(pm->log);
	cl_timer_stop(&pm->sweep_timer);
	cl_disp_unregister(pm->pc_disp_h);
	perfmgr_mad_unbind(pm);
	OSM_LOG_EXIT(pm->log);
}

void osm_perfmgr_destroy(osm_perfmgr_t * pm)
{
	OSM_LOG_ENTER(pm->log);
	perfmgr_db_destroy(pm->db);
	cl_timer_destroy(&pm->sweep_timer);
	OSM_LOG_EXIT(pm->log);
}

/**********************************************************************
 * Detect if someone else on the network could have cleared the counters
 * without us knowing.  This is easy to detect because the counters never
 * wrap but are "sticky"
 *
 * The one time this will not work is if the port is getting errors fast
 * enough to have the reading overtake the previous reading.  In this case,
 * counters will be missed.
 **********************************************************************/
static void perfmgr_check_oob_clear(osm_perfmgr_t * pm,
				    monitored_node_t * mon_node, uint8_t port,
				    perfmgr_db_err_reading_t * cr,
				    perfmgr_db_data_cnt_reading_t * dc)
{
	perfmgr_db_err_reading_t prev_err;
	perfmgr_db_data_cnt_reading_t prev_dc;

	if (perfmgr_db_get_prev_err(pm->db, mon_node->guid, port, &prev_err)
	    != PERFMGR_EVENT_DB_SUCCESS) {
		OSM_LOG(pm->log, OSM_LOG_VERBOSE, "Failed to find previous "
			"error reading for %s (guid 0x%" PRIx64 ") port %u\n",
			mon_node->name, mon_node->guid, port);
		return;
	}

	if (cr->symbol_err_cnt < prev_err.symbol_err_cnt ||
	    cr->link_err_recover < prev_err.link_err_recover ||
	    cr->link_downed < prev_err.link_downed ||
	    cr->rcv_err < prev_err.rcv_err ||
	    cr->rcv_rem_phys_err < prev_err.rcv_rem_phys_err ||
	    cr->rcv_switch_relay_err < prev_err.rcv_switch_relay_err ||
	    cr->xmit_discards < prev_err.xmit_discards ||
	    cr->xmit_constraint_err < prev_err.xmit_constraint_err ||
	    cr->rcv_constraint_err < prev_err.rcv_constraint_err ||
	    cr->link_integrity < prev_err.link_integrity ||
	    cr->buffer_overrun < prev_err.buffer_overrun ||
	    cr->vl15_dropped < prev_err.vl15_dropped) {
		OSM_LOG(pm->log, OSM_LOG_ERROR, "PerfMgr: ERR 540A: "
			"Detected an out of band error clear "
			"on %s (0x%" PRIx64 ") port %u\n",
			mon_node->name, mon_node->guid, port);
		perfmgr_db_clear_prev_err(pm->db, mon_node->guid, port);
	}

	/* FIXME handle extended counters */
	if (perfmgr_db_get_prev_dc(pm->db, mon_node->guid, port, &prev_dc)
	    != PERFMGR_EVENT_DB_SUCCESS) {
		OSM_LOG(pm->log, OSM_LOG_VERBOSE,
			"Failed to find previous data count "
			"reading for %s (0x%" PRIx64 ") port %u\n",
			mon_node->name, mon_node->guid, port);
		return;
	}

	if (dc->xmit_data < prev_dc.xmit_data ||
	    dc->rcv_data < prev_dc.rcv_data ||
	    dc->xmit_pkts < prev_dc.xmit_pkts ||
	    dc->rcv_pkts < prev_dc.rcv_pkts) {
		OSM_LOG(pm->log, OSM_LOG_ERROR,
			"PerfMgr: ERR 540B: Detected an out of band data counter "
			"clear on node %s (0x%" PRIx64 ") port %u\n",
			mon_node->name, mon_node->guid, port);
		perfmgr_db_clear_prev_dc(pm->db, mon_node->guid, port);
	}
}

/**********************************************************************
 * Return 1 if the value is "close" to overflowing
 **********************************************************************/
static int counter_overflow_4(uint8_t val)
{
	return (val >= 10);
}

static int counter_overflow_8(uint8_t val)
{
	return (val >= (UINT8_MAX - (UINT8_MAX / 4)));
}

static int counter_overflow_16(ib_net16_t val)
{
	return (cl_ntoh16(val) >= (UINT16_MAX - (UINT16_MAX / 4)));
}

static int counter_overflow_32(ib_net32_t val)
{
	return (cl_ntoh32(val) >= (UINT32_MAX - (UINT32_MAX / 4)));
}

/**********************************************************************
 * Check if the port counters have overflowed and if so issue a clear
 * MAD to the port.
 **********************************************************************/
static void perfmgr_check_overflow(osm_perfmgr_t * pm,
				   monitored_node_t * mon_node, int16_t pkey_ix,
				   uint8_t port, ib_port_counters_t * pc)
{
	osm_madw_context_t mad_context;
	ib_api_status_t status;
	ib_net32_t remote_qp;

	OSM_LOG_ENTER(pm->log);

	if (counter_overflow_16(pc->symbol_err_cnt) ||
	    counter_overflow_8(pc->link_err_recover) ||
	    counter_overflow_8(pc->link_downed) ||
	    counter_overflow_16(pc->rcv_err) ||
	    counter_overflow_16(pc->rcv_rem_phys_err) ||
	    counter_overflow_16(pc->rcv_switch_relay_err) ||
	    counter_overflow_16(pc->xmit_discards) ||
	    counter_overflow_8(pc->xmit_constraint_err) ||
	    counter_overflow_8(pc->rcv_constraint_err) ||
	    counter_overflow_4(PC_LINK_INT(pc->link_int_buffer_overrun)) ||
	    counter_overflow_4(PC_BUF_OVERRUN(pc->link_int_buffer_overrun)) ||
	    counter_overflow_16(pc->vl15_dropped) ||
	    counter_overflow_32(pc->xmit_data) ||
	    counter_overflow_32(pc->rcv_data) ||
	    counter_overflow_32(pc->xmit_pkts) ||
	    counter_overflow_32(pc->rcv_pkts)) {
		osm_node_t *p_node = NULL;
		ib_net16_t lid = 0;

		if (!mon_node->port[port].valid)
			goto Exit;

		osm_log_v2(pm->log, OSM_LOG_VERBOSE, FILE_ID,
			   "PerfMgr: Counter overflow: %s (0x%" PRIx64
			   ") port %d; clearing counters\n",
			   mon_node->name, mon_node->guid, port);

		cl_plock_acquire(&pm->osm->lock);
		p_node =
		    osm_get_node_by_guid(pm->subn, cl_hton64(mon_node->guid));
		lid = get_lid(p_node, port, mon_node);
		cl_plock_release(&pm->osm->lock);
		if (lid == 0) {
			OSM_LOG(pm->log, OSM_LOG_ERROR, "PerfMgr: ERR 540C: "
				"Failed to clear counters for %s (0x%"
				PRIx64 ") port %d; failed to get lid\n",
				mon_node->name, mon_node->guid, port);
			goto Exit;
		}

		remote_qp = get_qp(NULL, port);

		mad_context.perfmgr_context.node_guid = mon_node->guid;
		mad_context.perfmgr_context.port = port;
		mad_context.perfmgr_context.mad_method = IB_MAD_METHOD_SET;
		/* clear port counters */
		status = perfmgr_send_pc_mad(pm, lid, remote_qp, pkey_ix,
					     port, IB_MAD_METHOD_SET,
					     &mad_context);
		if (status != IB_SUCCESS)
			OSM_LOG(pm->log, OSM_LOG_ERROR, "PerfMgr: ERR 5411: "
				"Failed to send clear counters MAD for %s (0x%"
				PRIx64 ") port %d\n",
				mon_node->name, mon_node->guid, port);

		perfmgr_db_clear_prev_err(pm->db, mon_node->guid, port);
		perfmgr_db_clear_prev_dc(pm->db, mon_node->guid, port);
	}

Exit:
	OSM_LOG_EXIT(pm->log);
}

/**********************************************************************
 * Check values for logging of errors
 **********************************************************************/
static void perfmgr_log_errors(osm_perfmgr_t * pm,
			       monitored_node_t * mon_node, uint8_t port,
			       perfmgr_db_err_reading_t * reading)
{
	perfmgr_db_err_reading_t prev_read;
	perfmgr_db_err_t err =
	    perfmgr_db_get_prev_err(pm->db, mon_node->guid, port, &prev_read);

	if (err != PERFMGR_EVENT_DB_SUCCESS) {
		OSM_LOG(pm->log, OSM_LOG_VERBOSE, "Failed to find previous "
			"reading for %s (0x%" PRIx64 ") port %u\n",
			mon_node->name, mon_node->guid, port);
		return;
	}

#define LOG_ERR_CNT(errname, errnum, counter_name) \
	if (reading->counter_name > prev_read.counter_name) \
		OSM_LOG(pm->log, OSM_LOG_ERROR, "ERR %s: " \
			"%s : %" PRIu64 " : node " \
			"\"%s\" (NodeGUID: 0x%" PRIx64 ") : port %u\n", \
			errnum, errname, \
			reading->counter_name - prev_read.counter_name, \
			mon_node->name, mon_node->guid, port);

	LOG_ERR_CNT("SymbolErrorCounter",           "5431", symbol_err_cnt);
	LOG_ERR_CNT("LinkErrorRecoveryCounter",     "5432", link_err_recover);
	LOG_ERR_CNT("LinkDownedCounter",            "5433", link_downed);
	LOG_ERR_CNT("PortRcvErrors",                "5434", rcv_err);
	LOG_ERR_CNT("PortRcvRemotePhysicalErrors",  "5435", rcv_rem_phys_err);
	LOG_ERR_CNT("PortRcvSwitchRelayErrors",     "5436", rcv_switch_relay_err);
	LOG_ERR_CNT("PortXmitDiscards",             "5437", xmit_discards);
	LOG_ERR_CNT("PortXmitConstraintErrors",     "5438", xmit_constraint_err);
	LOG_ERR_CNT("PortRcvConstraintErrors",      "5439", rcv_constraint_err);
	LOG_ERR_CNT("LocalLinkIntegrityErrors",     "543A", link_integrity);
	LOG_ERR_CNT("ExcessiveBufferOverrunErrors", "543B", buffer_overrun);
	LOG_ERR_CNT("VL15Dropped",                  "543C", vl15_dropped);
}

static int16_t validate_redir_pkey(osm_perfmgr_t *pm, ib_net16_t pkey)
{
	int16_t pkey_ix = -1;
	osm_port_t *p_port;
	osm_pkey_tbl_t *p_pkey_tbl;
	ib_net16_t *p_orig_pkey;
	uint16_t block;
	uint8_t index;

	OSM_LOG_ENTER(pm->log);

	CL_PLOCK_ACQUIRE(pm->sm->p_lock);
	p_port = osm_get_port_by_guid(pm->subn, pm->port_guid);
	if (!p_port) {
		CL_PLOCK_RELEASE(pm->sm->p_lock);
		OSM_LOG(pm->log, OSM_LOG_ERROR,
			"ERR 541E: No PerfMgr port object\n");
		goto Exit;
	}
	if (p_port->p_physp && osm_physp_is_valid(p_port->p_physp)) {
		p_pkey_tbl = &p_port->p_physp->pkeys;
		if (!p_pkey_tbl) {
			CL_PLOCK_RELEASE(pm->sm->p_lock);
			OSM_LOG(pm->log, OSM_LOG_VERBOSE,
				"No PKey table found for PerfMgr port\n");
			goto Exit;
		}
		p_orig_pkey = cl_map_get(&p_pkey_tbl->keys,
					 ib_pkey_get_base(pkey));
		if (!p_orig_pkey) {
			CL_PLOCK_RELEASE(pm->sm->p_lock);
			OSM_LOG(pm->log, OSM_LOG_VERBOSE,
				"PKey 0x%x not found for PerfMgr port\n",
				cl_ntoh16(pkey));
			goto Exit;
		}
		if (osm_pkey_tbl_get_block_and_idx(p_pkey_tbl, p_orig_pkey,
						   &block, &index) == IB_SUCCESS) {
			CL_PLOCK_RELEASE(pm->sm->p_lock);
			pkey_ix = block * IB_NUM_PKEY_ELEMENTS_IN_BLOCK + index;
		} else {
			CL_PLOCK_RELEASE(pm->sm->p_lock);
			OSM_LOG(pm->log, OSM_LOG_ERROR,
				"ERR 541F: Failed to obtain P_Key 0x%04x "
				"block and index for PerfMgr port\n",
				cl_ntoh16(pkey));
		}
	} else {
		CL_PLOCK_RELEASE(pm->sm->p_lock);
		OSM_LOG(pm->log, OSM_LOG_ERROR,
			"ERR 5420: Local PerfMgt port physp invalid\n");
	}

Exit:
	OSM_LOG_EXIT(pm->log);
	return pkey_ix;
}

/**********************************************************************
 * The dispatcher uses a thread pool which will call this function when
 * there is a thread available to process the mad received on the wire.
 **********************************************************************/
static void pc_recv_process(void *context, void *data)
{
	osm_perfmgr_t *pm = context;
	osm_madw_t *p_madw = data;
	osm_madw_context_t *mad_context = &p_madw->context;
	ib_port_counters_t *wire_read =
	    (ib_port_counters_t *) & osm_madw_get_perfmgt_mad_ptr(p_madw)->data;
	ib_mad_t *p_mad = osm_madw_get_mad_ptr(p_madw);
	uint64_t node_guid = mad_context->perfmgr_context.node_guid;
	uint8_t port = mad_context->perfmgr_context.port;
	perfmgr_db_err_reading_t err_reading;
	perfmgr_db_data_cnt_reading_t data_reading;
	cl_map_item_t *p_node;
	monitored_node_t *p_mon_node;
	int16_t pkey_ix = 0;
	boolean_t valid = TRUE;

	OSM_LOG_ENTER(pm->log);

	/*
	 * get the monitored node struct to have the printable name
	 * for log messages
	 */
	if ((p_node = cl_qmap_get(&pm->monitored_map, node_guid)) ==
	    cl_qmap_end(&pm->monitored_map)) {
		OSM_LOG(pm->log, OSM_LOG_ERROR, "ERR 5412: GUID 0x%016"
			PRIx64 " not found in monitored map\n", node_guid);
		goto Exit;
	}
	p_mon_node = (monitored_node_t *) p_node;

	OSM_LOG(pm->log, OSM_LOG_VERBOSE,
		"Processing received MAD status 0x%x context 0x%"
		PRIx64 " port %u\n", p_mad->status, node_guid, port);

	CL_ASSERT(p_mad->attr_id == IB_MAD_ATTR_PORT_CNTRS ||
		  p_mad->attr_id == IB_MAD_ATTR_CLASS_PORT_INFO);

	/* Response could also be redirection (IBM eHCA PMA does this) */
	if (p_mad->status & IB_MAD_STATUS_REDIRECT &&
	    p_mad->attr_id == IB_MAD_ATTR_CLASS_PORT_INFO) {
		char gid_str[INET6_ADDRSTRLEN];
		ib_class_port_info_t *cpi =
		    (ib_class_port_info_t *) &
		    (osm_madw_get_perfmgt_mad_ptr(p_madw)->data);
		ib_api_status_t status;

		OSM_LOG(pm->log, OSM_LOG_VERBOSE,
			"Redirection to LID %u GID %s QP 0x%x received\n",
			cl_ntoh16(cpi->redir_lid),
			inet_ntop(AF_INET6, cpi->redir_gid.raw, gid_str,
				  sizeof gid_str), cl_ntoh32(cpi->redir_qp));

		if (!pm->subn->opt.perfmgr_redir) {
			OSM_LOG(pm->log, OSM_LOG_VERBOSE,
				"Redirection requested but disabled\n");
			valid = FALSE;
		}

		/* valid redirection ? */
		if (cpi->redir_lid == 0) {
			if (!ib_gid_is_notzero(&cpi->redir_gid)) {
				OSM_LOG(pm->log, OSM_LOG_VERBOSE,
					"Invalid redirection "
					"(both redirect LID and GID are zero)\n");
				valid = FALSE;
			}
		}
		if (cpi->redir_qp == 0) {
			OSM_LOG(pm->log, OSM_LOG_VERBOSE, "Invalid RedirectQP\n");
			valid = FALSE;
		}
		if (cpi->redir_pkey == 0) {
			OSM_LOG(pm->log, OSM_LOG_VERBOSE, "Invalid RedirectP_Key\n");
			valid = FALSE;
		}
		if (cpi->redir_qkey != IB_QP1_WELL_KNOWN_Q_KEY) {
			OSM_LOG(pm->log, OSM_LOG_VERBOSE, "Invalid RedirectQ_Key\n");
			valid = FALSE;
		}

		pkey_ix = validate_redir_pkey(pm, cpi->redir_pkey);
		if (pkey_ix == -1) {
			OSM_LOG(pm->log, OSM_LOG_VERBOSE,
				"Index for Pkey 0x%x not found\n",
				cl_ntoh16(cpi->redir_pkey));
			valid = FALSE;
		}

		if (cpi->redir_lid == 0) {
			/* GID redirection: get PathRecord information */
			OSM_LOG(pm->log, OSM_LOG_VERBOSE,
				"GID redirection not currently supported\n");
			goto Exit;
		}

		/* LID redirection support (easier than GID redirection) */
		cl_plock_acquire(&pm->osm->lock);
		/* Now, validate port number */
		if (port >= p_mon_node->num_ports) {
			cl_plock_release(&pm->osm->lock);
			OSM_LOG(pm->log, OSM_LOG_ERROR, "ERR 5413: "
				"Invalid port num %d for GUID 0x%016"
				PRIx64 " num ports %d\n", port, node_guid,
				p_mon_node->num_ports);
			goto Exit;
		}
		p_mon_node->port[port].redirection = TRUE;
		p_mon_node->port[port].valid = valid;
		memcpy(&p_mon_node->port[port].gid, &cpi->redir_gid,
		       sizeof(ib_gid_t));
		p_mon_node->port[port].lid = cpi->redir_lid;
		p_mon_node->port[port].qp = cpi->redir_qp;
		p_mon_node->port[port].pkey = cpi->redir_pkey;
		if (pkey_ix != -1)
			p_mon_node->port[port].pkey_ix = pkey_ix;
		cl_plock_release(&pm->osm->lock);

		if (!valid)
			goto Exit;

		/* Finally, reissue the query to the redirected location */
		status = perfmgr_send_pc_mad(pm, cpi->redir_lid, cpi->redir_qp,
					     pkey_ix, port,
					     mad_context->perfmgr_context.
					     mad_method, mad_context);
		if (status != IB_SUCCESS)
			OSM_LOG(pm->log, OSM_LOG_ERROR, "ERR 5414: "
				"Failed to send redirected MAD with method 0x%x for node 0x%"
				PRIx64 " port %d\n",
				mad_context->perfmgr_context.mad_method,
				node_guid, port);
		goto Exit;
	}

	perfmgr_db_fill_err_read(wire_read, &err_reading);
	/* FIXME separate query for extended counters if they are supported
	 * on the port.
	 */
	perfmgr_db_fill_data_cnt_read_pc(wire_read, &data_reading);

	/* detect an out of band clear on the port */
	if (mad_context->perfmgr_context.mad_method != IB_MAD_METHOD_SET)
		perfmgr_check_oob_clear(pm, p_mon_node, port, &err_reading,
					&data_reading);

	if (mad_context->perfmgr_context.mad_method == IB_MAD_METHOD_GET) {
		/* log errors from this reading */
		if (pm->subn->opt.perfmgr_log_errors)
			perfmgr_log_errors(pm, p_mon_node, port, &err_reading);

		perfmgr_db_add_err_reading(pm->db, node_guid, port,
					   &err_reading);
		perfmgr_db_add_dc_reading(pm->db, node_guid, port,
					  &data_reading);
	} else {
		perfmgr_db_clear_prev_err(pm->db, node_guid, port);
		perfmgr_db_clear_prev_dc(pm->db, node_guid, port);
	}

	perfmgr_check_overflow(pm, p_mon_node, pkey_ix, port, wire_read);

#ifdef ENABLE_OSM_PERF_MGR_PROFILE
	do {
		struct timeval proc_time;
		gettimeofday(&proc_time, NULL);
		diff_time(&p_madw->context.perfmgr_context.query_start,
			  &proc_time, &proc_time);
		update_mad_stats(&proc_time);
	} while (0);
#endif

Exit:
	osm_mad_pool_put(pm->mad_pool, p_madw);

	OSM_LOG_EXIT(pm->log);
}

/**********************************************************************
 * Initialize the PerfMgr object
 **********************************************************************/
ib_api_status_t osm_perfmgr_init(osm_perfmgr_t * pm, osm_opensm_t * osm,
				 const osm_subn_opt_t * p_opt)
{
	ib_api_status_t status;

	OSM_LOG_ENTER(&osm->log);

	OSM_LOG(&osm->log, OSM_LOG_VERBOSE, "Initializing PerfMgr\n");

	memset(pm, 0, sizeof(*pm));

	cl_event_construct(&pm->sig_sweep);
	cl_event_init(&pm->sig_sweep, FALSE);
	pm->subn = &osm->subn;
	pm->sm = &osm->sm;
	pm->log = &osm->log;
	pm->mad_pool = &osm->mad_pool;
	pm->vendor = osm->p_vendor;
	pm->trans_id = PERFMGR_INITIAL_TID_VALUE;
	pm->state =
	    p_opt->perfmgr ? PERFMGR_STATE_ENABLED : PERFMGR_STATE_DISABLE;
	pm->sweep_time_s = p_opt->perfmgr_sweep_time_s;
	pm->max_outstanding_queries = p_opt->perfmgr_max_outstanding_queries;
	pm->ignore_cas = p_opt->perfmgr_ignore_cas;
	pm->osm = osm;
	pm->local_port = -1;

	status = cl_timer_init(&pm->sweep_timer, perfmgr_sweep, pm);
	if (status != IB_SUCCESS)
		goto Exit;

	status = IB_INSUFFICIENT_RESOURCES;
	pm->db = perfmgr_db_construct(pm);
	if (!pm->db) {
		pm->state = PERFMGR_STATE_NO_DB;
		goto Exit;
	}

	pm->pc_disp_h = cl_disp_register(&osm->disp, OSM_MSG_MAD_PORT_COUNTERS,
					 pc_recv_process, pm);
	if (pm->pc_disp_h == CL_DISP_INVALID_HANDLE) {
		perfmgr_db_destroy(pm->db);
		goto Exit;
	}

	init_monitored_nodes(pm);

	if (pm->state == PERFMGR_STATE_ENABLED)
		cl_timer_start(&pm->sweep_timer, pm->sweep_time_s * 1000);

	pm->rm_nodes = p_opt->perfmgr_rm_nodes;
	status = IB_SUCCESS;
Exit:
	OSM_LOG_EXIT(pm->log);
	return status;
}

/**********************************************************************
 * Clear the counters from the db
 **********************************************************************/
void osm_perfmgr_clear_counters(osm_perfmgr_t * pm)
{
	/**
	 * FIXME todo issue clear on the fabric?
	 */
	perfmgr_db_clear_counters(pm->db);
	osm_log_v2(pm->log, OSM_LOG_INFO, FILE_ID, "PerfMgr counters cleared\n");
}

/*******************************************************************
 * Dump the DB information to the file specified
 *******************************************************************/
void osm_perfmgr_dump_counters(osm_perfmgr_t * pm, perfmgr_db_dump_t dump_type)
{
	char path[256];
	char *file_name;
	if (pm->subn->opt.event_db_dump_file)
		file_name = pm->subn->opt.event_db_dump_file;
	else {
		snprintf(path, sizeof(path), "%s/%s",
			 pm->subn->opt.dump_files_dir,
			 OSM_PERFMGR_DEFAULT_DUMP_FILE);
		file_name = path;
	}
	if (perfmgr_db_dump(pm->db, file_name, dump_type) != 0)
		OSM_LOG(pm->log, OSM_LOG_ERROR, "Failed to dump file %s : %s",
			file_name, strerror(errno));
}

/*******************************************************************
 * Print the DB information to the fp specified
 *******************************************************************/
void osm_perfmgr_print_counters(osm_perfmgr_t * pm, char *nodename, FILE * fp,
				char *port, int err_only)
{
	if (nodename) {
		char *end = NULL;
		uint64_t guid = strtoull(nodename, &end, 0);
		if (nodename + strlen(nodename) != end)
			perfmgr_db_print_by_name(pm->db, nodename, fp, port,
						 err_only);
		else
			perfmgr_db_print_by_guid(pm->db, guid, fp, port,
						 err_only);
	} else
		perfmgr_db_print_all(pm->db, fp, err_only);
}
#endif				/* ENABLE_OSM_PERF_MGR */
