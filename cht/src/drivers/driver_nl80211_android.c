/*
 * Driver interaction with Linux nl80211/cfg80211 - Android specific
 * Copyright (c) 2002-2014, Jouni Malinen <j@w1.fi>
 * Copyright (c) 2007, Johannes Berg <johannes@sipsolutions.net>
 * Copyright (c) 2009-2010, Atheros Communications
 *
 * This software may be distributed under the terms of the BSD license.
 * See README for more details.
 */

#include "includes.h"
#include <sys/ioctl.h>
#include <net/if.h>
#include <netlink/genl/genl.h>
#include <netlink/genl/family.h>
#include <netlink/genl/ctrl.h>
#include <fcntl.h>
#include <cutils/properties.h>

#include "utils/common.h"
#include "driver_nl80211.h"
#include "android_drv.h"
#include "linux_ioctl.h"
#include "eloop.h"

/* special telephony property for getting current country code */
#define TELEPHONY_ISO_COUNTRY_PROPERTY "gsm.operator.iso-country"
/* polling period for getting the current country from a telephony property */
#define COUNTRY_CODE_POLL_PERIOD_SEC	(15 * 60)
/* fast polling period for when reception is out */
#define COUNTRY_CODE_FAST_POLL_PERIOD_SEC	(60)
/* grace period until we reset the country code because of reception loss */
#define CELL_RECEPTION_LOSS_GRACE_SEC	(15 * 60)

#ifdef CONFIG_LIBNL20
/*
 * The libnl implementation used in ANDROID-KK and earlier does not implement
 * nla_put_flag and nla_put_string. Add the implementation here.
 */
int nla_put_flag(struct nl_msg *msg, int attrtype)
{
	return nla_put(msg, attrtype, 0, NULL);
}

int nla_put_string(struct nl_msg *msg, int attrtype, const char *str)
{
	return nla_put(msg, attrtype, strlen(str) + 1, str);
}
#endif /* CONFIG_LIBNL20 */

typedef struct android_wifi_priv_cmd {
	char *buf;
	int used_len;
	int total_len;
} android_wifi_priv_cmd;

static int drv_errors = 0;

void nl80211_report_hang(struct wpa_driver_nl80211_data *drv)
{
	struct nl80211_global *global = drv->global;
	struct wpa_driver_nl80211_data *tmp;

	/* fire the event on one of the static interfaces */
	dl_list_for_each_safe(drv, tmp, &global->interfaces,
			      struct wpa_driver_nl80211_data, list) {
		if (!drv->first_bss->if_dynamic)
			break;
	}
	wpa_printf(MSG_DEBUG, "nl80211: %s reporting HANGED",
		   drv->first_bss->ifname);

	wpa_msg(drv->ctx, MSG_INFO, WPA_EVENT_DRIVER_STATE "HANGED");
}


static void wpa_driver_send_hang_msg(struct wpa_driver_nl80211_data *drv)
{
	drv_errors++;
	if (drv_errors > DRV_NUMBER_SEQUENTIAL_ERRORS) {
		drv_errors = 0;
		nl80211_report_hang(drv);
	}
}


#define WPA_PS_ENABLED  0
#define WPA_PS_DISABLED	1

static int wpa_driver_set_power_save(void *priv, int state)
{
	return nl80211_set_power_save(priv, state == WPA_PS_ENABLED);
}

static int get_power_mode_handler(struct nl_msg *msg, void *arg)
{
	struct nlattr *tb[NL80211_ATTR_MAX + 1];
	struct genlmsghdr *gnlh = nlmsg_data(nlmsg_hdr(msg));
	int *state = arg;

	nla_parse(tb, NL80211_ATTR_MAX, genlmsg_attrdata(gnlh, 0),
		  genlmsg_attrlen(gnlh, 0), NULL);

	if (!tb[NL80211_ATTR_PS_STATE])
		return NL_SKIP;

	if (state) {
		int s = (int) nla_get_u32(tb[NL80211_ATTR_PS_STATE]);
		wpa_printf(MSG_DEBUG, "nl80211: Get power mode = %d", s);
		*state = (s == NL80211_PS_ENABLED) ?
			WPA_PS_ENABLED : WPA_PS_DISABLED;
	}

	return NL_SKIP;
}


static int wpa_driver_get_power_save(void *priv, int *state)
{
	struct i802_bss *bss = priv;
	struct wpa_driver_nl80211_data *drv = bss->drv;
	struct nl_msg *msg;
	int ret = -1;
	enum nl80211_ps_state ps_state;

	msg = nlmsg_alloc();
	if (!msg)
		return -1;

	nl80211_cmd(drv, msg, 0, NL80211_CMD_GET_POWER_SAVE);

	NLA_PUT_U32(msg, NL80211_ATTR_IFINDEX, bss->ifindex);

	ret = send_and_recv_msgs(drv, msg, get_power_mode_handler, state);
	msg = NULL;
	if (ret < 0)
		wpa_printf(MSG_ERROR, "nl80211: Get power mode fail: %d", ret);
nla_put_failure:
	nlmsg_free(msg);
	return ret;
}


static void wpa_driver_poll_country_change(void *eloop_ctx, void *timeout_ctx)
{
	struct nl80211_global *global = eloop_ctx;
	struct wpa_driver_nl80211_data *drv;
	char country[PROP_VALUE_MAX];
	char *cc, *pos;
	struct os_reltime now;

	/*
	 * if there are no interfaces, we can't send the message - too early
	 * or too late
	 */
	if (dl_list_empty(&global->interfaces))
		goto poll_again;

	dl_list_for_each(drv, &global->interfaces,
			 struct wpa_driver_nl80211_data, list) {
		if (drv->nlmode == NL80211_IFTYPE_STATION)
			break;
	}

	if (drv->nlmode != NL80211_IFTYPE_STATION) {
		wpa_printf(MSG_ERROR, "nl80211: could not find STA interface");
		return;
	}

	property_get(TELEPHONY_ISO_COUNTRY_PROPERTY, country, "");
	wpa_printf(MSG_DEBUG, "nl80211: full current country: %s", country);
	cc = country;
	pos = os_strchr(country, ',');
	if (pos) {
		*pos = '\0';
		/* the first country is null */
		if (pos == cc)
			cc = pos + 1;
	}

	/* no change */
	if (!os_strcmp(global->country_alpha2, cc))
		goto poll_again;

	wpa_printf(MSG_DEBUG, "nl80211: country changed: %s -> %s",
		   global->country_alpha2, cc);

	/* give a 15min grace period after reception loss */
	if (*cc == '\0') {
		if (!os_reltime_initialized(&global->cell_reception_loss)) {
			wpa_printf(MSG_DEBUG,
				   "nl80211: delaying reception loss");
			os_get_reltime(&global->cell_reception_loss);
			goto poll_again;
		} else {
			os_get_reltime(&now);
			if (!os_reltime_expired(&now,
						&global->cell_reception_loss,
						CELL_RECEPTION_LOSS_GRACE_SEC))
				goto poll_again;

			wpa_printf(MSG_DEBUG,
				   "nl80211: sending reception loss");
			global->cell_reception_loss.sec = 0;
			global->cell_reception_loss.usec = 0;
			global->during_cell_reception_loss = 1;
		}
	} else {
		global->cell_reception_loss.sec = 0;
		global->cell_reception_loss.usec = 0;
		global->during_cell_reception_loss = 0;
	}

	os_strlcpy(global->country_alpha2, cc, 3);
	nl80211_set_country(drv->first_bss, global->country_alpha2);

poll_again:
	eloop_cancel_timeout(wpa_driver_poll_country_change, eloop_ctx,
			     timeout_ctx);
	eloop_register_timeout(global->during_cell_reception_loss ?
			       COUNTRY_CODE_FAST_POLL_PERIOD_SEC :
			       COUNTRY_CODE_POLL_PERIOD_SEC,
			       0, wpa_driver_poll_country_change, eloop_ctx,
			       timeout_ctx);
}


static int android_priv_cmd(struct i802_bss *bss, const char *cmd)
{
	struct wpa_driver_nl80211_data *drv = bss->drv;
	struct ifreq ifr;
	android_wifi_priv_cmd priv_cmd;
	char buf[MAX_DRV_CMD_SIZE];
	int ret;

	os_memset(&ifr, 0, sizeof(ifr));
	os_memset(&priv_cmd, 0, sizeof(priv_cmd));
	os_strlcpy(ifr.ifr_name, bss->ifname, IFNAMSIZ);

	os_memset(buf, 0, sizeof(buf));
	os_strlcpy(buf, cmd, sizeof(buf));

	priv_cmd.buf = buf;
	priv_cmd.used_len = sizeof(buf);
	priv_cmd.total_len = sizeof(buf);
	ifr.ifr_data = &priv_cmd;

	ret = ioctl(drv->global->ioctl_sock, SIOCDEVPRIVATE + 1, &ifr);
	if (ret < 0) {
		wpa_printf(MSG_ERROR, "%s: failed to issue private commands",
			   __func__);
		wpa_driver_send_hang_msg(drv);
		return ret;
	}

	drv_errors = 0;
	return 0;
}


int android_pno_start(struct i802_bss *bss,
		      struct wpa_driver_scan_params *params)
{
	struct wpa_driver_nl80211_data *drv = bss->drv;
	struct ifreq ifr;
	android_wifi_priv_cmd priv_cmd;
	int ret = 0, i = 0, bp;
	char buf[WEXT_PNO_MAX_COMMAND_SIZE];

	bp = WEXT_PNOSETUP_HEADER_SIZE;
	os_memcpy(buf, WEXT_PNOSETUP_HEADER, bp);
	buf[bp++] = WEXT_PNO_TLV_PREFIX;
	buf[bp++] = WEXT_PNO_TLV_VERSION;
	buf[bp++] = WEXT_PNO_TLV_SUBVERSION;
	buf[bp++] = WEXT_PNO_TLV_RESERVED;

	while (i < WEXT_PNO_AMOUNT && (size_t) i < params->num_ssids) {
		/* Check that there is enough space needed for 1 more SSID, the
		 * other sections and null termination */
		if ((bp + WEXT_PNO_SSID_HEADER_SIZE + MAX_SSID_LEN +
		     WEXT_PNO_NONSSID_SECTIONS_SIZE + 1) >= (int) sizeof(buf))
			break;
		wpa_hexdump_ascii(MSG_DEBUG, "For PNO Scan",
				  params->ssids[i].ssid,
				  params->ssids[i].ssid_len);
		buf[bp++] = WEXT_PNO_SSID_SECTION;
		buf[bp++] = params->ssids[i].ssid_len;
		os_memcpy(&buf[bp], params->ssids[i].ssid,
			  params->ssids[i].ssid_len);
		bp += params->ssids[i].ssid_len;
		i++;
	}

	buf[bp++] = WEXT_PNO_SCAN_INTERVAL_SECTION;
	os_snprintf(&buf[bp], WEXT_PNO_SCAN_INTERVAL_LENGTH + 1, "%x",
		    WEXT_PNO_SCAN_INTERVAL);
	bp += WEXT_PNO_SCAN_INTERVAL_LENGTH;

	buf[bp++] = WEXT_PNO_REPEAT_SECTION;
	os_snprintf(&buf[bp], WEXT_PNO_REPEAT_LENGTH + 1, "%x",
		    WEXT_PNO_REPEAT);
	bp += WEXT_PNO_REPEAT_LENGTH;

	buf[bp++] = WEXT_PNO_MAX_REPEAT_SECTION;
	os_snprintf(&buf[bp], WEXT_PNO_MAX_REPEAT_LENGTH + 1, "%x",
		    WEXT_PNO_MAX_REPEAT);
	bp += WEXT_PNO_MAX_REPEAT_LENGTH + 1;

	memset(&ifr, 0, sizeof(ifr));
	memset(&priv_cmd, 0, sizeof(priv_cmd));
	os_strlcpy(ifr.ifr_name, bss->ifname, IFNAMSIZ);

	priv_cmd.buf = buf;
	priv_cmd.used_len = bp;
	priv_cmd.total_len = bp;
	ifr.ifr_data = &priv_cmd;

	ret = ioctl(drv->global->ioctl_sock, SIOCDEVPRIVATE + 1, &ifr);

	if (ret < 0) {
		wpa_printf(MSG_ERROR, "ioctl[SIOCSIWPRIV] (pnosetup): %d",
			   ret);
		wpa_driver_send_hang_msg(drv);
		return ret;
	}

	drv_errors = 0;

	return android_priv_cmd(bss, "PNOFORCE 1");
}


int android_pno_stop(struct i802_bss *bss)
{
	return android_priv_cmd(bss, "PNOFORCE 0");
}


int android_nl_socket_set_nonblocking(struct nl_handle *handle)
{
	return fcntl(nl_socket_get_fd(handle), F_SETFL, O_NONBLOCK);
}


static struct wpa_driver_nl80211_data *
nl80211_global_get_p2pdev(struct nl80211_global *global)
{
	struct wpa_driver_nl80211_data *drv;

	dl_list_for_each(drv, &global->interfaces,
			 struct wpa_driver_nl80211_data, list) {
		if (drv->nlmode == NL80211_IFTYPE_P2P_DEVICE)
			return drv;
	}
	return NULL;
}


int wpa_driver_nl80211_driver_cmd(void *priv, char *cmd, char *buf,
				  size_t buf_len)
{
	struct i802_bss *bss = priv;
	struct wpa_driver_nl80211_data *drv = bss->drv;
	struct wpa_driver_nl80211_data *p2p_drv;
	struct ifreq ifr;
	android_wifi_priv_cmd priv_cmd;
	int ret = 0;

	if (os_strcasecmp(cmd, "STOP") == 0) {
		p2p_drv = nl80211_global_get_p2pdev(drv->global);
		if (p2p_drv)
			nl80211_set_p2pdev(p2p_drv->first_bss, 0);
		linux_set_iface_flags(drv->global->ioctl_sock, bss->ifname, 0);
		wpa_msg(drv->ctx, MSG_INFO, WPA_EVENT_DRIVER_STATE "STOPPED");
	} else if (os_strcasecmp(cmd, "START") == 0) {
		linux_set_iface_flags(drv->global->ioctl_sock, bss->ifname, 1);
		p2p_drv = nl80211_global_get_p2pdev(drv->global);
		if (p2p_drv)
			nl80211_set_p2pdev(p2p_drv->first_bss, 1);
		wpa_msg(drv->ctx, MSG_INFO, WPA_EVENT_DRIVER_STATE "STARTED");
	} else if (os_strcasecmp(cmd, "MACADDR") == 0) {
		u8 macaddr[ETH_ALEN] = {};

		ret = linux_get_ifhwaddr(drv->global->ioctl_sock, bss->ifname,
					 macaddr);
		if (!ret)
			ret = os_snprintf(buf, buf_len,
					  "Macaddr = " MACSTR "\n",
					  MAC2STR(macaddr));
	} else if (os_strcasecmp(cmd, "RELOAD") == 0) {
		nl80211_report_hang(drv);
	} else if (os_strncasecmp(cmd, "POWERMODE ", 10) == 0) {
		int state = atoi(cmd + 10);
		ret = wpa_driver_set_power_save(priv, state);
		if (ret < 0)
			wpa_driver_send_hang_msg(drv);
		else
			drv_errors = 0;

	} else if (os_strncasecmp(cmd, "COUNTRY ", 8) == 0) {
		wpa_driver_poll_country_change(drv->global, NULL);
		ret = 0;
	} else if (os_strncasecmp(cmd, "GETPOWER", 8) == 0) {
		int state = -1;
		ret = wpa_driver_get_power_save(priv, &state);
		if (!ret && (state != -1))
			ret = os_snprintf(buf, buf_len, "POWERMODE = %d\n",
					  state);
	} else { /* Use private command */
		memset(&ifr, 0, sizeof(ifr));
		memset(&priv_cmd, 0, sizeof(priv_cmd));
		os_memcpy(buf, cmd, strlen(cmd) + 1);
		os_strlcpy(ifr.ifr_name, bss->ifname, IFNAMSIZ);

		priv_cmd.buf = buf;
		priv_cmd.used_len = buf_len;
		priv_cmd.total_len = buf_len;
		ifr.ifr_data = &priv_cmd;

		if ((ret = ioctl(drv->global->ioctl_sock, SIOCDEVPRIVATE + 1,
				 &ifr)) < 0) {
			wpa_printf(MSG_DEBUG,
				   "%s: failed to issue private command %s\n",
				   __func__, cmd);

			/* wpa_driver_send_hang_msg(drv); */

			wpa_printf(MSG_DEBUG,
				   "%s: Skip this failure in current implementation \n",
				   __func__);
			drv_errors = 0;
			ret = 0;

			/* TODO: the below private comands issues by Android ICS
			 * framework are not handled in current implementation.
			 * One need to understand implications and decide how to
			 * handle them:
			 *
			 * BTCOEXSCAN-STOP
			 * BTCOEXMODE
			 * RXFILTER-ADD
			 * RXFILTER-START
			 * RXFILTER-STOP
			 * RXFILTER-REMOVE
			 * SCAN-ACTIVE
			 * SCAN-PASSIVE
			 * SETBAND
			 */
		} else {
			drv_errors = 0;
			ret = 0;
			if ((os_strcasecmp(cmd, "LINKSPEED") == 0) ||
			    (os_strcasecmp(cmd, "RSSI") == 0) ||
			    (os_strcasecmp(cmd, "GETBAND") == 0))
				ret = strlen(buf);

			wpa_printf(MSG_DEBUG, "%s %s len = %d, %d", __func__,
				   buf, ret, strlen(buf));
		}
	}

	return ret;
}


int wpa_driver_nl80211_get_p2p_noa(void *priv, u8 *buf, size_t len)
{
#define UNUSED(_x) (void)(_x)
	UNUSED(priv);
#undef UNUSED

	wpa_printf(MSG_DEBUG, "nl80211: get_p2p_noa is not supported");
	memset(buf, 0, len);
	return 0;
}


int wpa_driver_nl80211_set_ap_wps_p2p_ie(void *priv,
					 const struct wpabuf *beacon,
					 const struct wpabuf *proberesp,
					 const struct wpabuf *assocresp)
{
#define UNUSED(_x) (void)(_x)
	UNUSED(priv);
	UNUSED(beacon);
	UNUSED(proberesp);
	UNUSED(assocresp);
#undef UNUSED

	wpa_printf(MSG_DEBUG, "nl80211: set_ap_wps_p2p_ie is not supported");
	return 0;
}

