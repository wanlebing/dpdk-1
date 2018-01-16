/*   SPDX-License-Identifier:        BSD-3-Clause
 *   Copyright 2017 NXP
 */

#include <assert.h>
#include <stdio.h>
#include <stdbool.h>
#include <errno.h>
#include <stdint.h>
#include <string.h>
#include <sys/epoll.h>

#include <rte_atomic.h>
#include <rte_byteorder.h>
#include <rte_common.h>
#include <rte_debug.h>
#include <rte_dev.h>
#include <rte_eal.h>
#include <rte_lcore.h>
#include <rte_log.h>
#include <rte_malloc.h>
#include <rte_memcpy.h>
#include <rte_memory.h>
#include <rte_memzone.h>
#include <rte_pci.h>
#include <rte_eventdev.h>
#include <rte_eventdev_pmd_vdev.h>
#include <rte_ethdev.h>
#include <rte_event_eth_rx_adapter.h>
#include <rte_dpaa_bus.h>
#include <rte_dpaa_logs.h>
#include <rte_cycles_64.h>

#include <dpaa_ethdev.h>
#include "dpaa_eventdev.h"
#include <dpaa_mempool.h>

/*
 * Clarifications
 * Evendev = Virtual Instance for SoC
 * Eventport = Portal Instance
 * Eventqueue = Channel Instance
 * 1 Eventdev can have N Eventqueue
 */

static int
dpaa_event_dequeue_timeout_ticks(struct rte_eventdev *dev, uint64_t ns,
				 uint64_t *timeout_ticks)
{
	uint64_t cycles_per_second;

	EVENTDEV_DRV_FUNC_TRACE();

	RTE_SET_USED(dev);

	cycles_per_second = rte_get_timer_hz();
	*timeout_ticks = ns * (cycles_per_second / NS_PER_S);

	return 0;
}

static void
dpaa_event_dev_info_get(struct rte_eventdev *dev,
			struct rte_event_dev_info *dev_info)
{
	EVENTDEV_DRV_FUNC_TRACE();

	RTE_SET_USED(dev);
	dev_info->driver_name = "event_dpaa";
	dev_info->min_dequeue_timeout_ns =
		DPAA_EVENT_MIN_DEQUEUE_TIMEOUT;
	dev_info->max_dequeue_timeout_ns =
		DPAA_EVENT_MAX_DEQUEUE_TIMEOUT;
	dev_info->dequeue_timeout_ns =
		DPAA_EVENT_MIN_DEQUEUE_TIMEOUT;
	dev_info->max_event_queues =
		DPAA_EVENT_MAX_QUEUES;
	dev_info->max_event_queue_flows =
		DPAA_EVENT_MAX_QUEUE_FLOWS;
	dev_info->max_event_queue_priority_levels =
		DPAA_EVENT_MAX_QUEUE_PRIORITY_LEVELS;
	dev_info->max_event_priority_levels =
		DPAA_EVENT_MAX_EVENT_PRIORITY_LEVELS;
	dev_info->max_event_ports =
		DPAA_EVENT_MAX_EVENT_PORT;
	dev_info->max_event_port_dequeue_depth =
		DPAA_EVENT_MAX_PORT_DEQUEUE_DEPTH;
	dev_info->max_event_port_enqueue_depth =
		DPAA_EVENT_MAX_PORT_ENQUEUE_DEPTH;
	/*
	 * TODO: Need to find out that how to fetch this info
	 * from kernel or somewhere else.
	 */
	dev_info->max_num_events =
		DPAA_EVENT_MAX_NUM_EVENTS;
	dev_info->event_dev_cap =
		RTE_EVENT_DEV_CAP_DISTRIBUTED_SCHED |
		RTE_EVENT_DEV_CAP_BURST_MODE |
		RTE_EVENT_DEV_CAP_MULTIPLE_QUEUE_PORT |
		RTE_EVENT_DEV_CAP_NONSEQ_MODE;
}

static int
dpaa_event_dev_configure(const struct rte_eventdev *dev)
{
	struct dpaa_eventdev *priv = dev->data->dev_private;
	struct rte_event_dev_config *conf = &dev->data->dev_conf;
	int ret, i;
	uint32_t *ch_id;

	EVENTDEV_DRV_FUNC_TRACE();

	priv->dequeue_timeout_ns = conf->dequeue_timeout_ns;
	priv->nb_events_limit = conf->nb_events_limit;
	priv->nb_event_queues = conf->nb_event_queues;
	priv->nb_event_ports = conf->nb_event_ports;
	priv->nb_event_queue_flows = conf->nb_event_queue_flows;
	priv->nb_event_port_dequeue_depth = conf->nb_event_port_dequeue_depth;
	priv->nb_event_port_enqueue_depth = conf->nb_event_port_enqueue_depth;
	priv->event_dev_cfg = conf->event_dev_cfg;

	/* Check dequeue timeout method is per dequeue or global */
	if (priv->event_dev_cfg & RTE_EVENT_DEV_CFG_PER_DEQUEUE_TIMEOUT) {
		/*
		 * Use timeout value as given in dequeue operation.
		 * So invalidating this timetout value.
		 */
		priv->dequeue_timeout_ns = 0;
	}

	ch_id = rte_malloc("dpaa-channels",
			  sizeof(uint32_t) * priv->nb_event_queues,
			  RTE_CACHE_LINE_SIZE);
	if (ch_id == NULL) {
		EVENTDEV_DRV_ERR("Fail to allocate memory for dpaa channels\n");
		return -ENOMEM;
	}
	/* Create requested event queues within the given event device */
	ret = qman_alloc_pool_range(ch_id, priv->nb_event_queues, 1, 0);
	if (ret < 0) {
		EVENTDEV_DRV_ERR("Failed to create internal channel\n");
		rte_free(ch_id);
		return ret;
	}
	for (i = 0; i < priv->nb_event_queues; i++)
		priv->evq_info[i].ch_id = (u16)ch_id[i];

	/* Lets prepare event ports */
	memset(&priv->ports[0], 0,
	      sizeof(struct dpaa_port) * priv->nb_event_ports);
	if (priv->event_dev_cfg & RTE_EVENT_DEV_CFG_PER_DEQUEUE_TIMEOUT) {
		for (i = 0; i < priv->nb_event_ports; i++) {
			priv->ports[i].timeout =
				DPAA_EVENT_PORT_DEQUEUE_TIMEOUT_INVALID;
		}
	} else if (priv->dequeue_timeout_ns == 0) {
		for (i = 0; i < priv->nb_event_ports; i++) {
			dpaa_event_dequeue_timeout_ticks(NULL,
				DPAA_EVENT_PORT_DEQUEUE_TIMEOUT_NS,
				&priv->ports[i].timeout);
		}
	} else {
		for (i = 0; i < priv->nb_event_ports; i++) {
			dpaa_event_dequeue_timeout_ticks(NULL,
				priv->dequeue_timeout_ns,
				&priv->ports[i].timeout);
		}
	}
	/*
	 * TODO: Currently portals are affined with threads. Maximum threads
	 * can be created equals to number of lcore.
	 */
	rte_free(ch_id);
	EVENTDEV_DRV_LOG("Configured eventdev devid=%d", dev->data->dev_id);

	return 0;
}

static int
dpaa_event_dev_start(struct rte_eventdev *dev)
{
	EVENTDEV_DRV_FUNC_TRACE();
	RTE_SET_USED(dev);

	return 0;
}

static void
dpaa_event_dev_stop(struct rte_eventdev *dev)
{
	EVENTDEV_DRV_FUNC_TRACE();
	RTE_SET_USED(dev);
}

static int
dpaa_event_dev_close(struct rte_eventdev *dev)
{
	EVENTDEV_DRV_FUNC_TRACE();
	RTE_SET_USED(dev);

	return 0;
}

static void
dpaa_event_queue_def_conf(struct rte_eventdev *dev, uint8_t queue_id,
			  struct rte_event_queue_conf *queue_conf)
{
	EVENTDEV_DRV_FUNC_TRACE();

	RTE_SET_USED(dev);
	RTE_SET_USED(queue_id);

	memset(queue_conf, 0, sizeof(struct rte_event_queue_conf));
	queue_conf->schedule_type = RTE_SCHED_TYPE_PARALLEL;
	queue_conf->priority = RTE_EVENT_DEV_PRIORITY_HIGHEST;
}

static int
dpaa_event_queue_setup(struct rte_eventdev *dev, uint8_t queue_id,
		       const struct rte_event_queue_conf *queue_conf)
{
	struct dpaa_eventdev *priv = dev->data->dev_private;
	struct dpaa_eventq *evq_info = &priv->evq_info[queue_id];

	EVENTDEV_DRV_FUNC_TRACE();

	switch (queue_conf->schedule_type) {
	case RTE_SCHED_TYPE_PARALLEL:
	case RTE_SCHED_TYPE_ATOMIC:
		break;
	case RTE_SCHED_TYPE_ORDERED:
		EVENTDEV_DRV_ERR("Schedule type is not supported.");
		return -1;
	}
	evq_info->event_queue_cfg = queue_conf->event_queue_cfg;
	evq_info->event_queue_id = queue_id;

	return 0;
}

static void
dpaa_event_queue_release(struct rte_eventdev *dev, uint8_t queue_id)
{
	EVENTDEV_DRV_FUNC_TRACE();

	RTE_SET_USED(dev);
	RTE_SET_USED(queue_id);
}

static void
dpaa_event_port_default_conf_get(struct rte_eventdev *dev, uint8_t port_id,
				 struct rte_event_port_conf *port_conf)
{
	EVENTDEV_DRV_FUNC_TRACE();

	RTE_SET_USED(dev);
	RTE_SET_USED(port_id);

	port_conf->new_event_threshold = DPAA_EVENT_MAX_NUM_EVENTS;
	port_conf->dequeue_depth = DPAA_EVENT_MAX_PORT_DEQUEUE_DEPTH;
	port_conf->enqueue_depth = DPAA_EVENT_MAX_PORT_ENQUEUE_DEPTH;
}

static int
dpaa_event_port_setup(struct rte_eventdev *dev, uint8_t port_id,
		      const struct rte_event_port_conf *port_conf)
{
	struct dpaa_eventdev *eventdev = dev->data->dev_private;

	EVENTDEV_DRV_FUNC_TRACE();

	RTE_SET_USED(port_conf);
	dev->data->ports[port_id] = &eventdev->ports[port_id];

	return 0;
}

static void
dpaa_event_port_release(void *port)
{
	EVENTDEV_DRV_FUNC_TRACE();

	RTE_SET_USED(port);
}

static int
dpaa_event_port_link(struct rte_eventdev *dev, void *port,
		     const uint8_t queues[], const uint8_t priorities[],
		     uint16_t nb_links)
{
	struct dpaa_eventdev *priv = dev->data->dev_private;
	struct dpaa_port *event_port = (struct dpaa_port *)port;
	struct dpaa_eventq *event_queue;
	uint8_t eventq_id;
	int i;

	RTE_SET_USED(dev);
	RTE_SET_USED(priorities);

	/* First check that input configuration are valid */
	for (i = 0; i < nb_links; i++) {
		eventq_id = queues[i];
		event_queue = &priv->evq_info[eventq_id];
		if ((event_queue->event_queue_cfg
			& RTE_EVENT_QUEUE_CFG_SINGLE_LINK)
			&& (event_queue->event_port)) {
			return -EINVAL;
		}
	}

	for (i = 0; i < nb_links; i++) {
		eventq_id = queues[i];
		event_queue = &priv->evq_info[eventq_id];
		event_port->evq_info[i].event_queue_id = eventq_id;
		event_port->evq_info[i].ch_id = event_queue->ch_id;
		event_queue->event_port = port;
	}

	event_port->num_linked_evq = event_port->num_linked_evq + i;

	return (int)i;
}

static int
dpaa_event_port_unlink(struct rte_eventdev *dev, void *port,
		       uint8_t queues[], uint16_t nb_links)
{
	int i;
	uint8_t eventq_id;
	struct dpaa_eventq *event_queue;
	struct dpaa_eventdev *priv = dev->data->dev_private;
	struct dpaa_port *event_port = (struct dpaa_port *)port;

	if (!event_port->num_linked_evq)
		return nb_links;

	for (i = 0; i < nb_links; i++) {
		eventq_id = queues[i];
		event_port->evq_info[eventq_id].event_queue_id = -1;
		event_port->evq_info[eventq_id].ch_id = 0;
		event_queue = &priv->evq_info[eventq_id];
		event_queue->event_port = NULL;
	}

	event_port->num_linked_evq = event_port->num_linked_evq - i;

	return (int)i;
}

static const struct rte_eventdev_ops dpaa_eventdev_ops = {
	.dev_infos_get    = dpaa_event_dev_info_get,
	.dev_configure    = dpaa_event_dev_configure,
	.dev_start        = dpaa_event_dev_start,
	.dev_stop         = dpaa_event_dev_stop,
	.dev_close        = dpaa_event_dev_close,
	.queue_def_conf   = dpaa_event_queue_def_conf,
	.queue_setup      = dpaa_event_queue_setup,
	.queue_release    = dpaa_event_queue_release,
	.port_def_conf    = dpaa_event_port_default_conf_get,
	.port_setup       = dpaa_event_port_setup,
	.port_release       = dpaa_event_port_release,
	.port_link        = dpaa_event_port_link,
	.port_unlink      = dpaa_event_port_unlink,
	.timeout_ticks    = dpaa_event_dequeue_timeout_ticks,
};

static int
dpaa_event_dev_create(const char *name)
{
	struct rte_eventdev *eventdev;
	struct dpaa_eventdev *priv;

	eventdev = rte_event_pmd_vdev_init(name,
					   sizeof(struct dpaa_eventdev),
					   rte_socket_id());
	if (eventdev == NULL) {
		EVENTDEV_DRV_ERR("Failed to create eventdev vdev %s", name);
		goto fail;
	}

	eventdev->dev_ops       = &dpaa_eventdev_ops;

	/* For secondary processes, the primary has done all the work */
	if (rte_eal_process_type() != RTE_PROC_PRIMARY)
		return 0;

	priv = eventdev->data->dev_private;
	priv->max_event_queues = DPAA_EVENT_MAX_QUEUES;

	return 0;
fail:
	return -EFAULT;
}

static int
dpaa_event_dev_probe(struct rte_vdev_device *vdev)
{
	const char *name;

	name = rte_vdev_device_name(vdev);
	EVENTDEV_DRV_LOG("Initializing %s", name);

	return dpaa_event_dev_create(name);
}

static int
dpaa_event_dev_remove(struct rte_vdev_device *vdev)
{
	const char *name;

	name = rte_vdev_device_name(vdev);
	EVENTDEV_DRV_LOG("Closing %s", name);

	return rte_event_pmd_vdev_uninit(name);
}

static struct rte_vdev_driver vdev_eventdev_dpaa_pmd = {
	.probe = dpaa_event_dev_probe,
	.remove = dpaa_event_dev_remove
};

RTE_PMD_REGISTER_VDEV(EVENTDEV_NAME_DPAA_PMD, vdev_eventdev_dpaa_pmd);