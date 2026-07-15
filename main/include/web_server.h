/*
 * PPP-over-USB + WiFi SoftAP Router (ESP32-C3)
 *
 * Web server module extracted from original monolithic source.
 *
 * Author: Martin Köhler [martinkoehler]
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file web_server.h
 * @brief HTTP server for AP configuration and router status.
 *
 * Responsibilities:
 *  - Start httpd on AP interface
 *  - Provide "/" status page
 *  - Provide "/set" POST handler to change AP SSID/pass
 */

void web_server_start(void);
void web_server_stop(void);
void web_server_restart(void);
bool web_server_health_check(void);
bool web_server_health_check_ex(int *status_out, esp_err_t *err_out);
void web_server_get_cached_health(int *status_out, esp_err_t *err_out,
                                  int64_t *checked_at_us_out);
bool web_server_is_running(void);
bool web_server_is_ota_in_progress(void);
int web_server_get_ota_progress(void);

#ifdef __cplusplus
}
#endif
