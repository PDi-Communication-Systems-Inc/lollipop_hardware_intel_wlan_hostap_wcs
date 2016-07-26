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

#ifndef _TDLS_AUTO_SUPPLICANT_H_
#define _TDLS_AUTO_SUPPLICANT_H_

#include "wpa_supplicant_i.h"

#ifdef CONFIG_TDLS_AUTO_MODE
int wpas_tdls_auto_init(struct wpa_supplicant *wpa_s);
void wpas_tdls_auto_deinit(struct wpa_supplicant *wpa_s);
void wpas_tdls_auto_peer_connected(struct wpa_sm *sm, const u8 *addr);
void wpas_tdls_auto_peer_disconnected(struct wpa_sm *sm, const u8 *addr);
void wpas_tdls_auto_remove_peers(struct wpa_supplicant *wpa_s,
				 int kill_active_links);
int wpas_tdls_auto_start(struct wpa_supplicant *wpa_s, const u8 *addr);
void wpas_tdls_auto_stop(struct wpa_supplicant *wpa_s, const u8 *addr);
void wpas_tdls_auto_discovery_response(struct wpa_supplicant *wpa_s,
				       const u8 *addr, int rssi);
#else
static inline int wpas_tdls_auto_init(struct wpa_supplicant *wpa_s)
{
	return 0;
}

static inline void wpas_tdls_auto_deinit(struct wpa_supplicant *wpa_s)
{
}

static inline void wpas_tdls_auto_peer_connected(struct wpa_sm *sm,
						 const u8 *addr)
{
}

static inline void wpas_tdls_auto_peer_disconnected(struct wpa_sm *sm,
						    const u8 *addr)
{
}

static inline void wpas_tdls_auto_remove_peers(struct wpa_supplicant *wpa_s,
					       int kill_active_links)
{
}

static inline int wpas_tdls_auto_start(struct wpa_supplicant *wpa_s,
				       const u8 *addr)
{
	return 0;
}

static inline void wpas_tdls_auto_stop(struct wpa_supplicant *wpa_s,
				       const u8 *addr)
{
}

static inline void
wpas_tdls_auto_discovery_response(struct wpa_supplicant *wpa_s, const u8 *addr,
				  int rssi)
{
}
#endif /* CONFIG_TDLS_AUTO_MODE */


#endif /* TDLS_AUTO_SUPPLICANT_H_ */
