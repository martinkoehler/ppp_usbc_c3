/*
 * PPP-over-USB + WiFi SoftAP Router (ESP32-C3)
 *
 * AP/NVS interface for other modules.
 *
 * Author: Martin Köhler [martinkoehler]
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include "esp_err.h"
#include "esp_netif.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file ap_config.h
 * @brief Small AP interface exposed by app_main for web UI.
 *
 * The actual WiFi/NVS logic stays in ppp_usb_main.c,
 * but web_server.c needs to:
 *  - read SSID/pass
 *  - trigger apply+restart
 *  - access ap_netif for DHCP station listing
 */

/** Get current AP SSID string (NUL terminated). */
const char *ap_get_ssid(void);

/** Get current AP password string (NUL terminated). */
const char *ap_get_pass(void);

/** AP netif handle (for DHCP client lookup). */
esp_netif_t *ap_get_netif(void);

/**
 * @brief Save new AP credentials to NVS and restart AP.
 *
 * @param ssid New SSID (validated already)
 * @param pass New password (empty allowed, or >=8 chars)
 * @return ESP_OK on success
 */
esp_err_t ap_set_credentials_and_restart(const char *ssid, const char *pass);

/**
 * @brief Restart AP using current in-memory credentials.
 *
 * @return ESP_OK on success
 */
esp_err_t ap_restart(void);

#ifdef __cplusplus
}
#endif
