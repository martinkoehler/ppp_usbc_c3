/*
 * PPP-over-USB + WiFi SoftAP Router (ESP32-C3)
 *
 * App glue / startup file after modular refactor.
 *
 * Keeps:
 *  - NVS storage for AP creds
 *  - WiFi SoftAP setup + static IP config
 *  - Exposed AP interface for web server
 *  - Starts modules (PPP, web server, broker, OLED)
 *
 * Author: Martin Köhler [martinkoehler]
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
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
#include "mqtt_broker.h"
#include "oled.h"
#include "watchdog.h"

/* ------------------------- AP defaults ------------------------- */
#define DEFAULT_AP_SSID     "ESP32C3-PPP-AP"
#define DEFAULT_AP_PASS     "12345678"
#define AP_CHANNEL          1
#define AP_MAX_CONN         4

#define AP_IP_ADDR     "192.168.4.1"
#define AP_GATEWAY     "192.168.4.1"
#define AP_NETMASK     "255.255.255.0"

/* NVS keys/namespace */
#define NVS_NS   "apcfg"
#define NVS_KEY_SSID "ssid"
#define NVS_KEY_PASS "pass"

/* ------------------------- module state ------------------------- */
static const char *TAG = "ppp_usb_ap_web";

static char g_ap_ssid[33] = DEFAULT_AP_SSID;
static char g_ap_pass[65] = DEFAULT_AP_PASS;
static esp_netif_t *ap_netif = NULL;

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

    nvs_close(nvs);

    ESP_LOGI(TAG, "Loaded AP config from NVS: SSID='%s' PASS len=%d",
             g_ap_ssid, (int)strlen(g_ap_pass));
}

static esp_err_t save_ap_config_to_nvs(const char *ssid, const char *pass)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &nvs);
    if (err != ESP_OK) return err;

    err = nvs_set_str(nvs, NVS_KEY_SSID, ssid);
    if (err != ESP_OK) { nvs_close(nvs); return err; }

    err = nvs_set_str(nvs, NVS_KEY_PASS, pass);
    if (err != ESP_OK) { nvs_close(nvs); return err; }

    err = nvs_commit(nvs);
    nvs_close(nvs);
    return err;
}

/* =========================================================================
 * WiFi SoftAP Handling
 * ========================================================================= */

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    (void)arg;

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_STACONNECTED) {
        wifi_event_ap_staconnected_t *e = (wifi_event_ap_staconnected_t *)event_data;
        ESP_LOGI(TAG, "STA joined: " MACSTR " AID=%d", MAC2STR(e->mac), e->aid);
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_STADISCONNECTED) {
        wifi_event_ap_stadisconnected_t *e = (wifi_event_ap_stadisconnected_t *)event_data;
        ESP_LOGI(TAG, "STA left: " MACSTR " AID=%d", MAC2STR(e->mac), e->aid);
    }
}

static void apply_ap_config_and_restart(void)
{
    wifi_config_t wifi_config = {0};
    wifi_config.ap.channel = AP_CHANNEL;
    wifi_config.ap.max_connection = AP_MAX_CONN;
    wifi_config.ap.authmode = WIFI_AUTH_WPA2_PSK;
    wifi_config.ap.pmf_cfg.required = false;

    strncpy((char *)wifi_config.ap.ssid, g_ap_ssid, sizeof(wifi_config.ap.ssid));
    wifi_config.ap.ssid_len = strlen(g_ap_ssid);

    strncpy((char *)wifi_config.ap.password, g_ap_pass, sizeof(wifi_config.ap.password));

    if (strlen(g_ap_pass) == 0) {
        wifi_config.ap.authmode = WIFI_AUTH_OPEN;
    }

    ESP_ERROR_CHECK(esp_wifi_stop());
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "SoftAP restarted. SSID='%s'", g_ap_ssid);
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

    wifi_config_t wifi_config = {0};
    wifi_config.ap.channel = AP_CHANNEL;
    wifi_config.ap.max_connection = AP_MAX_CONN;
    wifi_config.ap.authmode = WIFI_AUTH_WPA2_PSK;
    wifi_config.ap.pmf_cfg.required = false;

    strncpy((char *)wifi_config.ap.ssid, g_ap_ssid, sizeof(wifi_config.ap.ssid));
    wifi_config.ap.ssid_len = strlen(g_ap_ssid);
    strncpy((char *)wifi_config.ap.password, g_ap_pass, sizeof(wifi_config.ap.password));

    if (strlen(g_ap_pass) == 0) wifi_config.ap.authmode = WIFI_AUTH_OPEN;

    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "SoftAP up. SSID=%s IP=%s", g_ap_ssid, AP_IP_ADDR);
}

/* =========================================================================
 * ap_config.h interface (used by web_server.c)
 * ========================================================================= */

const char *ap_get_ssid(void) { return g_ap_ssid; }
const char *ap_get_pass(void) { return g_ap_pass; }
esp_netif_t *ap_get_netif(void) { return ap_netif; }

esp_err_t ap_set_credentials_and_restart(const char *ssid, const char *pass)
{
    esp_err_t err = save_ap_config_to_nvs(ssid, pass);
    if (err != ESP_OK) return err;

    strlcpy(g_ap_ssid, ssid, sizeof(g_ap_ssid));
    strlcpy(g_ap_pass, pass, sizeof(g_ap_pass));
    apply_ap_config_and_restart();
    return ESP_OK;
}

esp_err_t ap_restart(void)
{
    apply_ap_config_and_restart();
    return ESP_OK;
}

/* =========================================================================
 * Main entry point
 * ========================================================================= */

void app_main(void)
{
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    load_ap_config_from_nvs();
    wifi_init_softap();

    /* Start modules */
    web_server_start();
    mqtt_broker_start();
    oled_start();
    ppp_usb_start();
    watchdog_start(10, 5000); // Reset after 10s, feeds every 5s

    /* app_main no longer needs a forever loop:
     * watchdog loop runs in its own task.
     * PPP reconnect is handled inside ppp.c. */
}
