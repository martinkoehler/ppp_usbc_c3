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

#include "web_server.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_task_wdt.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "watchdog";
static TaskHandle_t watchdog_task_handle = NULL;
static uint32_t feed_period_ms = 5000;

#define WEB_HEALTH_FAIL_LIMIT 3

/**
 * @brief Watchdog feed and web-server health-check loop.
 *
 * The former client-ping check deliberately is not present here. A Wi-Fi
 * station may legitimately ignore ICMP while remaining associated, so a
 * failed ping must not be used as evidence that the SoftAP has failed.
 */
static void watchdog_task(void *arg)
{
    (void)arg;
    int web_fail_count = 0;

    ESP_LOGI(TAG, "Watchdog task started");

    while (1) {
        esp_err_t err = esp_task_wdt_reset();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to feed task watchdog: %s",
                     esp_err_to_name(err));
        }

        if (web_server_is_ota_in_progress()) {
            web_fail_count = 0;
        } else if (!web_server_health_check()) {
            web_fail_count++;
            ESP_LOGW(TAG, "Webserver health-check failure %d/%d",
                     web_fail_count, WEB_HEALTH_FAIL_LIMIT);

            if (web_fail_count >= WEB_HEALTH_FAIL_LIMIT) {
                ESP_LOGW(TAG, "Restarting webserver after repeated health-check failures");
                web_server_restart();
                web_fail_count = 0;
            }
        } else {
            web_fail_count = 0;
        }

        vTaskDelay(pdMS_TO_TICKS(feed_period_ms));
    }
}

void watchdog_start(uint32_t timeout_seconds, uint32_t period_ms)
{
    if (timeout_seconds == 0 || period_ms == 0) {
        ESP_LOGE(TAG, "Invalid watchdog configuration: timeout=%us period=%ums",
                 timeout_seconds, period_ms);
        return;
    }

    if (period_ms >= timeout_seconds * 1000U) {
        ESP_LOGE(TAG,
                 "Watchdog feed period (%ums) must be shorter than timeout (%ums)",
                 period_ms, timeout_seconds * 1000U);
        return;
    }

    if (watchdog_task_handle != NULL) {
        ESP_LOGW(TAG, "Watchdog task already running");
        return;
    }

    feed_period_ms = period_ms;

    esp_task_wdt_config_t config = {
        .timeout_ms = timeout_seconds * 1000U,
        .idle_core_mask = (1U << portGET_CORE_ID()),
        .trigger_panic = true,
    };

    esp_err_t err = esp_task_wdt_init(&config);
    if (err == ESP_ERR_INVALID_STATE) {
        /* ESP-IDF may already have initialized TWDT from sdkconfig. */
        err = esp_task_wdt_reconfigure(&config);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to configure task watchdog: %s",
                 esp_err_to_name(err));
        return;
    }

    BaseType_t rc = xTaskCreate(
        watchdog_task,
        "watchdog_task",
        3072,
        NULL,
        tskIDLE_PRIORITY + 1,
        &watchdog_task_handle
    );

    if (rc != pdPASS || watchdog_task_handle == NULL) {
        watchdog_task_handle = NULL;
        ESP_LOGE(TAG, "Failed to create watchdog task");
        return;
    }

    err = esp_task_wdt_add(watchdog_task_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to subscribe watchdog task: %s",
                 esp_err_to_name(err));
        vTaskDelete(watchdog_task_handle);
        watchdog_task_handle = NULL;
        return;
    }

    ESP_LOGI(TAG, "Watchdog started: timeout=%us feed_period=%ums",
             timeout_seconds, period_ms);
}

void watchdog_deinit(void)
{
    if (watchdog_task_handle != NULL) {
        esp_err_t err = esp_task_wdt_delete(watchdog_task_handle);
        if (err != ESP_OK && err != ESP_ERR_NOT_FOUND) {
            ESP_LOGW(TAG, "Failed to unsubscribe watchdog task: %s",
                     esp_err_to_name(err));
        }

        vTaskDelete(watchdog_task_handle);
        watchdog_task_handle = NULL;
    }

    esp_err_t err = esp_task_wdt_deinit();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "Failed to deinitialize task watchdog: %s",
                 esp_err_to_name(err));
    }

    ESP_LOGI(TAG, "Watchdog stopped");
}
