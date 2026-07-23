/*
 * PPP-over-USB + WiFi SoftAP Router (ESP32-C3)
 *
 * FRITZ!Box MQTT telemetry client interface.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MQTT_TELEMETRY_PORT 1883
#define MQTT_BROKER_HOST_MAX_LEN 15
#define MQTT_ROOT_TOPIC_MAX_LEN 63
#define MQTT_DEFAULT_ROOT_TOPIC "OBK-681"
#define OBK_POWER_STALE_TIMEOUT_MS 30000

typedef struct {
    bool broker_auto;
    char broker_host[MQTT_BROKER_HOST_MAX_LEN + 1];
    char root_topic[MQTT_ROOT_TOPIC_MAX_LEN + 1];
} mqtt_telemetry_config_t;

/** Load persistent settings and start the MQTT client task. */
esp_err_t mqtt_telemetry_start(void);

/** True while the ESP-MQTT client is connected to the configured broker. */
bool mqtt_telemetry_is_broker_connected(void);

/** Copy the current persistent broker and root-topic settings. */
void mqtt_telemetry_get_config(mqtt_telemetry_config_t *out);

/** Resolve the active broker address. False means the PPP peer is unavailable. */
bool mqtt_telemetry_get_effective_broker_host(char *out, size_t out_len);

/**
 * Persist new settings and reconnect the client.
 * In automatic mode the PPP peer is used and broker_host may be empty. In
 * manual mode broker_host must be an IPv4 address. root_topic must be an exact
 * topic root without '/', whitespace, '+' or '#'.
 */
esp_err_t mqtt_telemetry_set_config(bool broker_auto, const char *broker_host,
                                    const char *root_topic);

/** Copy the latest fresh power payload, or "N/A" when unavailable/stale. */
void mqtt_telemetry_get_power(char *out, size_t out_len);

/**
 * Return the OBK connection state.
 *  1 = online, 0 = offline, -1 = no retained state yet,
 * -2 = configured MQTT broker is not connected.
 */
int mqtt_telemetry_get_obk_connected_state(void);

#ifdef __cplusplus
}
#endif
