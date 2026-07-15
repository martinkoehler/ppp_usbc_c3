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

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

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

typedef struct {
    bool channel_auto;
    uint8_t active_channel;
    uint8_t manual_channel;
    bool scan_in_progress;
    esp_err_t last_scan_result;
    int64_t last_scan_time_us;
} ap_channel_status_t;

/** Copy a consistent snapshot of the current AP configuration. */
void ap_get_config_snapshot(char *ssid, size_t ssid_len,
                            char *pass, size_t pass_len,
                            ap_channel_status_t *channel_status);

/** AP netif handle (for DHCP client lookup). */
esp_netif_t *ap_get_netif(void);

/** Return true while the Wi-Fi SoftAP driver is running. */
bool ap_is_running(void);

/**
 * Return a compact SoftAP self-check code for the OLED debug page.
 *
 * R = ready, E = no AP_START event, M = wrong/unavailable Wi-Fi mode,
 * N = AP netif down, I = AP IP unavailable, C = AP config mismatch,
 * L = SoftAP control block unavailable.
 */
char ap_get_health_code(void);

/**
 * @brief Apply new AP credentials, persist them, or restore the old settings.
 *
 * @param ssid New SSID (validated already)
 * @param pass New password (empty allowed, or >=8 chars)
 * @param channel_auto Select the least congested channel automatically
 * @param manual_channel Manual 2.4 GHz channel, used when auto selection is off
 * @return ESP_OK on success
 */
esp_err_t ap_set_credentials_and_restart(const char *ssid, const char *pass,
                                         bool channel_auto,
                                         uint8_t manual_channel);

/**
 * @brief Restart AP using current in-memory credentials.
 *
 * @return ESP_OK on success
 */
esp_err_t ap_restart(void);

#ifdef __cplusplus
}
#endif
