/*
 * TDLS Auto mode
 * Copyright(c) 2013 - 2015 Intel Mobile Communications GmbH.
 * Copyright(c) 2011 - 2015 Intel Corporation. All rights reserved.
 *
 * Contact Information:
 * Intel Linux Wireless <ilw@linux.intel.com>
 * Intel Corporation, 5200 N.E. Elam Young Parkway, Hillsboro, OR 97124-6497
 *
 * This software may be distributed under the terms of the BSD license.
 * See README for more details.
 */

#ifndef _TDLS_AUTO_MODE_H_
#define _TDLS_AUTO_MODE_H_

struct tdls_auto_mode_ctx;

#define tdls_auto_excessive(fmt, ...) \
	wpa_printf(MSG_EXCESSIVE, fmt, ## __VA_ARGS__)
#define tdls_auto_debug(fmt, ...) \
	wpa_printf(MSG_DEBUG, fmt, ## __VA_ARGS__)
#define tdls_auto_info(fmt, ...) \
	wpa_printf(MSG_INFO, fmt, ## __VA_ARGS__)
#define tdls_auto_err(fmt, ...) \
	wpa_printf(MSG_ERROR, fmt, ## __VA_ARGS__)

struct wpas_tdls_auto_ctx {
	void *ctx; /* pointer to arbitrary upper level context */

	/*
	 * start a TDLS connection to peer. Can return back to this layer
	 * via the tdls_auto_peer_connected callback
	 */
	int (*connect)(void *ctx, const u8 *addr);

	/*
	 * teardown a TDLS connection with peer. Can return back to this
	 * layer via the tdls_auto_peer_disconnected callback.
	 */
	void (*disconnect)(void *ctx, const u8 *addr);

	/* send TDLS discovery request to peer */
	void (*send_discovery)(void *ctx, const u8 *addr);

	/* get RSSI of connected TDLS peer. returns -102 on failure */
	int (*get_rssi)(void *ctx, const u8 *addr);

	/* add/del peer from peer cache for measuring Rx/Tx statistics */
	int (*monitor_traffic)(void *ctx, const u8 *addr, int add);

	/* get Rx/Tx statistics for connected TDLS peer */
	int (*get_sta_bytes)(void *ctx, const u8 *addr, u32 *tx_bytes,
			     u32 *rx_bytes);

	/* see documentation in correspnding config.c variables */
	int tdls_auto_rssi_connect_threshold;
	unsigned int tdls_auto_data_connect_threshold;
	unsigned int tdls_auto_fast_connect_period;
	unsigned int tdls_auto_slow_connect_period;
	unsigned int tdls_auto_data_teardown_threshold;
	unsigned int tdls_auto_data_teardown_period;
	int tdls_auto_rssi_teardown_threshold;
	unsigned int tdls_auto_rssi_teardown_period;
	unsigned int tdls_auto_rssi_teardown_count;
	unsigned int tdls_auto_max_connected_peers;
};

/* initialize TDLS auto-mode layer */
struct tdls_auto_mode_ctx *tdls_auto_init(struct wpas_tdls_auto_ctx *ctx);

/* deinit TDLS auto-mode layer and free memory */
void tdls_auto_deinit(struct tdls_auto_mode_ctx *ctx);

/* Callbacks called on TDLS peer connect/disconnect. Called for any TDLS
 * connection, incoming or outgoing.
 */
void tdls_auto_peer_connected(struct tdls_auto_mode_ctx *ctx, const u8 *addr);
void tdls_auto_peer_disconnected(struct tdls_auto_mode_ctx *ctx,
				 const u8 *addr);

/*
 * remove all TDLS peers from tracking and optionally teardown existing
 * TDLS connections
 */
void tdls_auto_remove_peers(struct tdls_auto_mode_ctx *ctx,
			    int kill_active_links);

/* add/remove a potential TDLS peer to the peer-cache */
int tdls_auto_mode_start(struct tdls_auto_mode_ctx *ctx, const u8 *addr);
void tdls_auto_mode_stop(struct tdls_auto_mode_ctx *ctx, const u8 *addr);

/* TDLS discovery response from a given peer, along with RSSI */
void tdls_auto_discovery_response(struct tdls_auto_mode_ctx *ctx,
				  const u8 *addr, int rssi);

#endif /* TDLS_AUTO_MODE_H_ */
