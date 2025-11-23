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

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file web_server.h
 * @brief HTTP server for AP configuration and router status.
 *
 * Responsibilities:
 *  - Start httpd on AP interface
 *  - Provide "/" status page with auto-refresh
 *  - Provide "/set" POST handler to change AP SSID/pass
 */

void web_server_start(void);

#ifdef __cplusplus
}
#endif

