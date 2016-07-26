/*
 * WPA Supplicant - background scan and roaming interface
 * Copyright (c) 2009-2010, Jouni Malinen <j@w1.fi>
 * Copyright(c) 2013 - 2014 Intel Mobile Communications GmbH.
 * Copyright(c) 2011 - 2014 Intel Corporation. All rights reserved.
 *
 * This software may be distributed under the terms of the BSD license.
 * See README for more details.
 */

#include "includes.h"

#include "common.h"
#include "wpa_supplicant_i.h"
#include "config_ssid.h"
#include "bgscan.h"

#ifdef CONFIG_BGSCAN_SIMPLE
extern const struct bgscan_ops bgscan_simple_ops;
#endif /* CONFIG_BGSCAN_SIMPLE */
#ifdef CONFIG_BGSCAN_LEARN
extern const struct bgscan_ops bgscan_learn_ops;
#endif /* CONFIG_BGSCAN_LEARN */

static const struct bgscan_ops * bgscan_modules[] = {
#ifdef CONFIG_BGSCAN_SIMPLE
	&bgscan_simple_ops,
#endif /* CONFIG_BGSCAN_SIMPLE */
#ifdef CONFIG_BGSCAN_LEARN
	&bgscan_learn_ops,
#endif /* CONFIG_BGSCAN_LEARN */
	NULL
};


int bgscan_init(struct wpa_supplicant *wpa_s, struct wpa_ssid *ssid,
		const char *name)
{
	const char *params;
	size_t nlen;
	int i;
	const struct bgscan_ops *ops = NULL;

	bgscan_deinit(wpa_s);
	if (name == NULL)
		return -1;

	params = os_strchr(name, ':');
	if (params == NULL) {
		params = "";
		nlen = os_strlen(name);
	} else {
		nlen = params - name;
		params++;
	}

	for (i = 0; bgscan_modules[i]; i++) {
		if (os_strncmp(name, bgscan_modules[i]->name, nlen) == 0) {
			ops = bgscan_modules[i];
			break;
		}
	}

	if (ops == NULL) {
		wpa_printf(MSG_ERROR, "bgscan: Could not find module "
			   "matching the parameter '%s'", name);
		return -1;
	}

	wpa_s->bgscan_priv = ops->init(wpa_s, params, ssid);
	if (wpa_s->bgscan_priv == NULL)
		return -1;
	wpa_s->bgscan = ops;
	wpa_printf(MSG_DEBUG, "bgscan: Initialized module '%s' with "
		   "parameters '%s'", ops->name, params);

	return 0;
}


void bgscan_deinit(struct wpa_supplicant *wpa_s)
{
	if (wpa_s->bgscan && wpa_s->bgscan_priv) {
		wpa_printf(MSG_DEBUG, "bgscan: Deinitializing module '%s'",
			   wpa_s->bgscan->name);
		wpa_s->bgscan->deinit(wpa_s->bgscan_priv);
		wpa_s->bgscan = NULL;
		wpa_s->bgscan_priv = NULL;
	}
}


int bgscan_notify_scan(struct wpa_supplicant *wpa_s,
		       struct wpa_scan_results *scan_res,
		       int notify_only)
{
	if (wpa_s->bgscan && wpa_s->bgscan_priv && wpa_s->bgscan->notify_scan)
		return wpa_s->bgscan->notify_scan(wpa_s->bgscan_priv,
						  scan_res, notify_only);
	return 0;
}


void bgscan_notify_beacon_loss(struct wpa_supplicant *wpa_s)
{
	if (wpa_s->bgscan && wpa_s->bgscan_priv &&
	    wpa_s->bgscan->notify_beacon_loss)
		wpa_s->bgscan->notify_beacon_loss(wpa_s->bgscan_priv);
}


void bgscan_notify_signal_change(struct wpa_supplicant *wpa_s, int above,
				 int current_signal, int current_noise,
				 int current_txrate)
{
	if (wpa_s->bgscan && wpa_s->bgscan_priv &&
	    wpa_s->bgscan->notify_signal_change)
		wpa_s->bgscan->notify_signal_change(wpa_s->bgscan_priv, above,
						    current_signal,
						    current_noise,
						    current_txrate);
}

void bgscan_notify_tcm_changed(struct wpa_supplicant *wpa_s,
			       enum traffic_load traffic_load,
			       int vi_vo_present)
{
	if (wpa_s->bgscan && wpa_s->bgscan_priv &&
	    wpa_s->bgscan->notify_tcm_changed)
		wpa_s->bgscan->notify_tcm_changed(wpa_s->bgscan_priv,
						  traffic_load, vi_vo_present);
}

void bgscan_notify_scan_trigger(struct wpa_supplicant *wpa_s,
				struct wpa_driver_scan_params *params)
{
	if (wpa_s->bgscan && wpa_s->bgscan_priv &&
	    wpa_s->bgscan->notify_scan_trigger)
		wpa_s->bgscan->notify_scan_trigger(wpa_s->bgscan_priv, params);
}
