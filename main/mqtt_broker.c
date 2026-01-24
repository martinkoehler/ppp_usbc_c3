/*
 * PPP-over-USB + WiFi SoftAP Router (ESP32-C3)
 *
 * Embedded MQTT broker module (Mosquitto port).
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

#include "mqtt_broker.h"
#include "ap_config.h"

#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <stdio.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "esp_log.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_event.h"
#include "mqtt_client.h"

#include "mosq_broker.h"

/* -------------------- Module state -------------------- */

static const char *TAG = "mqtt_broker";

static TaskHandle_t s_broker_task = NULL;
static bool s_broker_running = false;
static TaskHandle_t s_sub_task = NULL;
static esp_mqtt_client_handle_t s_sub_client = NULL;
static char s_sub_uri[64];
/* latest OBK power topic payload */
static char g_obk_power[64] = "N/A";
static int64_t g_obk_last_update_us = 0;
static SemaphoreHandle_t g_obk_mutex = NULL;
static int8_t g_obk_connected_state = -1;
static char s_in_topic[64];
static char s_in_payload[64];
static size_t s_in_len = 0;
static size_t s_in_total = 0;

static void trim_whitespace(char *s)
{
    char *start;
    char *end;
    size_t len;

    if (!s || !*s) return;

    start = s;
    while (*start && isspace((unsigned char)*start)) start++;
    end = start + strlen(start);
    while (end > start && isspace((unsigned char)end[-1])) end--;
    len = (size_t)(end - start);

    if (start != s && len > 0) memmove(s, start, len);
    s[len] = 0;
}

static void handle_obk_message(const char *topic, const char *data, int len)
{
    if (!topic || !data || len <= 0) return;

    if (strcmp(topic, OBK_POWER_TOPIC) == 0) {
        if (g_obk_mutex && xSemaphoreTake(g_obk_mutex, pdMS_TO_TICKS(5)) == pdTRUE) {
            int n = len;
            if (n >= (int)sizeof(g_obk_power)) n = sizeof(g_obk_power) - 1;
            memcpy(g_obk_power, data, n);
            g_obk_power[n] = 0;
            g_obk_last_update_us = esp_timer_get_time();
            xSemaphoreGive(g_obk_mutex);
        }
    } else if (strcmp(topic, OBK_CONNECTED_TOPIC) == 0) {
        if (g_obk_mutex && xSemaphoreTake(g_obk_mutex, pdMS_TO_TICKS(5)) == pdTRUE) {
            char tmp[16];
            int n = len;
            if (n >= (int)sizeof(tmp)) n = sizeof(tmp) - 1;
            memcpy(tmp, data, n);
            tmp[n] = 0;
            trim_whitespace(tmp);
            if (strcasecmp(tmp, "online") == 0) {
                g_obk_connected_state = 1;
            } else if (strcasecmp(tmp, "offline") == 0) {
                g_obk_connected_state = 0;
            }
            xSemaphoreGive(g_obk_mutex);
        }
    }
}

static bool get_ap_ip_str(char *out, size_t out_len)
{
    esp_netif_t *ap_netif = ap_get_netif();
    if (!ap_netif || !out || out_len == 0) return false;

    esp_netif_ip_info_t ip_info = {0};
    if (esp_netif_get_ip_info(ap_netif, &ip_info) != ESP_OK) return false;
    if (ip_info.ip.addr == 0) return false;

    esp_ip4addr_ntoa(&ip_info.ip, out, out_len);
    return true;
}

static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    (void)handler_args;
    (void)base;

    esp_mqtt_event_handle_t event = event_data;
    if (!event) return;

    switch (event_id) {
        case MQTT_EVENT_CONNECTED:
            esp_mqtt_client_subscribe(event->client, OBK_POWER_TOPIC, 0);
            esp_mqtt_client_subscribe(event->client, OBK_CONNECTED_TOPIC, 0);
            break;
        case MQTT_EVENT_DATA: {
            if (event->current_data_offset == 0) {
                size_t tlen = (event->topic_len < (int)(sizeof(s_in_topic) - 1))
                                  ? (size_t)event->topic_len
                                  : (sizeof(s_in_topic) - 1);
                memcpy(s_in_topic, event->topic, tlen);
                s_in_topic[tlen] = 0;
                s_in_len = 0;
                s_in_total = (size_t)event->total_data_len;
            }

            size_t copy = (size_t)event->data_len;
            if (s_in_len + copy >= sizeof(s_in_payload)) {
                if (s_in_len < sizeof(s_in_payload) - 1) {
                    copy = (sizeof(s_in_payload) - 1) - s_in_len;
                } else {
                    copy = 0;
                }
            }
            if (copy > 0) {
                memcpy(&s_in_payload[s_in_len], event->data, copy);
                s_in_len += copy;
            }

            if ((event->current_data_offset + event->data_len) >= event->total_data_len) {
                if (s_in_len < sizeof(s_in_payload)) {
                    s_in_payload[s_in_len] = 0;
                }
                handle_obk_message(s_in_topic, s_in_payload, (int)s_in_len);
            }
            break;
        }
        default:
            break;
    }
}

static void mqtt_sub_task(void *arg)
{
    (void)arg;
    while (1) {
        if (!s_sub_client) {
            char ip_str[16];
            if (!get_ap_ip_str(ip_str, sizeof(ip_str))) {
                vTaskDelay(pdMS_TO_TICKS(1000));
                continue;
            }
            snprintf(s_sub_uri, sizeof(s_sub_uri), "mqtt://%s:%d", ip_str, MQTT_BROKER_PORT);
            esp_mqtt_client_config_t cfg = {
                .broker.address.uri = s_sub_uri
            };
            s_sub_client = esp_mqtt_client_init(&cfg);
            if (s_sub_client) {
                esp_mqtt_client_register_event(s_sub_client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
                esp_mqtt_client_start(s_sub_client);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}

/* -------------------- Telemetry -------------------- */

void mqtt_broker_init_telemetry(void)
{
    if (!g_obk_mutex) {
        g_obk_mutex = xSemaphoreCreateMutex();
        configASSERT(g_obk_mutex);
    }
}

void mqtt_broker_get_obk_power(char *out, size_t out_len)
{
    if (!out || out_len == 0) return;

    strlcpy(out, "N/A", out_len);
    if (g_obk_mutex && xSemaphoreTake(g_obk_mutex, pdMS_TO_TICKS(5)) == pdTRUE) {
        int64_t now_us = esp_timer_get_time();
        int64_t age_us = (g_obk_last_update_us == 0) ? INT64_MAX : (now_us - g_obk_last_update_us);
        if (age_us <= (int64_t)OBK_POWER_STALE_TIMEOUT_MS * 1000) {
            strlcpy(out, g_obk_power, out_len);
        }
        xSemaphoreGive(g_obk_mutex);
    }
}

int mqtt_broker_get_obk_connected_state(void)
{
    int state = -1;
    if (g_obk_mutex && xSemaphoreTake(g_obk_mutex, pdMS_TO_TICKS(5)) == pdTRUE) {
        state = g_obk_connected_state;
        xSemaphoreGive(g_obk_mutex);
    }
    return state;
}

/* -------------------- Broker task -------------------- */

static void mqtt_broker_task(void *arg)
{
    (void)arg;

    struct mosq_broker_config cfg = {
        .host = "0.0.0.0",
        .port = MQTT_BROKER_PORT,
        .tls_cfg = NULL,
        .handle_message_cb = NULL
    };

    ESP_LOGI(TAG, "Mosquitto broker starting on port %d (host=%s)...",
             cfg.port, cfg.host);

    s_broker_running = true;
    int rc = mosq_broker_run(&cfg);
    s_broker_running = false;

    ESP_LOGW(TAG, "Mosquitto broker exited rc=%d", rc);
    s_broker_task = NULL;
    vTaskDelete(NULL);
}

void mqtt_broker_start(void)
{
    if (s_broker_task || s_broker_running) {
        ESP_LOGI(TAG, "Broker already running");
        return;
    }

    mqtt_broker_init_telemetry();

    xTaskCreate(mqtt_broker_task, "mosq_broker",
                8192, NULL, 8, &s_broker_task);
    if (!s_sub_task) {
        xTaskCreate(mqtt_sub_task, "mqtt_sub",
                    4096, NULL, 7, &s_sub_task);
    }
}

bool mqtt_broker_is_running(void)
{
    return s_broker_running;
}
