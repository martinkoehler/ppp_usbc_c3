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

#include <string.h>
#include <stdio.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"

#include "mosq_broker.h"

/* -------------------- Module state -------------------- */

static const char *TAG = "mqtt_broker";

static TaskHandle_t s_broker_task = NULL;
static bool s_broker_running = false;

/* latest OBK power topic payload */
static char g_obk_power[64] = "N/A";
static int64_t g_obk_last_update_us = 0;
static SemaphoreHandle_t g_obk_mutex = NULL;

/* -------------------- Telemetry -------------------- */

void mqtt_broker_init_telemetry(void)
{
    if (!g_obk_mutex) {
        g_obk_mutex = xSemaphoreCreateMutex();
        configASSERT(g_obk_mutex);
    }
}

/**
 * @brief Called for every MQTT message brokered by Mosquitto.
 * Captures OBK_POWER_TOPIC into g_obk_power.
 */
static void broker_message_cb(char *client, char *topic, char *data,
                              int len, int qos, int retain)
{
    (void)client; (void)qos; (void)retain;

    if (!topic || !data || len <= 0) return;

    if (strcmp(topic, OBK_POWER_TOPIC) == 0) {
        if (g_obk_mutex && xSemaphoreTake(g_obk_mutex, 0) == pdTRUE) {
            int n = len;
            if (n >= (int)sizeof(g_obk_power)) n = sizeof(g_obk_power) - 1;
            memcpy(g_obk_power, data, n);
            g_obk_power[n] = 0;
            g_obk_last_update_us = esp_timer_get_time();
            xSemaphoreGive(g_obk_mutex);
        }
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

/* -------------------- Broker task -------------------- */

static void mqtt_broker_task(void *arg)
{
    (void)arg;

    struct mosq_broker_config cfg = {
        .host = "0.0.0.0",
        .port = MQTT_BROKER_PORT,
        .tls_cfg = NULL,
        .handle_message_cb = broker_message_cb
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
}

bool mqtt_broker_is_running(void)
{
    return s_broker_running;
}
