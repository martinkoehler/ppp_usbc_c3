/*
 * PPP-over-USB + WiFi SoftAP Router (ESP32-C3)
 *
 * Client RSSI tracking module.
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
 * @file client_rssi.h
 * @brief Track WiFi signal strength (RSSI) of connected SoftAP clients.
 *
 * Responsibilities:
 *  - Monitor connected clients and their RSSI values
 *  - Provide lookup of RSSI by MAC address
 *  - Expose average/best client RSSI for OLED display
 */

/**
 * @brief Initialize client RSSI tracking.
 */
void client_rssi_init(void);

/**
 * @brief Get RSSI (signal strength) of a specific client by MAC address.
 * @param mac Pointer to 6-byte MAC address
 * @return RSSI in dBm (typically -40 to -100), or INT8_MIN if unknown
 */
int8_t client_rssi_get_by_mac(const uint8_t *mac);

/**
 * @brief Get the RSSI of the strongest connected client (power publisher).
 * @return RSSI in dBm, or INT8_MIN if no clients connected
 */
int8_t client_rssi_get_best(void);

/**
 * @brief Get average RSSI of all connected clients.
 * @return RSSI in dBm, or INT8_MIN if no clients connected
 */
int8_t client_rssi_get_average(void);

/**
 * @brief Convert RSSI (dBm) to WiFi signal strength bars (0-4).
 * @param rssi RSSI value in dBm (e.g., -40 = strong, -100 = weak)
 * @return 0-4 representing signal strength bars
 */
uint8_t client_rssi_to_bars(int8_t rssi);

#ifdef __cplusplus
}
#endif
