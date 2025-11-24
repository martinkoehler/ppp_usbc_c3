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
#include "esp_log.h"
#include "esp_task_wdt.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "watchdog";
static TaskHandle_t watchdog_task_handle = NULL;
static uint32_t feed_period_ms = 5000; // Default feed period

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
