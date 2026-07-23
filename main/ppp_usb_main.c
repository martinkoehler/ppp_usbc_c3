/*
 * PPP-over-USB + WiFi SoftAP Router (ESP32-C3)
 *
 * App glue / startup file after modular refactor.
 *
 * Keeps:
 *  - NVS storage for AP creds
 *  - WiFi SoftAP setup + static IP config
 *  - Exposed AP interface for web server
 *  - Starts modules (PPP, web server, MQTT telemetry, OLED)
 *
 * Author: Martin Köhler [martinkoehler]
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <string.h>
#include <ctype.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "nvs.h"

#include "esp_wifi.h"
#include "esp_mac.h"
#include "lwip/ip4_addr.h"

#include "ap_config.h"
#include "ppp.h"
#include "web_server.h"
#include "mqtt_telemetry.h"
#include "oled.h"
#include "watchdog.h"
#include "client_rssi.h"

/* ------------------------- AP defaults ------------------------- */
#define DEFAULT_AP_SSID     "ESP32C3-PPP-AP"
#define DEFAULT_AP_PASS     "12345678"
#define DEFAULT_AP_CHANNEL  11
#define DEFAULT_CHANNEL_AUTO true
#define AP_MAX_CONN         4
#define AP_MIN_CHANNEL      1
#define AP_MAX_CHANNEL      11
#define AP_COUNTRY_CODE     "US"
#define AP_COUNTRY_SCHAN    1
#define AP_COUNTRY_NCHAN    11
#define AP_MAX_TX_POWER_QDBM 80
#define AUTO_SCAN_INTERVAL_US (6LL * 60LL * 60LL * 1000000LL)
#define AUTO_SCAN_IDLE_US     (5LL * 60LL * 1000000LL)
#define AUTO_INITIAL_SCAN_IDLE_US (60LL * 1000000LL)
#define AUTO_SCAN_POLL_MS     (60 * 1000)

#define AP_IP_ADDR     "192.168.4.1"
#define AP_GATEWAY     "192.168.4.1"
#define AP_NETMASK     "255.255.255.0"

/* NVS keys/namespace */
#define NVS_NS   "apcfg"
#define NVS_KEY_SSID "ssid"
#define NVS_KEY_PASS "pass"
#define NVS_KEY_CHANNEL "channel"
#define NVS_KEY_CHANNEL_AUTO "auto_chan"
#define NVS_KEY_LAST_AUTO_CHANNEL "last_auto"

/* ------------------------- module state ------------------------- */
static const char *TAG = "ppp_usb_ap_web";

static char g_ap_ssid[33] = DEFAULT_AP_SSID;
static char g_ap_pass[65] = DEFAULT_AP_PASS;
static uint8_t g_ap_channel = DEFAULT_AP_CHANNEL;
static uint8_t g_manual_channel = DEFAULT_AP_CHANNEL;
static uint8_t g_last_auto_channel = DEFAULT_AP_CHANNEL;
static bool g_channel_auto = DEFAULT_CHANNEL_AUTO;
static bool g_scan_in_progress = false;
static esp_err_t g_last_scan_result = ESP_ERR_INVALID_STATE;
static int64_t g_last_scan_time_us = 0;
static int64_t g_last_client_activity_us = 0;
static esp_netif_t *ap_netif = NULL;
static esp_netif_t *sta_netif = NULL;
static bool ap_started = false;
static SemaphoreHandle_t ap_config_mutex = NULL;
static portMUX_TYPE ap_state_lock = portMUX_INITIALIZER_UNLOCKED;

static esp_err_t save_ap_config_to_nvs(const char *ssid, const char *pass,
                                       bool channel_auto,
                                       uint8_t manual_channel,
                                       uint8_t active_channel);

static uint8_t sanitize_ap_channel(uint8_t channel)
{
    if (channel < AP_MIN_CHANNEL || channel > AP_MAX_CHANNEL) {
        return DEFAULT_AP_CHANNEL;
    }
    return channel;
}

/* =========================================================================
 * NVS Flash Storage - Load & Save AP config
 * ========================================================================= */

static void load_ap_config_from_nvs(void)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(NVS_NS, NVS_READONLY, &nvs);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "No NVS namespace yet, using defaults");
        return;
    }

    size_t ssid_len = sizeof(g_ap_ssid);
    size_t pass_len = sizeof(g_ap_pass);

    err = nvs_get_str(nvs, NVS_KEY_SSID, g_ap_ssid, &ssid_len);
    if (err != ESP_OK) {
        strncpy(g_ap_ssid, DEFAULT_AP_SSID, sizeof(g_ap_ssid));
    }

    err = nvs_get_str(nvs, NVS_KEY_PASS, g_ap_pass, &pass_len);
    if (err != ESP_OK) {
        strncpy(g_ap_pass, DEFAULT_AP_PASS, sizeof(g_ap_pass));
    }

    uint8_t channel = DEFAULT_AP_CHANNEL;
    err = nvs_get_u8(nvs, NVS_KEY_CHANNEL, &channel);
    if (err == ESP_OK) {
        g_manual_channel = sanitize_ap_channel(channel);
    }

    uint8_t channel_auto = DEFAULT_CHANNEL_AUTO ? 1 : 0;
    if (nvs_get_u8(nvs, NVS_KEY_CHANNEL_AUTO, &channel_auto) == ESP_OK) {
        g_channel_auto = channel_auto != 0;
    }

    uint8_t last_auto_channel = DEFAULT_AP_CHANNEL;
    if (nvs_get_u8(nvs, NVS_KEY_LAST_AUTO_CHANNEL,
                   &last_auto_channel) == ESP_OK) {
        last_auto_channel = sanitize_ap_channel(last_auto_channel);
    }
    g_last_auto_channel = last_auto_channel;
    g_ap_channel = g_channel_auto ? g_last_auto_channel : g_manual_channel;

    nvs_close(nvs);

    ESP_LOGI(TAG, "Loaded AP config: SSID='%s' PASS len=%d mode=%s CH=%u manual=%u",
             g_ap_ssid, (int)strlen(g_ap_pass),
             g_channel_auto ? "auto" : "manual", g_ap_channel,
             g_manual_channel);
}

static esp_err_t save_ap_config_to_nvs(const char *ssid, const char *pass,
                                       bool channel_auto,
                                       uint8_t manual_channel,
                                       uint8_t active_channel)
{
    nvs_handle_t nvs;
    manual_channel = sanitize_ap_channel(manual_channel);
    active_channel = sanitize_ap_channel(active_channel);
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &nvs);
    if (err != ESP_OK) return err;

    err = nvs_set_str(nvs, NVS_KEY_SSID, ssid);
    if (err != ESP_OK) { nvs_close(nvs); return err; }

    err = nvs_set_str(nvs, NVS_KEY_PASS, pass);
    if (err != ESP_OK) { nvs_close(nvs); return err; }

    err = nvs_set_u8(nvs, NVS_KEY_CHANNEL, manual_channel);
    if (err != ESP_OK) { nvs_close(nvs); return err; }

    err = nvs_set_u8(nvs, NVS_KEY_CHANNEL_AUTO, channel_auto ? 1 : 0);
    if (err != ESP_OK) { nvs_close(nvs); return err; }

    err = nvs_set_u8(nvs, NVS_KEY_LAST_AUTO_CHANNEL,
                     channel_auto ? active_channel : g_last_auto_channel);
    if (err != ESP_OK) { nvs_close(nvs); return err; }

    err = nvs_commit(nvs);
    nvs_close(nvs);
    if (err == ESP_OK && channel_auto) {
        g_last_auto_channel = active_channel;
    }
    return err;
}

static uint32_t rssi_weight(int8_t rssi)
{
    if (rssi >= -50) return 100;
    if (rssi >= -60) return 50;
    if (rssi >= -70) return 20;
    if (rssi >= -80) return 5;
    return 1;
}

static uint32_t overlap_weight(uint8_t candidate, uint8_t primary)
{
    unsigned distance = candidate > primary ? candidate - primary
                                             : primary - candidate;
    static const uint8_t overlap[] = {100, 75, 50, 25, 10};
    return distance < sizeof(overlap) ? overlap[distance] : 0;
}

static uint8_t choose_best_channel(const wifi_ap_record_t *records,
                                   uint16_t record_count,
                                   bool use_hysteresis)
{
    static const uint8_t candidates[] = {1, 6, 11};
    uint32_t scores[sizeof(candidates)] = {0};

    for (uint16_t i = 0; i < record_count; i++) {
        if (records[i].primary < AP_MIN_CHANNEL ||
            records[i].primary > AP_MAX_CHANNEL) {
            continue;
        }
        uint32_t signal = rssi_weight(records[i].rssi);
        for (size_t c = 0; c < sizeof(candidates); c++) {
            scores[c] += signal * overlap_weight(candidates[c],
                                                  records[i].primary);
        }
    }

    size_t best = 0;
    for (size_t c = 1; c < sizeof(candidates); c++) {
        if (scores[c] < scores[best] ||
            (scores[c] == scores[best] && candidates[c] == g_ap_channel)) {
            best = c;
        }
    }

    if (use_hysteresis) {
        for (size_t c = 0; c < sizeof(candidates); c++) {
            if (candidates[c] != g_ap_channel) continue;
            /* Require a score at least 25 percent better before disrupting AP. */
            if (scores[c] == 0 || scores[best] * 4U > scores[c] * 3U) {
                best = c;
            }
            break;
        }
    }

    ESP_LOGI(TAG, "Channel scores: 1=%lu 6=%lu 11=%lu; selected %u",
             (unsigned long)scores[0], (unsigned long)scores[1],
             (unsigned long)scores[2], candidates[best]);
    return candidates[best];
}

/* Caller serializes this with ap_config_mutex. Wi-Fi must already be started. */
static esp_err_t scan_and_select_channel(bool use_hysteresis)
{
    uint8_t original_channel = g_ap_channel;
    if (!sta_netif) {
        g_last_scan_result = ESP_ERR_INVALID_STATE;
        g_last_scan_time_us = esp_timer_get_time();
        return g_last_scan_result;
    }
    wifi_mode_t original_mode;
    esp_err_t err = esp_wifi_get_mode(&original_mode);
    if (err != ESP_OK) return err;

    g_scan_in_progress = true;
    wifi_mode_t scan_mode = original_mode == WIFI_MODE_AP
                                ? WIFI_MODE_APSTA : original_mode;
    if (scan_mode != original_mode) {
        err = esp_wifi_set_mode(scan_mode);
        if (err != ESP_OK) goto done;
    }

    /* Defaults are deliberately used here: they are validated by the driver
     * for both STA startup scans and APSTA background scans. */
    err = esp_wifi_scan_start(NULL, true);
    if (err != ESP_OK) goto restore_mode;

    uint16_t ap_count = 0;
    err = esp_wifi_scan_get_ap_num(&ap_count);
    if (err != ESP_OK) goto clear_scan;

    wifi_ap_record_t *records = NULL;
    if (ap_count > 0) {
        records = calloc(ap_count, sizeof(*records));
        if (!records) {
            err = ESP_ERR_NO_MEM;
            goto clear_scan;
        }
        uint16_t fetched = ap_count;
        err = esp_wifi_scan_get_ap_records(&fetched, records);
        if (err == ESP_OK) {
            g_ap_channel = choose_best_channel(records, fetched,
                                                use_hysteresis);
        }
        free(records);
    } else {
        /* With no visible APs, keep an already non-overlapping channel. */
        g_ap_channel = choose_best_channel(NULL, 0, use_hysteresis);
    }
    goto restore_mode;

clear_scan:
    esp_wifi_clear_ap_list();
restore_mode:
    if (scan_mode != original_mode) {
        esp_err_t restore_err = esp_wifi_set_mode(original_mode);
        if (err == ESP_OK) err = restore_err;
    }
done:
    if (err != ESP_OK) {
        g_ap_channel = original_channel;
    }
    g_last_scan_result = err;
    g_last_scan_time_us = esp_timer_get_time();
    g_scan_in_progress = false;
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Automatic channel scan failed: %s",
                 esp_err_to_name(err));
    }
    return err;
}

static esp_err_t apply_softap_runtime_settings(void)
{
    wifi_country_t country = {
        .cc = AP_COUNTRY_CODE,
        .schan = AP_COUNTRY_SCHAN,
        .nchan = AP_COUNTRY_NCHAN,
        .policy = WIFI_COUNTRY_POLICY_MANUAL,
    };

    esp_err_t err = esp_wifi_set_country(&country);
    if (err != ESP_OK) {
        return err;
    }

    err = esp_wifi_set_bandwidth(WIFI_IF_AP, WIFI_BW_HT20);
    if (err != ESP_OK) {
        return err;
    }

    err = esp_wifi_set_protocol(WIFI_IF_AP, WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N);
    if (err != ESP_OK) {
        return err;
    }
    return esp_wifi_set_max_tx_power(AP_MAX_TX_POWER_QDBM);
}

/* =========================================================================
 * WiFi SoftAP Handling
 * ========================================================================= */

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    (void)arg;

    if (event_base != WIFI_EVENT) {
        return;
    }

    switch (event_id) {
        case WIFI_EVENT_AP_START:
            portENTER_CRITICAL(&ap_state_lock);
            ap_started = true;
            portEXIT_CRITICAL(&ap_state_lock);
            ESP_LOGI(TAG, "WiFi SoftAP driver started");
            break;

        case WIFI_EVENT_AP_STOP:
            portENTER_CRITICAL(&ap_state_lock);
            ap_started = false;
            portEXIT_CRITICAL(&ap_state_lock);
            ESP_LOGW(TAG, "WiFi SoftAP driver stopped");
            break;

        case WIFI_EVENT_AP_STACONNECTED: {
            wifi_event_ap_staconnected_t *e =
                (wifi_event_ap_staconnected_t *)event_data;
            ESP_LOGI(TAG, "STA joined: " MACSTR " AID=%d",
                     MAC2STR(e->mac), e->aid);
            portENTER_CRITICAL(&ap_state_lock);
            g_last_client_activity_us = esp_timer_get_time();
            portEXIT_CRITICAL(&ap_state_lock);
            break;
        }

        case WIFI_EVENT_AP_STADISCONNECTED: {
            wifi_event_ap_stadisconnected_t *e =
                (wifi_event_ap_stadisconnected_t *)event_data;
            ESP_LOGI(TAG, "STA left: " MACSTR " AID=%d",
                     MAC2STR(e->mac), e->aid);
            portENTER_CRITICAL(&ap_state_lock);
            g_last_client_activity_us = esp_timer_get_time();
            portEXIT_CRITICAL(&ap_state_lock);
            break;
        }

        default:
            break;
    }
}

static void fill_softap_config(wifi_config_t *wifi_config)
{
    memset(wifi_config, 0, sizeof(*wifi_config));

    wifi_config->ap.channel = g_ap_channel;
    wifi_config->ap.max_connection = AP_MAX_CONN;
    wifi_config->ap.authmode = strlen(g_ap_pass) == 0
                                   ? WIFI_AUTH_OPEN
                                   : WIFI_AUTH_WPA2_PSK;
    wifi_config->ap.pmf_cfg.required = false;
    wifi_config->ap.beacon_interval = 100;
    wifi_config->ap.dtim_period = 1;

    wifi_config->ap.ssid_len = strlen(g_ap_ssid);
    memcpy(wifi_config->ap.ssid, g_ap_ssid, wifi_config->ap.ssid_len);

    memcpy(wifi_config->ap.password, g_ap_pass, strlen(g_ap_pass));
}

static esp_err_t apply_ap_config_and_restart(void)
{
    wifi_config_t wifi_config;
    fill_softap_config(&wifi_config);

    ESP_LOGW(TAG, "Restarting SoftAP. SSID='%s' CH=%u",
             g_ap_ssid, g_ap_channel);

    esp_err_t err = esp_wifi_stop();
    if (err != ESP_OK && err != ESP_ERR_WIFI_NOT_STARTED) {
        ESP_LOGE(TAG, "esp_wifi_stop failed: %s", esp_err_to_name(err));
        return err;
    }

    vTaskDelay(pdMS_TO_TICKS(250));

    err = esp_wifi_set_mode(WIFI_MODE_AP);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_set_mode failed: %s", esp_err_to_name(err));
        return err;
    }

    err = esp_wifi_set_config(WIFI_IF_AP, &wifi_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_set_config failed: %s", esp_err_to_name(err));
        return err;
    }

    err = esp_wifi_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_start failed: %s", esp_err_to_name(err));
        return err;
    }

    err = apply_softap_runtime_settings();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SoftAP runtime settings failed: %s",
                 esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "SoftAP restart completed");
    return ESP_OK;
}

static void wifi_init_softap(void)
{
    ESP_LOGI(TAG, "Initializing WiFi SoftAP...");

    ap_netif = esp_netif_create_default_wifi_ap();
    assert(ap_netif);

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));

    /* Static IP for AP */
    ESP_ERROR_CHECK(esp_netif_dhcps_stop(ap_netif));
    esp_netif_ip_info_t ip_info = {0};
    ip4_addr_t tmp;

    ip4addr_aton(AP_IP_ADDR, &tmp); ip_info.ip.addr = tmp.addr;
    ip4addr_aton(AP_GATEWAY, &tmp); ip_info.gw.addr = tmp.addr;
    ip4addr_aton(AP_NETMASK, &tmp); ip_info.netmask.addr = tmp.addr;

    ESP_ERROR_CHECK(esp_netif_set_ip_info(ap_netif, &ip_info));
    ESP_ERROR_CHECK(esp_netif_dhcps_start(ap_netif));

    wifi_config_t wifi_config;
    fill_softap_config(&wifi_config);

    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(apply_softap_runtime_settings());

    ESP_LOGI(TAG, "SoftAP up. SSID=%s CH=%u IP=%s", g_ap_ssid, g_ap_channel, AP_IP_ADDR);

    /* Scanning needs a station netif, but failure to allocate this optional
     * interface must never take down the already-running SoftAP. */
    sta_netif = esp_netif_create_default_wifi_sta();
    if (!sta_netif) {
        ESP_LOGW(TAG, "STA netif unavailable; automatic scans disabled");
    }

    portENTER_CRITICAL(&ap_state_lock);
    g_last_client_activity_us = esp_timer_get_time();
    portEXIT_CRITICAL(&ap_state_lock);
}

/* =========================================================================
 * ap_config.h interface (used by web_server.c)
 * ========================================================================= */

void ap_get_config_snapshot(char *ssid, size_t ssid_len,
                            char *pass, size_t pass_len,
                            ap_channel_status_t *channel_status)
{
    if (ap_config_mutex) {
        xSemaphoreTake(ap_config_mutex, portMAX_DELAY);
    }
    if (ssid && ssid_len > 0) strlcpy(ssid, g_ap_ssid, ssid_len);
    if (pass && pass_len > 0) strlcpy(pass, g_ap_pass, pass_len);
    if (channel_status) {
        uint8_t actual_channel = g_ap_channel;
        wifi_second_chan_t second_channel;
        if (esp_wifi_get_channel(&actual_channel, &second_channel) != ESP_OK) {
            actual_channel = g_ap_channel;
        }
        *channel_status = (ap_channel_status_t) {
            .channel_auto = g_channel_auto,
            .active_channel = actual_channel,
            .manual_channel = g_manual_channel,
            .scan_in_progress = g_scan_in_progress,
            .last_scan_result = g_last_scan_result,
            .last_scan_time_us = g_last_scan_time_us,
        };
    }
    if (ap_config_mutex) {
        xSemaphoreGive(ap_config_mutex);
    }
}
esp_netif_t *ap_get_netif(void) { return ap_netif; }
bool ap_is_running(void)
{
    bool started;
    portENTER_CRITICAL(&ap_state_lock);
    started = ap_started;
    portEXIT_CRITICAL(&ap_state_lock);
    return started;
}

char ap_get_health_code(void)
{
    if (!ap_is_running()) {
        return 'E';
    }

    wifi_mode_t mode;
    if (esp_wifi_get_mode(&mode) != ESP_OK ||
        (mode != WIFI_MODE_AP && mode != WIFI_MODE_APSTA)) {
        return 'M';
    }

    if (ap_netif == NULL || !esp_netif_is_netif_up(ap_netif)) {
        return 'N';
    }

    esp_netif_ip_info_t ip_info;
    if (esp_netif_get_ip_info(ap_netif, &ip_info) != ESP_OK ||
        ip_info.ip.addr == 0) {
        return 'I';
    }

    char expected_ssid[sizeof(g_ap_ssid)];
    ap_channel_status_t channel_status;
    ap_get_config_snapshot(expected_ssid, sizeof(expected_ssid),
                           NULL, 0, &channel_status);

    wifi_config_t config;
    if (esp_wifi_get_config(WIFI_IF_AP, &config) != ESP_OK ||
        config.ap.channel != channel_status.active_channel ||
        config.ap.ssid_len != strlen(expected_ssid) ||
        memcmp(config.ap.ssid, expected_ssid, config.ap.ssid_len) != 0) {
        return 'C';
    }

    wifi_sta_list_t sta_list = {0};
    if (esp_wifi_ap_get_sta_list(&sta_list) != ESP_OK) {
        return 'L';
    }

    return 'R';
}

esp_err_t ap_set_credentials_and_restart(const char *ssid, const char *pass,
                                         bool channel_auto,
                                         uint8_t manual_channel)
{
    if (!ssid || !pass || ssid[0] == 0 || strlen(ssid) > 32 ||
        manual_channel < AP_MIN_CHANNEL || manual_channel > AP_MAX_CHANNEL) {
        return ESP_ERR_INVALID_ARG;
    }
    size_t pass_len = strlen(pass);
    if (pass_len != 0 && (pass_len < 8 || pass_len > 64)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (pass_len == 64) {
        for (size_t i = 0; i < pass_len; i++) {
            if (!isxdigit((unsigned char)pass[i])) {
                return ESP_ERR_INVALID_ARG;
            }
        }
    }

    xSemaphoreTake(ap_config_mutex, portMAX_DELAY);

    char old_ssid[sizeof(g_ap_ssid)];
    char old_pass[sizeof(g_ap_pass)];
    uint8_t old_channel = g_ap_channel;
    uint8_t old_manual_channel = g_manual_channel;
    bool old_channel_auto = g_channel_auto;
    strlcpy(old_ssid, g_ap_ssid, sizeof(old_ssid));
    strlcpy(old_pass, g_ap_pass, sizeof(old_pass));

    strlcpy(g_ap_ssid, ssid, sizeof(g_ap_ssid));
    strlcpy(g_ap_pass, pass, sizeof(g_ap_pass));
    g_channel_auto = channel_auto;
    g_manual_channel = manual_channel;
    if (g_channel_auto) {
        scan_and_select_channel(false);
    } else {
        g_ap_channel = g_manual_channel;
    }

    bool save_attempted = false;
    esp_err_t err = apply_ap_config_and_restart();
    if (err == ESP_OK) {
        save_attempted = true;
        err = save_ap_config_to_nvs(g_ap_ssid, g_ap_pass,
                                    g_channel_auto, g_manual_channel,
                                    g_ap_channel);
    }
    if (err == ESP_OK) {
        xSemaphoreGive(ap_config_mutex);
        return ESP_OK;
    }

    ESP_LOGE(TAG, "AP configuration update failed, restoring previous settings: %s",
             esp_err_to_name(err));
    strlcpy(g_ap_ssid, old_ssid, sizeof(g_ap_ssid));
    strlcpy(g_ap_pass, old_pass, sizeof(g_ap_pass));
    g_ap_channel = old_channel;
    g_manual_channel = old_manual_channel;
    g_channel_auto = old_channel_auto;
    esp_err_t rollback_err = apply_ap_config_and_restart();
    if (rollback_err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to restore previous AP settings: %s",
                 esp_err_to_name(rollback_err));
    }
    if (save_attempted) {
        esp_err_t restore_nvs_err = save_ap_config_to_nvs(
            old_ssid, old_pass, old_channel_auto, old_manual_channel,
            old_channel);
        if (restore_nvs_err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to restore previous AP settings in NVS: %s",
                     esp_err_to_name(restore_nvs_err));
        }
    }
    xSemaphoreGive(ap_config_mutex);
    return err;
}

esp_err_t ap_restart(void)
{
    xSemaphoreTake(ap_config_mutex, portMAX_DELAY);
    esp_err_t err = apply_ap_config_and_restart();
    xSemaphoreGive(ap_config_mutex);
    return err;
}

static void channel_rescan_task(void *arg)
{
    (void)arg;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(AUTO_SCAN_POLL_MS));

        wifi_sta_list_t stations = {0};
        int64_t now = esp_timer_get_time();
        if (esp_wifi_ap_get_sta_list(&stations) != ESP_OK) continue;
        if (stations.num > 0) {
            portENTER_CRITICAL(&ap_state_lock);
            g_last_client_activity_us = now;
            portEXIT_CRITICAL(&ap_state_lock);
            continue;
        }

        int64_t last_client_activity;
        portENTER_CRITICAL(&ap_state_lock);
        last_client_activity = g_last_client_activity_us;
        portEXIT_CRITICAL(&ap_state_lock);

        xSemaphoreTake(ap_config_mutex, portMAX_DELAY);
        bool initial_scan_due = g_last_scan_time_us == 0 &&
            now - last_client_activity >= AUTO_INITIAL_SCAN_IDLE_US;
        bool periodic_scan_due = g_last_scan_time_us > 0 &&
            now - g_last_scan_time_us >= AUTO_SCAN_INTERVAL_US &&
            now - last_client_activity >= AUTO_SCAN_IDLE_US;
        bool due = g_channel_auto && !g_scan_in_progress &&
                   (initial_scan_due || periodic_scan_due);
        if (!due || web_server_is_ota_in_progress()) {
            xSemaphoreGive(ap_config_mutex);
            continue;
        }

        /* Close the race with a client joining while the mutex was acquired. */
        memset(&stations, 0, sizeof(stations));
        if (esp_wifi_ap_get_sta_list(&stations) != ESP_OK || stations.num > 0) {
            xSemaphoreGive(ap_config_mutex);
            continue;
        }

        uint8_t old_channel = g_ap_channel;
        esp_err_t err = scan_and_select_channel(g_last_scan_time_us != 0);
        if (err == ESP_OK && g_ap_channel != old_channel) {
            ESP_LOGW(TAG, "Idle automatic channel change %u -> %u",
                     old_channel, g_ap_channel);
            err = apply_ap_config_and_restart();
            if (err != ESP_OK) {
                g_ap_channel = old_channel;
                apply_ap_config_and_restart();
            } else {
                esp_err_t save_err = save_ap_config_to_nvs(
                    g_ap_ssid, g_ap_pass, g_channel_auto,
                    g_manual_channel, g_ap_channel);
                if (save_err != ESP_OK) {
                    ESP_LOGW(TAG, "Failed to save selected channel: %s",
                             esp_err_to_name(save_err));
                }
            }
        }
        xSemaphoreGive(ap_config_mutex);
    }
}

/* =========================================================================
 * Main entry point
 * ========================================================================= */

void app_main(void)
{
    esp_err_t nvs_err = nvs_flash_init();
    if (nvs_err == ESP_ERR_NVS_NO_FREE_PAGES ||
        nvs_err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS needs recovery (%s); erasing and reinitializing",
                 esp_err_to_name(nvs_err));
        ESP_ERROR_CHECK(nvs_flash_erase());
        nvs_err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(nvs_err);
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    ap_config_mutex = xSemaphoreCreateMutex();
    ESP_ERROR_CHECK(ap_config_mutex ? ESP_OK : ESP_ERR_NO_MEM);

    load_ap_config_from_nvs();
    wifi_init_softap();

    BaseType_t channel_task_ok = xTaskCreate(
        channel_rescan_task, "channel_rescan", 4096, NULL, 3, NULL);
    ESP_ERROR_CHECK(channel_task_ok == pdPASS ? ESP_OK : ESP_ERR_NO_MEM);

    /* Start modules */
    ESP_ERROR_CHECK(client_rssi_init()); // Initialize client RSSI tracking first
    ESP_ERROR_CHECK(web_server_start());
    ESP_ERROR_CHECK(mqtt_telemetry_start());
    ESP_ERROR_CHECK(oled_start());
    ESP_ERROR_CHECK(ppp_usb_start());
    ESP_ERROR_CHECK(watchdog_start(30, 5000)); // Feed every 5s with a 30s timeout

    /* app_main no longer needs a forever loop:
     * watchdog loop runs in its own task.
     * PPP reconnect is handled inside ppp.c. */
}
