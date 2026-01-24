/*
 * PPP-over-USB + WiFi SoftAP Router (ESP32-C3)
 *
 * MQTT broker module extracted from original monolithic source.
 *
 * Author: Martin Köhler [martinkoehler]
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file mqtt_broker.h
 * @brief Embedded Mosquitto broker lifecycle + OBK power telemetry storage.
 *
 * Responsibilities:
 *  - Start Mosquitto broker on local AP / PPP interfaces.
 *  - Track broker running status.
 *  - Capture latest payload for OBK_POWER_TOPIC in a thread-safe way.
 *
 * Used by:
 *  - OLED module (display power)
 *  - Web server module (status page)
 *  - app_main (startup)
 */

/** Port the embedded Mosquitto broker listens on. */
#define MQTT_BROKER_PORT 1883

/** OBK base topic prefix. */
#define OBK_TOPIC_PREFIX "obk_wr"
/** OBK topic whose payload is displayed on OLED / web UI. */
#define OBK_POWER_TOPIC OBK_TOPIC_PREFIX "/power/get"
/** OBK topic indicating the power-source device connection state. */
#define OBK_CONNECTED_TOPIC OBK_TOPIC_PREFIX "/connected"
/** Treat power telemetry as stale after this many milliseconds without updates. */
#define OBK_POWER_STALE_TIMEOUT_MS 30000

/**
 * @brief Start MQTT broker in background task.
 * Safe to call multiple times.
 */
void mqtt_broker_start(void);

/**
 * @brief Is broker currently running?
 */
bool mqtt_broker_is_running(void);

/**
 * @brief Get latest OBK power payload.
 *
 * Thread-safe. Copies a NUL-terminated string into out.
 *
 * @param out Destination buffer (receives "N/A" if no data yet)
 * @param out_len Size of destination buffer
 */
void mqtt_broker_get_obk_power(char *out, size_t out_len);

/**
 * @brief Get last known OBK connection state.
 *
 * @return 1 for online, 0 for offline, -1 for unknown/not yet received.
 */
int mqtt_broker_get_obk_connected_state(void);

/**
 * @brief Initialize storage / mutex for telemetry.
 *
 * Must be called once before start if used.
 * app_main calls this early.
 */
void mqtt_broker_init_telemetry(void);

#ifdef __cplusplus
}
#endif
