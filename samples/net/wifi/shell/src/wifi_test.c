/*
 * Copyright (c) 2016 Intel Corporation.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <errno.h>
#include <zephyr/device.h>
#include <zephyr/net/wifi_mgmt.h>
#include <zephyr/logging/log.h>

#define WIFI_SSID     "ssid"
#define WIFI_PASS     "passwd"
#define WIFI_SECURITY 1

LOG_MODULE_REGISTER(wifi_connect, LOG_LEVEL_INF);

/* Function to connect to a Wi-Fi network */
int wifi_connect(struct net_if *iface, const char *ssid, const char *psk,
		 enum wifi_security_type security)
{
	struct wifi_connect_req_params params;
	int ret;

	/* Configure connection parameters */
	params.ssid = ssid;
	params.ssid_length = strlen(ssid);
	params.psk = psk;
	params.psk_length = strlen(psk);
	params.security = security;

	LOG_INF("Connecting to Wi-Fi network: SSID = %s", ssid);

	/* Initiate the Wi-Fi connection */
	ret = net_mgmt(NET_REQUEST_WIFI_CONNECT, iface, &params, sizeof(params));
	if (ret) {
		LOG_ERR("Wi-Fi connection failed (error code: %d)", ret);
		return ret;
	}

	LOG_INF("Wi-Fi connection successful!");
	return 0;
}

int main(void)
{
	const struct device *esp_dev = device_get_binding("wifi");
	struct net_if *iface;
	int ret;

	LOG_INF("Starting Wi-Fi with ESP8285 (esp_at)");

	iface = net_if_lookup_by_dev(esp_dev);
	if (!iface) {
		LOG_ERR("Unable to retrieve the wifi network interface.");
		return -1;
	}


	if (!device_is_ready(esp_dev)) {
		LOG_ERR("The Wi-Fi device is not ready.");
		return -1;
	} else {
		LOG_INF("The Wi-Fi device is ready.");
	}

	LOG_INF("Wi-Fi interface found: %s", esp_dev->name);

	/* Connect to Wi-Fi */
	ret = wifi_connect(iface, WIFI_SSID, WIFI_PASS, WIFI_SECURITY);
	if (ret) {
		LOG_ERR("Wi-Fi connection failed.");
		return -1;
	}

	return 0;
}