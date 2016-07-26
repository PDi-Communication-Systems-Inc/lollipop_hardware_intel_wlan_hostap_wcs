/*
 * TDLS automatic connection management (auto-mode)
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

#include "includes.h"
#include "common.h"
#include "eloop.h"
#include "os.h"

#include "tdls_auto_mode.h"

/**
 * TDLS auto-mode is a heuristics based approach to initiating and terminating
 * TDLS connections. It uses RSSI and traffic based thresholds to determine
 * when it is worthwhile for the HW to maintain a TDLS connection with a given
 * peer.
 * When an external applications adds a peer as a candidate TDLS-peer, the
 * auto-mode module sends it discovery requests and records the RSSI values
 * of discovery response packets.
 * If the RSSI is above a given threshold, a TDLS connection is setup.
 * While a peer is connected, its data-RSSI and traffic are continuously
 * monitored. If either falls below threshold, the TDLS connection is torn down.
 * The peer again becomes a candidate peers and discovery requests are
 * periodically sent to it.
 * When a peer is added or disconnected, a fast connection mechanism is
 * activated to allow for fast initial connection and reconnection in the event
 * of a spurious disconnect. If the peer doesn't respond within this time, a
 * slow connection cycle is used. This cycle is intended to capture peer RSSI
 * and traffic changes over time.
 */

/*
 * Initial fast connection attempts. If these fail a slow connect cycle is
 * used.
 */
#define TDLS_AUTO_MAX_FAST_CONN_ATTEMPTS 20

/*
 * The minimal time between data rate samples.
 */
#define TDLS_AUTO_MIN_SAMPLE_TIME_DIFF_MSEC 100

struct tdls_auto_peer {
	struct dl_list list;

	u8 addr[ETH_ALEN]; /* other end MAC address */

	/* is connected now as a TDLS sta */
	int connected;

	/* latest RSSI */
	int rssi;

	/* number of consecutive polls with bad RSSI */
	unsigned int low_rssi_vals;

	/*
	 * number of times we've tried to connect to the peer using the fast
	 * connection schedule.
	 */
	int fast_attempts;

	/*
	 * data stats - in + out traffic (in bps). Only valid while the peer
	 * is conencted
	 */
	u32 data_rate;

	/* data stats - last query time and tx/rx data */
	struct os_reltime last_query_time;
	u32 last_rx_bytes;
	u32 last_tx_bytes;

	/* is peer only incoming and _not_ part of auto-mode */
	int incoming_peer;
};


struct tdls_auto_mode_ctx {
	struct dl_list peers;

	unsigned int peer_count;
	unsigned int conn_peer_count;

	struct wpas_tdls_auto_ctx *extn;
};


static struct tdls_auto_peer *
tdls_auto_find_peer(struct tdls_auto_mode_ctx *ctx, const u8 *addr)
{
	struct tdls_auto_peer *peer;

	dl_list_for_each(peer, &ctx->peers, struct tdls_auto_peer, list)
		if (os_memcmp(peer->addr, addr, ETH_ALEN) == 0)
			return peer;

	return NULL;
}


static struct tdls_auto_peer *
tdls_auto_peer_add_to_list(struct tdls_auto_mode_ctx *ctx, const u8 *addr)
{
	struct tdls_auto_peer *peer = os_zalloc(sizeof(*peer));

	if (!peer)
		return NULL;

	os_memcpy(peer->addr, addr, ETH_ALEN);
	dl_list_add(&ctx->peers, &peer->list);

	ctx->peer_count++;
	return peer;
}


static void tdls_auto_set_timer(struct tdls_auto_mode_ctx *ctx,
				unsigned int msecs,
				eloop_timeout_handler handler)
{
	/* remove previous timer set */
	eloop_cancel_timeout(handler, ctx, NULL);
	eloop_register_timeout(0, 1000 * msecs, handler, ctx, NULL);
}


static void tdls_auto_cancel_timer(struct tdls_auto_mode_ctx *ctx,
				   eloop_timeout_handler handler)
{
	eloop_cancel_timeout(handler, ctx, NULL);
}


static void tdls_auto_get_connected_sta_rssi(struct tdls_auto_mode_ctx *ctx,
					     struct tdls_auto_peer *peer)
{
	peer->rssi = ctx->extn->get_rssi(ctx->extn->ctx, peer->addr);
	tdls_auto_excessive("TDLSAUTO: last RSSI of connected peer " MACSTR
			    ": %d", MAC2STR(peer->addr), peer->rssi);
}


static void
tdls_auto_get_peer_data_rate(struct tdls_auto_mode_ctx *ctx,
			     struct tdls_auto_peer *peer)
{
	u32 tx_bytes, rx_bytes, delta_bits;
	struct os_reltime now, diff;
	int delta_msec;

	os_get_reltime(&now);
	os_reltime_sub(&now, &peer->last_query_time, &diff);
	delta_msec = diff.sec * 1000 + diff.usec / 1000;

	/* Measurements that are not long enough are invalid. */
	if (delta_msec < TDLS_AUTO_MIN_SAMPLE_TIME_DIFF_MSEC) {
		tdls_auto_excessive("TDLSAUTO: " MACSTR " dtime=%u. No sample",
				    MAC2STR(peer->addr), delta_msec);
		return;
	}

	if (ctx->extn->get_sta_bytes(ctx->extn->ctx, peer->addr, &tx_bytes,
				     &rx_bytes)) {
		tdls_auto_err("TDLSAUTO: could not get data stats for " MACSTR,
			      MAC2STR(peer->addr));
		peer->data_rate = 0;
		return;
	}

	delta_bits = (rx_bytes - peer->last_rx_bytes) * 8;
	delta_bits += (tx_bytes - peer->last_tx_bytes) * 8;

	peer->last_rx_bytes = rx_bytes;
	peer->last_tx_bytes = tx_bytes;
	peer->last_query_time = now;
	peer->data_rate = delta_bits / delta_msec * 1000;

	tdls_auto_excessive("TDLSAUTO: " MACSTR " : rate=%u bps, timed=%u tx=%u, rx=%u",
			    MAC2STR(peer->addr), peer->data_rate, delta_msec,
			    tx_bytes, rx_bytes);
}


static void tdls_auto_fast_connect_timeout(void *eloop_ctx, void *timeout_ctx)
{
	struct tdls_auto_mode_ctx *ctx = eloop_ctx;
	struct tdls_auto_peer *peer, *tmp;
	int peer_in_fast_connect = 0;

	dl_list_for_each_safe(peer, tmp, &ctx->peers, struct tdls_auto_peer,
			      list) {
		if (peer->connected)
			continue;

		if (peer->fast_attempts > TDLS_AUTO_MAX_FAST_CONN_ATTEMPTS)
			continue;

		tdls_auto_debug("TDLSAUTO: fast connect to " MACSTR " retry %d",
				MAC2STR(peer->addr), peer->fast_attempts);
		peer->fast_attempts++;
		peer_in_fast_connect = 1;

		/* avoid discovery if peer traffic is not fast enough */
		tdls_auto_get_peer_data_rate(ctx, peer);
		tdls_auto_excessive("TDLSAUTO: data-rate of unconnected peer "
				    MACSTR ": %ld", MAC2STR(peer->addr),
				    (unsigned long)peer->data_rate);
		if (peer->data_rate <
				ctx->extn->tdls_auto_data_connect_threshold)
			continue;

		tdls_auto_excessive("TDLSAUTO: discovering peer " MACSTR,
				    MAC2STR(peer->addr));
		ctx->extn->send_discovery(ctx->extn->ctx, peer->addr);
	}

	if (!peer_in_fast_connect)
		return;

	tdls_auto_set_timer(ctx, ctx->extn->tdls_auto_fast_connect_period,
			    tdls_auto_fast_connect_timeout);
}


static void tdls_auto_slow_connect_timeout(void *eloop_ctx, void *timeout_ctx)
{
	struct tdls_auto_mode_ctx *ctx = eloop_ctx;
	struct tdls_auto_peer *peer, *tmp;

	dl_list_for_each_safe(peer, tmp, &ctx->peers, struct tdls_auto_peer,
			      list) {
		if (peer->connected)
			continue;

		/* fast connect timer will take care of connection here */
		if (peer->fast_attempts <= TDLS_AUTO_MAX_FAST_CONN_ATTEMPTS)
			continue;

		/* avoid discovery if peer traffic is not fast enough */
		tdls_auto_get_peer_data_rate(ctx, peer);
		tdls_auto_excessive("TDLSAUTO: data-rate of unconnected peer "
				    MACSTR ": %ld", MAC2STR(peer->addr),
				    (unsigned long)peer->data_rate);
		if (peer->data_rate <
				ctx->extn->tdls_auto_data_connect_threshold)
			continue;

		tdls_auto_debug("TDLSAUTO: slow connect - sending discovery to "
				MACSTR, MAC2STR(peer->addr));
		ctx->extn->send_discovery(ctx->extn->ctx, peer->addr);
	}

	if (!ctx->peer_count)
		return;

	tdls_auto_set_timer(ctx, ctx->extn->tdls_auto_slow_connect_period,
			    tdls_auto_slow_connect_timeout);
}


static void tdls_auto_data_teardown_timeout(void *eloop_ctx, void *timeout_ctx)
{
	struct tdls_auto_mode_ctx *ctx = eloop_ctx;
	struct tdls_auto_peer *peer, *tmp;

	dl_list_for_each_safe(peer, tmp, &ctx->peers, struct tdls_auto_peer,
			      list) {
		if (!peer->connected)
			continue;

		tdls_auto_get_peer_data_rate(ctx, peer);
		tdls_auto_excessive("TDLSAUTO: data-rate of connected peer "
				    MACSTR ": %ld", MAC2STR(peer->addr),
				    (unsigned long)peer->data_rate);
		if (peer->data_rate >=
				ctx->extn->tdls_auto_data_teardown_threshold)
			continue;

		tdls_auto_debug("TDLSAUTO: Removing peer " MACSTR
				" because of low data rate %ld",
				MAC2STR(peer->addr),
				(unsigned long)peer->data_rate);

		/* this might remove an incoming peer */
		ctx->extn->disconnect(ctx->extn->ctx, peer->addr);
	}

	if (!ctx->conn_peer_count)
		return;

	tdls_auto_set_timer(ctx, ctx->extn->tdls_auto_data_teardown_period,
			    tdls_auto_data_teardown_timeout);
}


static void tdls_auto_rssi_teardown_timeout(void *eloop_ctx, void *timeout_ctx)
{
	struct tdls_auto_mode_ctx *ctx = eloop_ctx;
	struct tdls_auto_peer *peer, *tmp;

	dl_list_for_each_safe(peer, tmp, &ctx->peers, struct tdls_auto_peer,
			      list) {
		if (!peer->connected)
			continue;

		tdls_auto_get_connected_sta_rssi(ctx, peer);
		if (peer->rssi >=
				ctx->extn->tdls_auto_rssi_teardown_threshold) {
			peer->low_rssi_vals = 0;
			continue;
		}

		peer->low_rssi_vals++;
		tdls_auto_debug("TDLSAUTO: bad RSSI %d for peer " MACSTR
				" for %d consecutive times", peer->rssi,
				MAC2STR(peer->addr), peer->low_rssi_vals);
		if (peer->low_rssi_vals <=
				ctx->extn->tdls_auto_rssi_teardown_count)
			continue;

		tdls_auto_debug("TDLSAUTO: Removing peer " MACSTR
				" because of low RSSI %d", MAC2STR(peer->addr),
				peer->rssi);
		/* this might remove an incoming peer */
		ctx->extn->disconnect(ctx->extn->ctx, peer->addr);
		peer->low_rssi_vals = 0;
	}

	if (!ctx->conn_peer_count)
		return;

	tdls_auto_set_timer(ctx, ctx->extn->tdls_auto_rssi_teardown_period,
			    tdls_auto_rssi_teardown_timeout);
}


static void tdls_auto_peer_free(struct tdls_auto_mode_ctx *ctx,
				struct tdls_auto_peer *peer)
{
	ctx->extn->monitor_traffic(ctx->extn->ctx, peer->addr, 0);

	dl_list_del(&peer->list);
	os_free(peer);

	ctx->peer_count--;
}


void tdls_auto_remove_peers(struct tdls_auto_mode_ctx *ctx,
			    int kill_active_links)
{
	struct tdls_auto_peer *peer, *tmp;

	/* sometimes we are called after deinit */
	if (!ctx)
		return;

	dl_list_for_each_safe(peer, tmp, &ctx->peers, struct tdls_auto_peer,
			      list) {
		tdls_auto_debug("TDLSAUTO: Remove peer " MACSTR,
				MAC2STR(peer->addr));
		if (!kill_active_links)
			peer->connected = 0;
		tdls_auto_mode_stop(ctx, peer->addr);
	}
}


struct tdls_auto_mode_ctx *tdls_auto_init(struct wpas_tdls_auto_ctx *extn)
{
	struct tdls_auto_mode_ctx *ctx = os_zalloc(sizeof(*ctx));

	if (!ctx) {
		tdls_auto_err("TDLSAUTO: Failed to allocate context");
		return NULL;
	}

	ctx->extn = extn;
	dl_list_init(&ctx->peers);

	tdls_auto_info("TDLSAUTO: initialized");
	return ctx;
}


void tdls_auto_deinit(struct tdls_auto_mode_ctx *ctx)
{
	if (!ctx)
		return;

	tdls_auto_remove_peers(ctx, 0);
	os_free(ctx->extn);
	os_free(ctx);
}


static struct tdls_auto_peer *
tdls_auto_add_peer(struct tdls_auto_mode_ctx *ctx, const u8 *addr)
{
	struct tdls_auto_peer *peer;

	/*
	 * the peer might already exist in kernel because of a previous wpa_s
	 * crash, so remove it before adding it.
	 */
	ctx->extn->monitor_traffic(ctx->extn->ctx, addr, 0);
	if (ctx->extn->monitor_traffic(ctx->extn->ctx, addr, 1)) {
		tdls_auto_err("TDLSAUTO: could not add peer to traffic accounting");
		return NULL;
	}

	peer = tdls_auto_peer_add_to_list(ctx, addr);
	if (!peer) {
		tdls_auto_err("TDLSAUTO: cannot add new member to list");
		ctx->extn->monitor_traffic(ctx->extn->ctx, addr, 0);
		return NULL;
	}

	/* start fast connect timer when any peer is added */
	tdls_auto_set_timer(ctx, ctx->extn->tdls_auto_fast_connect_period,
			    tdls_auto_fast_connect_timeout);

	if (ctx->peer_count != 1)
		goto out;

	/* start slow connect timer when the first peer is added */
	tdls_auto_set_timer(ctx, ctx->extn->tdls_auto_slow_connect_period,
			    tdls_auto_slow_connect_timeout);

out:
	return peer;
}


int tdls_auto_mode_start(struct tdls_auto_mode_ctx *ctx, const u8 *addr)
{
	struct tdls_auto_peer *peer;

	if (!ctx)
		return -1;

	peer = tdls_auto_find_peer(ctx, addr);
	if (peer) {
		tdls_auto_debug("TDLSAUTO: existing peer " MACSTR,
				MAC2STR(addr));
		return 0;
	}

	peer = tdls_auto_add_peer(ctx, addr);
	if (!peer)
		return -1;

	tdls_auto_info("TDLSAUTO: starting auto-mode for " MACSTR
		       " total peers: %d", MAC2STR(addr), ctx->peer_count);
	return 0;
}


void tdls_auto_mode_stop(struct tdls_auto_mode_ctx *ctx, const u8 *addr)
{
	struct tdls_auto_peer *peer;

	if (!ctx)
		return;

	peer = tdls_auto_find_peer(ctx, addr);
	if (!peer) {
		tdls_auto_err("TDLSAUTO: Could not find peer " MACSTR
			      " to stop auto-mode", MAC2STR(addr));
		return;
	}

	tdls_auto_info("TDLSAUTO: stopping auto-mode for " MACSTR
		       " total peers: %d", MAC2STR(addr), ctx->peer_count);

	/*
	 * corner case here - we might have been connected because of an
	 * incoming connection, but we assume the remote end will try again
	 * in that case.
	 * Clear the incoming_peer member before calling the disconnect,
	 * to avoid a double-free of the peer in the disconnect callback.
	 */
	if (peer->connected) {
		peer->incoming_peer = 0;
		ctx->extn->disconnect(ctx->extn->ctx, peer->addr);
	}

	tdls_auto_peer_free(ctx, peer);

	/* remove connect timeouts if it's the last peer */
	if (!ctx->peer_count) {
		tdls_auto_cancel_timer(ctx, tdls_auto_fast_connect_timeout);
		tdls_auto_cancel_timer(ctx, tdls_auto_slow_connect_timeout);
	}
}


void tdls_auto_peer_connected(struct tdls_auto_mode_ctx *ctx, const u8 *addr)
{
	struct tdls_auto_peer *peer;

	if (!ctx)
		return;

	ctx->conn_peer_count++;

	peer = tdls_auto_find_peer(ctx, addr);
	if (!peer) {
		peer = tdls_auto_add_peer(ctx, addr);
		if (!peer)
			return;

		peer->incoming_peer = 1;
	}

	peer->connected = 1;

	/*
	 * get the initial peer data rate, so later invocations get the diff
	 * correctly. Also reset the idle-teardown timer. A switch to TDLS
	 * mode can momentarily hurt the peer's traffic and an unfortunate
	 * idle check right on connect might otherwise wrongly disconnect the
	 * peer.
	 */
	tdls_auto_get_peer_data_rate(ctx, peer);
	tdls_auto_set_timer(ctx, ctx->extn->tdls_auto_data_teardown_period,
			    tdls_auto_data_teardown_timeout);

	tdls_auto_debug("TDLSAUTO: peer " MACSTR " connected", MAC2STR(addr));

	/* initialize RSSI disconnection timer on first connected peer */
	if (ctx->conn_peer_count != 1)
		return;

	tdls_auto_set_timer(ctx, ctx->extn->tdls_auto_rssi_teardown_period,
			    tdls_auto_rssi_teardown_timeout);
}


void tdls_auto_peer_disconnected(struct tdls_auto_mode_ctx *ctx, const u8 *addr)
{
	struct tdls_auto_peer *peer;

	if (!ctx)
		return;

	peer = tdls_auto_find_peer(ctx, addr);
	if (!peer)
		return;

	tdls_auto_debug("TDLSAUTO: %s peer " MACSTR " disconnected",
			peer->incoming_peer ? "incoming" : "outgoing",
			MAC2STR(addr));

	peer->connected = 0;

	if (peer->incoming_peer) {
		/* don't track incoming peers after disconnection */
		tdls_auto_mode_stop(ctx, peer->addr);
	} else {
		/* immediately try fast-reconnect for outgoing peer */
		peer->low_rssi_vals = 0;
		peer->fast_attempts = 0;
		tdls_auto_set_timer(ctx,
				    ctx->extn->tdls_auto_fast_connect_period,
				    tdls_auto_fast_connect_timeout);
	}

	ctx->conn_peer_count--;
}


void tdls_auto_discovery_response(struct tdls_auto_mode_ctx *ctx,
				  const u8 *addr, int rssi)
{
	struct tdls_auto_peer *peer;
	int res;

	if (!ctx)
		return;

	tdls_auto_debug("TDLSAUTO: Discovery response from " MACSTR " RSSI %d",
			MAC2STR(addr), rssi);

	peer = tdls_auto_find_peer(ctx, addr);
	if (!peer)
		return;

	peer->rssi = rssi;

	if (peer->connected) {
		tdls_auto_err("TDLSAUTO: discovery-resp from connected peer "
			      MACSTR, MAC2STR(peer->addr));
		return;
	}

	if (peer->rssi <= ctx->extn->tdls_auto_rssi_connect_threshold)
		return;

	/* make sure an unsolicited discovery response won't game the system */
	tdls_auto_get_peer_data_rate(ctx, peer);
	if (peer->data_rate < ctx->extn->tdls_auto_data_connect_threshold)
		return;

	/* don't start connecting if we have the maximum peers already */
	if (ctx->conn_peer_count >= ctx->extn->tdls_auto_max_connected_peers) {
		tdls_auto_debug("TDLSAUTO: avoiding new connection"
				" because of too much connected peers");
		return;
	}

	res = ctx->extn->connect(ctx->extn->ctx, peer->addr);
	tdls_auto_debug("TDLSAUTO: connecting " MACSTR " res %d",
			MAC2STR(peer->addr), res);
}
