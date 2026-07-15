/*
 * PPP-over-USB + WiFi SoftAP Router (ESP32-C3)
 *
 * Client RSSI tracking module.
 *
 * Author: Martin Köhler [martinkoehler]
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "client_rssi.h"

#include <string.h>
#include <stdint.h>
#include <limits.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "esp_log.h"
#include "esp_mac.h"
#include "esp_wifi.h"

static const char *TAG = "client_rssi";

/* -------------------- Module state -------------------- */

#define MAX_STA_CLIENTS 4

typedef struct {
    uint8_t mac[6];
    int8_t rssi;
} sta_rssi_entry_t;

static sta_rssi_entry_t sta_rssi_list[MAX_STA_CLIENTS];
static uint8_t sta_count = 0;
static SemaphoreHandle_t rssi_mutex = NULL;

/* -------------------- RSSI Monitoring Task -------------------- */

static void rssi_update_task(void *arg)
{
    (void)arg;

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(5000)); // Update every 5 seconds

        wifi_sta_list_t sta_list = {0};
        if (esp_wifi_ap_get_sta_list(&sta_list) != ESP_OK) {
            continue;
        }

        if (xSemaphoreTake(rssi_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
            continue;
        }

        sta_count = sta_list.num;
        if (sta_count > MAX_STA_CLIENTS) {
            sta_count = MAX_STA_CLIENTS;
        }

        for (uint8_t i = 0; i < sta_count; i++) {
            memcpy(sta_rssi_list[i].mac, sta_list.sta[i].mac, 6);
            sta_rssi_list[i].rssi = sta_list.sta[i].rssi;

            ESP_LOGD(TAG, "STA[%d] MAC=" MACSTR " RSSI=%d dBm",
                     i, MAC2STR(sta_list.sta[i].mac), sta_list.sta[i].rssi);
        }

        xSemaphoreGive(rssi_mutex);
    }
}

/* -------------------- Public API -------------------- */

esp_err_t client_rssi_init(void)
{
    if (!rssi_mutex) {
        rssi_mutex = xSemaphoreCreateMutex();
        if (!rssi_mutex) {
            return ESP_ERR_NO_MEM;
        }
    }

    memset(sta_rssi_list, 0, sizeof(sta_rssi_list));
    sta_count = 0;

    if (xTaskCreate(rssi_update_task, "rssi_update",
                    4096, NULL, 6, NULL) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create RSSI update task");
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "Client RSSI tracking initialized");
    return ESP_OK;
}

int8_t client_rssi_get_by_mac(const uint8_t *mac)
{
    if (!mac) return INT8_MIN;

    int8_t rssi = INT8_MIN;

    if (xSemaphoreTake(rssi_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        for (uint8_t i = 0; i < sta_count; i++) {
            if (memcmp(sta_rssi_list[i].mac, mac, 6) == 0) {
                rssi = sta_rssi_list[i].rssi;
                break;
            }
        }
        xSemaphoreGive(rssi_mutex);
    }

    return rssi;
}

int8_t client_rssi_get_best(void)
{
    int8_t best_rssi = INT8_MIN;

    if (xSemaphoreTake(rssi_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        for (uint8_t i = 0; i < sta_count; i++) {
            if (sta_rssi_list[i].rssi > best_rssi) {
                best_rssi = sta_rssi_list[i].rssi;
            }
        }
        xSemaphoreGive(rssi_mutex);
    }

    return best_rssi;
}

int8_t client_rssi_get_average(void)
{
    int8_t avg_rssi = INT8_MIN;

    if (xSemaphoreTake(rssi_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        if (sta_count > 0) {
            int32_t sum = 0;
            for (uint8_t i = 0; i < sta_count; i++) {
                sum += sta_rssi_list[i].rssi;
            }
            avg_rssi = (int8_t)(sum / sta_count);
        }
        xSemaphoreGive(rssi_mutex);
    }

    return avg_rssi;
}

uint8_t client_rssi_to_bars(int8_t rssi)
{
    /* Convert RSSI (dBm) to 0-4 bars:
     * -30 to -50  = 4 bars (excellent)
     * -50 to -65  = 3 bars (good)
     * -65 to -75  = 2 bars (fair)
     * -75 to -85  = 1 bar  (poor)
     * -85 to -120 = 0 bars (very weak)
     */
    if (rssi > -50) return 4;
    if (rssi > -65) return 3;
    if (rssi > -75) return 2;
    if (rssi > -85) return 1;
    return 0;
}
