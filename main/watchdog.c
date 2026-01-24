/*
 * Watchdog Utility for ESP32-C3 Projects
 *
 * Periodically feeds the ESP-IDF Task Watchdog (TWDT) from a dedicated FreeRTOS task.
 * If the system hangs and TWDT is not fed, an automatic chip reset is triggered.
 *
 * Author: Martin Köhler [martinkoehler]
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Copyright (C) 2025 Martin Köhler
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

/**
 * @file watchdog.c
 * @brief Task Watchdog management for ESP32-C3, with dedicated feeding loop.
 *
 * Usage:
 *   - Call watchdog_start(timeout_sec, feed_period_ms) in app_main.
 *   - Watchdog task will periodically call esp_task_wdt_reset().
 *   - If system deadlocks and watchdog isn't fed, ESP32-C3 will reset.
 *   - Stop using watchdog_deinit() if needed before shutdown/reset.
 */

#include "watchdog.h"
#include <string.h>
#include "ap_config.h"
#include "oled.h"
#include "mqtt_broker.h"
#include "esp_mac.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "ping/ping_sock.h"
#include "esp_task_wdt.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/ip_addr.h"
#include "lwip/ip4_addr.h"

static const char *TAG = "watchdog";
static TaskHandle_t watchdog_task_handle = NULL;
static uint32_t feed_period_ms = 10000; // Default feed period
#define AP_MAX_CONN 4

typedef struct {
    bool success;
    SemaphoreHandle_t done;
} ping_result_t;

static void ping_on_success(esp_ping_handle_t hdl, void *args)
{
    (void)hdl;
    ping_result_t *res = (ping_result_t *)args;
    res->success = true;
}

static void ping_on_end(esp_ping_handle_t hdl, void *args)
{
    (void)hdl;
    ping_result_t *res = (ping_result_t *)args;
    xSemaphoreGive(res->done);
}

static bool ping_client_ip(esp_ip4_addr_t ip)
{
    ping_result_t result = {
        .success = false,
        .done = xSemaphoreCreateBinary()
    };
    if (!result.done) return false;

    esp_ping_config_t ping_config = ESP_PING_DEFAULT_CONFIG();
    ip_addr_t target_addr;
    ip_addr_set_ip4_u32_val(target_addr, ip.addr);
    ping_config.target_addr = target_addr;
    ping_config.count = 1;
    ping_config.interval_ms = 100;
    ping_config.timeout_ms = 1000;
    ping_config.data_size = 32;

    esp_ping_callbacks_t callbacks = {
        .on_ping_success = ping_on_success,
        .on_ping_timeout = NULL,
        .on_ping_end = ping_on_end,
        .cb_args = &result
    };

    esp_ping_handle_t ping_handle = NULL;
    if (esp_ping_new_session(&ping_config, &callbacks, &ping_handle) != ESP_OK) {
        vSemaphoreDelete(result.done);
        return false;
    }

    esp_ping_start(ping_handle);
    xSemaphoreTake(result.done, pdMS_TO_TICKS(ping_config.timeout_ms + 200));
    esp_ping_stop(ping_handle);
    esp_ping_delete_session(ping_handle);
    vSemaphoreDelete(result.done);
    return result.success;
}

static bool ping_connected_clients(void)
{
    wifi_sta_list_t sta_list = {0};
    if (esp_wifi_ap_get_sta_list(&sta_list) != ESP_OK) {
        ESP_LOGW(TAG, "Failed to fetch STA list");
        return true;
    }

    if (sta_list.num == 0) return true;

    esp_netif_t *ap_netif = ap_get_netif();
    if (!ap_netif) {
        ESP_LOGW(TAG, "AP netif not ready for ping checks");
        return true;
    }

    esp_netif_pair_mac_ip_t pairs[AP_MAX_CONN];
    int n = sta_list.num;
    if (n > AP_MAX_CONN) n = AP_MAX_CONN;

    for (int i = 0; i < n; i++) {
        memcpy(pairs[i].mac, sta_list.sta[i].mac, 6);
        pairs[i].ip.addr = 0;
    }

    esp_err_t err = esp_netif_dhcps_get_clients_by_mac(ap_netif, n, pairs);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "DHCP client lookup failed: %s", esp_err_to_name(err));
        return true;
    }

    for (int i = 0; i < n; i++) {
        if (pairs[i].ip.addr == 0) {
            ESP_LOGW(TAG, "Skipping ping for client with no IP: " MACSTR, MAC2STR(pairs[i].mac));
            continue;
        }
        if (!ping_client_ip(pairs[i].ip)) {
            ESP_LOGW(TAG, "Client ping failed: " IPSTR, IP2STR(&pairs[i].ip));
            return false;
        }
    }

    return true;
}

/**
 * @brief Watchdog feed loop, running as a FreeRTOS task.
 *
 * This function is scheduled as a background task and periodically calls
 * esp_task_wdt_reset(). If this function is blocked for longer than the
 * configured timeout, the hardware watchdog will trigger a system reset.
 *
 * @param arg Unused parameter, required by FreeRTOS task signature.
 */
static void watchdog_task(void *arg)
{
    ESP_LOGI(TAG, "Watchdog task started");
    while (1) {
        esp_task_wdt_reset();
        if (mqtt_broker_get_obk_connected_state() != 0) {
            vTaskDelay(pdMS_TO_TICKS(feed_period_ms));
            continue;
        }
        if (!ping_connected_clients()) {
            ESP_LOGW(TAG, "Ping watchdog triggered, restarting AP");
            oled_blank_and_reset_screensaver();
            ap_restart();
        }
        vTaskDelay(pdMS_TO_TICKS(feed_period_ms));
    }
}

/**
 * @brief Start and configure the watchdog timer and its feeding task.
 *
 * Call this at application startup. Only one watchdog task will run.
 *
 * @param timeout_seconds Number of seconds before watchdog triggers reset if not fed.
 * @param period_ms Period in milliseconds for the feed loop.
 */
void watchdog_start(uint32_t timeout_seconds, uint32_t period_ms)
{
    feed_period_ms = period_ms;
    esp_task_wdt_config_t config = {
        .timeout_ms = timeout_seconds * 1000,
        .idle_core_mask = (1 << portGET_CORE_ID()),
        .trigger_panic = false,
    };
    esp_err_t err = esp_task_wdt_init(&config);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "Failed to initialize WDT: %s", esp_err_to_name(err));
        return;
    }
    esp_task_wdt_add(NULL); // Add current task (often app_main)

    if (watchdog_task_handle == NULL) {
        xTaskCreate(watchdog_task, "watchdog_task", 2048, NULL, tskIDLE_PRIORITY + 1, &watchdog_task_handle);
        esp_task_wdt_add(watchdog_task_handle);
    }
    ESP_LOGI(TAG, "Watchdog started: timeout=%us, feed_period=%ums", timeout_seconds, period_ms);
}

/**
 * @brief Deinitialize watchdog and remove its feed task.
 *
 * Call this before shutdown or to disable watchdogs.
 */
void watchdog_deinit(void)
{
    if (watchdog_task_handle) {
        vTaskDelete(watchdog_task_handle);
        esp_task_wdt_delete(watchdog_task_handle);
        watchdog_task_handle = NULL;
    }
    esp_task_wdt_delete(NULL);
    esp_task_wdt_deinit();
    ESP_LOGI(TAG, "Watchdog stopped");
}
