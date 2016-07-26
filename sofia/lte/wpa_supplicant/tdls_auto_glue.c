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

#include "rsn_supp/wpa.h"
#include "rsn_supp/wpa_i.h"
#include "config.h"
#include "wpa_supplicant_i.h"
#include "driver_i.h"

#include "tdls_auto_supplicant.h"
#include "tdls_auto_mode.h"

/* for building vendor commands */
#include <netlink/genl/genl.h>
#include "drivers/iwl_vendor_cmd_copy.h"

/* for compatible nlmsg_len definition */
#include "drivers/driver_nl80211.h"

static struct wpa_supplicant *wpas_tdls_auto_get_wpa(struct wpa_sm *sm)
{
	return (struct wpa_supplicant *)sm->ctx->ctx;
}


void wpas_tdls_auto_peer_connected(struct wpa_sm *sm, const u8 *addr)
{
	struct wpa_supplicant *wpa_s = wpas_tdls_auto_get_wpa(sm);

	if (!wpa_s->conf->tdls_auto_enabled || !wpa_s->tdls_auto)
		return;

	tdls_auto_peer_connected(wpa_s->tdls_auto, addr);
}


void wpas_tdls_auto_peer_disconnected(struct wpa_sm *sm, const u8 *addr)
{
	struct wpa_supplicant *wpa_s = wpas_tdls_auto_get_wpa(sm);

	if (!wpa_s->conf->tdls_auto_enabled || !wpa_s->tdls_auto)
		return;

	tdls_auto_peer_disconnected(wpa_s->tdls_auto, addr);
}


void wpas_tdls_auto_remove_peers(struct wpa_supplicant *wpa_s,
				 int kill_active_links)
{
	if (!wpa_s->conf->tdls_auto_enabled || !wpa_s->tdls_auto)
		return;

	tdls_auto_remove_peers(wpa_s->tdls_auto, kill_active_links);
}


int wpas_tdls_auto_start(struct wpa_supplicant *wpa_s, const u8 *addr)
{
	if (!wpa_s->conf->tdls_auto_enabled || !wpa_s->tdls_auto)
		return -1;

	return tdls_auto_mode_start(wpa_s->tdls_auto, addr);
}


void wpas_tdls_auto_stop(struct wpa_supplicant *wpa_s, const u8 *addr)
{
	if (!wpa_s->conf->tdls_auto_enabled || !wpa_s->tdls_auto)
		return;

	tdls_auto_mode_stop(wpa_s->tdls_auto, addr);
}


void wpas_tdls_auto_discovery_response(struct wpa_supplicant *wpa_s,
				       const u8 *addr, int rssi)
{
	if (!wpa_s->conf->tdls_auto_enabled || !wpa_s->tdls_auto)
		return;

	tdls_auto_discovery_response(wpa_s->tdls_auto, addr, rssi);
}


static int wpas_tdls_auto_connect(void *ctx, const u8 *addr)
{
	struct wpa_supplicant *wpa_s = ctx;

	if (!wpa_s)
		return -1;

	return wpa_tdls_start(wpa_s->wpa, addr);
}


static void wpas_tdls_auto_disconnect(void *ctx, const u8 *addr)
{
	struct wpa_supplicant *wpa_s = ctx;

	if (!wpa_s)
		return;

	wpa_tdls_teardown_link(wpa_s->wpa, addr,
			       WLAN_REASON_TDLS_TEARDOWN_UNSPECIFIED);
}


static void wpas_tdls_auto_send_discovery(void *ctx, const u8 *addr)
{
	struct wpa_supplicant *wpa_s = ctx;

	if (!wpa_s)
		return;

	wpa_tdls_send_discovery_request(wpa_s->wpa, addr);
}


static int wpas_tdls_auto_get_rssi(void *ctx, const u8 *addr)
{
	struct wpa_supplicant *wpa_s = ctx;
	struct hostap_sta_driver_data data;

	os_memset(&data, 0, sizeof(data));

	if (!wpa_s || wpa_drv_read_sta_data(wpa_s, &data, addr))
		return -102;

	return data.last_rssi;
}


static int
wpas_tdls_auto_monitor_traffic(void *ctx, const u8 *addr, int add)
{
	struct wpa_supplicant *wpa_s = ctx;
	struct nl_msg *msg;
	int ret;

	if (!wpa_s)
		return -1;

	msg = nlmsg_alloc();
	if (!msg)
		return -ENOMEM;

	if (nla_put(msg, IWL_MVM_VENDOR_ATTR_ADDR, ETH_ALEN, addr)) {
		ret = -ENOBUFS;
		goto out;
	}

	ret = wpa_drv_vendor_cmd(wpa_s, INTEL_OUI,
				 add ? IWL_MVM_VENDOR_CMD_TDLS_PEER_CACHE_ADD :
				       IWL_MVM_VENDOR_CMD_TDLS_PEER_CACHE_DEL,
				 nlmsg_data(nlmsg_hdr(msg)),
				 nlmsg_datalen(nlmsg_hdr(msg)), NULL);
out:
	nlmsg_free(msg);
	return ret;
}


static int
wpas_tdls_auto_get_sta_bytes(void *ctx, const u8 *addr, u32 *tx_bytes,
			     u32 *rx_bytes)
{
	struct wpa_supplicant *wpa_s = ctx;
	struct nl_msg *msg;
	int ret;
	struct wpabuf *buf;

	if (!wpa_s)
		return -EINVAL;

	buf = wpabuf_alloc(sizeof(u32) * 2 + 50);
	if (!buf)
		return -ENOMEM;

	msg = nlmsg_alloc();
	if (!msg) {
		ret = -ENOMEM;
		goto out_free_buf;
	}

	if (nla_put(msg, IWL_MVM_VENDOR_ATTR_ADDR, ETH_ALEN, addr)) {
		ret = -ENOBUFS;
		goto out_free_msg;
	}

	ret = wpa_drv_vendor_cmd(wpa_s, INTEL_OUI,
				 IWL_MVM_VENDOR_CMD_TDLS_PEER_CACHE_QUERY,
				 nlmsg_data(nlmsg_hdr(msg)),
				 nlmsg_datalen(nlmsg_hdr(msg)), buf);
	if (!ret) {
		*tx_bytes = *(u32 *)wpabuf_head(buf);
		*rx_bytes = *(u32 *)(wpabuf_head(buf) + sizeof(u32));
	}

out_free_msg:
	nlmsg_free(msg);
out_free_buf:
	wpabuf_free(buf);
	return ret;
}


int wpas_tdls_auto_init(struct wpa_supplicant *wpa_s)
{
	struct wpas_tdls_auto_ctx *glue;

	if (!wpa_s->conf->tdls_auto_enabled)
		return 0;

	if (!wpa_tdls_is_external_setup(wpa_s->wpa)) {
		tdls_auto_err("TDLSAUTO: TDLS support and external setup "
			      "required for auto-mode");
		return -1;
	}

	if (wpa_s->tdls_auto) {
		tdls_auto_err("TDLSAUTO: already initialized");
		return -1;
	}

	glue = os_zalloc(sizeof(*glue));
	if (!glue) {
		tdls_auto_err("TDLSAUTO: Failed to allocate context");
		return -1;
	}

	glue->ctx = wpa_s;
	glue->connect = wpas_tdls_auto_connect;
	glue->disconnect = wpas_tdls_auto_disconnect;
	glue->send_discovery = wpas_tdls_auto_send_discovery;
	glue->get_rssi = wpas_tdls_auto_get_rssi;
	glue->monitor_traffic = wpas_tdls_auto_monitor_traffic;
	glue->get_sta_bytes = wpas_tdls_auto_get_sta_bytes;
	glue->tdls_auto_rssi_connect_threshold =
				wpa_s->conf->tdls_auto_rssi_connect_threshold;
	glue->tdls_auto_data_connect_threshold =
				wpa_s->conf->tdls_auto_data_connect_threshold;
	glue->tdls_auto_fast_connect_period =
				wpa_s->conf->tdls_auto_fast_connect_period;
	glue->tdls_auto_slow_connect_period =
				wpa_s->conf->tdls_auto_slow_connect_period;
	glue->tdls_auto_data_teardown_threshold =
				wpa_s->conf->tdls_auto_data_teardown_threshold;
	glue->tdls_auto_data_teardown_period =
				wpa_s->conf->tdls_auto_data_teardown_period;
	glue->tdls_auto_rssi_teardown_threshold =
				wpa_s->conf->tdls_auto_rssi_teardown_threshold;
	glue->tdls_auto_rssi_teardown_period =
				wpa_s->conf->tdls_auto_rssi_teardown_period;
	glue->tdls_auto_rssi_teardown_count =
				wpa_s->conf->tdls_auto_rssi_teardown_count;
	glue->tdls_auto_max_connected_peers =
				wpa_s->conf->tdls_auto_max_connected_peers;

	if (glue->tdls_auto_fast_connect_period >
					glue->tdls_auto_slow_connect_period) {
		tdls_auto_err("TDLSAUTO: fast cycle (%d) must be shorter than slow (%d)",
			      glue->tdls_auto_fast_connect_period,
			      glue->tdls_auto_slow_connect_period);
		os_free(glue);
		return -1;
	}

	wpa_s->tdls_auto = tdls_auto_init(glue);
	if (!wpa_s->tdls_auto) {
		tdls_auto_err("TDLSAUTO: init failure");
		os_free(glue);
		return -1;
	}

	return 0;
}


void wpas_tdls_auto_deinit(struct wpa_supplicant *wpa_s)
{
	if (!wpa_s->conf->tdls_auto_enabled || !wpa_s->tdls_auto)
		return;

	tdls_auto_deinit(wpa_s->tdls_auto);
	wpa_s->tdls_auto = NULL;
}
