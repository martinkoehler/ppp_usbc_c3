/*
 * PPP-over-USB + WiFi SoftAP Router (ESP32-C3)
 *
 * This application runs on an ESP32-C3 device and creates a router
 * that bridges a PPP connection from the USB Serial/JTAG peripheral
 * with a WiFi SoftAP network. It serves an HTTP status/control page,
 * runs a local MQTT broker, and displays power telemetry on an OLED.
 *
 * Features:
 * - Accepts PPP connections over USB CDC (PPP-over-Serial/JTAG)
 * - Hosted WiFi Access Point (SoftAP) configurable via web UI/NVS
 * - HTTP server with auto-refresh, AP config, DHCP station info, PPP status
 * - Built-in MQTT broker (Mosquitto port) for local telemetry/messages
 * - Collects and displays MQTT solar power value on OLED display
 * - Stores WiFi AP config in NVS flash, allows secure/unencrypted AP
 * - All IP/subnet setup is static ("no NAT", PPP and AP in distinct subnets)
 * - Robust to power-loss; minimal RAM/stack usage (heap pages, tasks)
 *
 * Modules:
 * - OLED display driver via u8g2 library
 * - WiFi, PPP, NVS, HTTPD, MQTT broker, FreeRTOS tasks & synchronization
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
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#include <string.h>
#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"

#include "esp_log.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "nvs.h"

// OLED display libraries
#include "u8g2.h"
#include "u8g2_esp32_hal.h"   // HAL for ESP32 and u8g2 display

/* === OLED Configuration === */
/**
 * OLED display connection configuration.
 * - SDA/SCL: I2C pins.
 * - RST: Unused.
 * - width/height: Display window size.
 * - xOffset/yOffset: Where the display region starts.
 * - I2C_ADDR_8BIT: 8-bit I2C address for SSD1306.
 */
#define OLED_SDA GPIO_NUM_5
#define OLED_SCL GPIO_NUM_6
#define OLED_RST U8G2_ESP32_HAL_UNDEFINED

static const int width   = 72;
static const int height  = 40;
static const int xOffset = 28;
static const int yOffset = 18;
static const uint8_t I2C_ADDR_8BIT = (0x3C << 1);

static u8g2_t u8g2; // OLED driver handle

/* === LwIP and PPP === */
#include "lwip/err.h"
#include "lwip/sys.h"
#include "lwip/netif.h"
#include "lwip/ip4_addr.h"
#include "netif/ppp/ppp.h"
#include "netif/ppp/pppapi.h"
#include "netif/ppp/pppos.h"
#include "netif/ppp/ppp_impl.h"

/* === USB Serial/JTAG input for PPP === */
#include "driver/usb_serial_jtag.h"

/* === WiFi SoftAP === */
#include "esp_wifi.h"
#include "esp_mac.h"

/* === HTTP Server for configuration/API === */
#include "esp_http_server.h"

/* === Embedded MQTT broker === */
#include "mosq_broker.h"

static TaskHandle_t s_broker_task = NULL;  // MQTT broker task handle
static bool s_broker_running = false;      // Is broker running?
#define MQTT_BROKER_PORT 1883              // MQTT listening port

static const char *TAG = "ppp_usb_ap_web";

/* === Web UI Auto-refresh and OBK telemetry === */
#define PAGE_REFRESH_SEC 3   // HTTP status page auto-refresh interval (sec)
/**
 * OBK_POWER_TOPIC: Solar value MQTT topic.
 * g_obk_power: latest message received on OBK_POWER_TOPIC.
 * g_obk_mutex: protects g_obk_power from concurrent access.
 */
#define OBK_POWER_TOPIC "obk_wr/power/get"
static char g_obk_power[64] = "N/A";
static SemaphoreHandle_t g_obk_mutex = NULL;

/* === Default WiFi Access Point Config === */
#define DEFAULT_AP_SSID     "ESP32C3-PPP-AP"
#define DEFAULT_AP_PASS     "12345678"
#define AP_CHANNEL          1
#define AP_MAX_CONN         4
#define AP_IP_ADDR     "192.168.4.1"
#define AP_GATEWAY     "192.168.4.1"
#define AP_NETMASK     "255.255.255.0"

/* === NVS keys/namespace for AP Config === */
#define NVS_NS   "apcfg"
#define NVS_KEY_SSID "ssid"
#define NVS_KEY_PASS "pass"

// RAM copies of current AP credentials
static char g_ap_ssid[33] = DEFAULT_AP_SSID;
static char g_ap_pass[65] = DEFAULT_AP_PASS;

/* ==== PPP state ==== */
static ppp_pcb *ppp = NULL;
static struct netif ppp_netif; // lwIP network interface handle

static esp_netif_t *ap_netif = NULL; // esp-netif handle for AP interface

static httpd_handle_t s_httpd = NULL; // HTTP server handle

static EventGroupHandle_t s_event_group; // FreeRTOS event bits for PPP status
#define PPP_CONNECTED_BIT BIT0
#define PPP_DISCONN_BIT   BIT1
#define PPP_MRU_MTU 512 // Custom PPP MRU/MTU for limited links

/* =========================================================================
 *                NVS Flash Storage - Load & Save AP config
 * =========================================================================
 */

/**
 * Load WiFi AP SSID/password from NVS flash.
 * If not present, reset to defaults.
 */
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

/**
 * Save WiFi AP SSID/password into NVS flash.
 * Returns error code.
 */
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
 *                PPP over USB Serial/JTAG
 * =========================================================================
 */

/**
 * PPP RX task: reads bytes from USB Serial/JTAG and feeds to PPP stack.
 */
static void ppp_usb_rx_task(void *arg)
{
    uint8_t buf[256];
    while (1) {
        int n = usb_serial_jtag_read_bytes(buf, sizeof(buf), pdMS_TO_TICKS(100));
        if (n > 0 && ppp) {
            pppos_input_tcpip(ppp, buf, n);
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

/**
 * PPP Output callback: Called by lwIP when PPP needs to transmit data.
 */
static u32_t ppp_output_cb(ppp_pcb *pcb, const void *data, u32_t len, void *ctx)
{
    int written = usb_serial_jtag_write_bytes(data, len, pdMS_TO_TICKS(1000));
    if (written < 0) {
        ESP_LOGW(TAG, "USB write error");
        return 0;
    }
    return (u32_t)written;
}

/**
 * PPP Link status callback (called when PPP connects/disconnects)
 */
static void ppp_status_cb(ppp_pcb *pcb, int err_code, void *ctx)
{
    switch (err_code) {
        case PPPERR_NONE: {
            ESP_LOGI(TAG, "PPP connected");
            ip4_addr_t ip = pcb->netif->ip_addr.u_addr.ip4;
            ip4_addr_t gw = pcb->netif->gw.u_addr.ip4;
            ip4_addr_t nm = pcb->netif->netmask.u_addr.ip4;

            ESP_LOGI(TAG, "PPP IP: " IPSTR, IP2STR(&ip));
            ESP_LOGI(TAG, "PPP GW: " IPSTR, IP2STR(&gw));
            ESP_LOGI(TAG, "PPP NM: " IPSTR, IP2STR(&nm));
            xEventGroupSetBits(s_event_group, PPP_CONNECTED_BIT);
            break;
        }
        case PPPERR_USER:
        default:
            ESP_LOGW(TAG, "PPP error/closed: %d", err_code);
            xEventGroupSetBits(s_event_group, PPP_DISCONN_BIT);
            break;
    }
}

/* =========================================================================
 *                OLED Display Handling
 * =========================================================================
 */

/**
 * Renders OLED UI: solar power value, simple title.
 * Copies latest value from MQTT telemetry.
 */
static void handle_oled(void)
{
    char power_copy[30];
    strlcpy(power_copy, "N/A", sizeof(power_copy));
    if (g_obk_mutex && xSemaphoreTake(g_obk_mutex, pdMS_TO_TICKS(5)) == pdTRUE) {
        strlcpy(power_copy, g_obk_power, sizeof(power_copy));
        xSemaphoreGive(g_obk_mutex);
    }

    u8g2_ClearBuffer(&u8g2);
    u8g2_SetFont(&u8g2, u8g2_font_4x6_tr);

    u8g2_DrawStr(&u8g2, xOffset + 0, yOffset + 10, "Solar power");
    char buffer[32];
    snprintf(buffer, sizeof(buffer), "%s W", power_copy);
    u8g2_DrawStr(&u8g2, xOffset + 0, yOffset + 30, buffer);

    u8g2_SendBuffer(&u8g2); // flush to display
}

/**
 * Task that periodically refreshes the OLED display.
 */
static void oled_task(void *arg)
{
    while (1) {
        handle_oled();
        vTaskDelay(pdMS_TO_TICKS(1000)); // Update every second
    }
}

/**
 * OLED initialization and display setup - starts background display task.
 */
static void start_oled(void)
{
    u8g2_esp32_hal_t hal = U8G2_ESP32_HAL_DEFAULT;
    hal.bus.i2c.sda = OLED_SDA;
    hal.bus.i2c.scl = OLED_SCL;
    hal.reset = U8G2_ESP32_HAL_UNDEFINED;
    hal.dc    = U8G2_ESP32_HAL_UNDEFINED;

    u8g2_esp32_hal_init(hal);

    u8g2_Setup_ssd1306_i2c_128x64_noname_f(
        &u8g2,
        U8G2_R0,
        u8g2_esp32_i2c_byte_cb,
        u8g2_esp32_gpio_and_delay_cb
    );
    u8g2_SetI2CAddress(&u8g2, I2C_ADDR_8BIT);
    u8g2_InitDisplay(&u8g2);
    u8g2_SetPowerSave(&u8g2, 0);
    u8g2_SetContrast(&u8g2, 255);

    ESP_LOGI(TAG, "abrobot-oled init OK. Active window %dx%d @ offset (%d,%d)", width, height, xOffset, yOffset);
    xTaskCreate(oled_task, "oled_task", 4096, NULL, 5, NULL);
}

/* =========================================================================
 *                WiFi SoftAP (Access Point) Handling
 * =========================================================================
 */

/**
 * WiFi event handler: tracks clients joining/leaving AP.
 */
static void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_STACONNECTED) {
        wifi_event_ap_staconnected_t *e = (wifi_event_ap_staconnected_t *)event_data;
        ESP_LOGI(TAG, "STA joined: " MACSTR " AID=%d", MAC2STR(e->mac), e->aid);
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_STADISCONNECTED) {
        wifi_event_ap_stadisconnected_t *e = (wifi_event_ap_stadisconnected_t *)event_data;
        ESP_LOGI(TAG, "STA left: " MACSTR " AID=%d", MAC2STR(e->mac), e->aid);
    }
}

/**
 * Apply new AP config and restart WiFi SoftAP.
 */
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

/**
 * Initialize WiFi SoftAP, configure static IP, launch AP.
 */
static void wifi_init_softap(void)
{
    ESP_LOGI(TAG, "Initializing WiFi SoftAP...");

    ap_netif = esp_netif_create_default_wifi_ap();
    assert(ap_netif);

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));

    ESP_ERROR_CHECK(esp_netif_dhcps_stop(ap_netif));
    esp_netif_ip_info_t ip_info = {0};
    ip4_addr_t tmp;

    ip4addr_aton(AP_IP_ADDR, &tmp);   ip_info.ip.addr = tmp.addr;
    ip4addr_aton(AP_GATEWAY, &tmp);   ip_info.gw.addr = tmp.addr;
    ip4addr_aton(AP_NETMASK, &tmp);   ip_info.netmask.addr = tmp.addr;

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
 *                HTTP Web Server (Config/Status UI)
 * =========================================================================
 */

/**
 * Generates HTML table of all WiFi-connected client stations (MAC/IP).
 */
static void append_station_table(char *out, size_t out_len)
{
    wifi_sta_list_t sta_list = {0};
    esp_wifi_ap_get_sta_list(&sta_list);

    strlcat(out, "<h3>Connected Clients</h3>", out_len);

    if (sta_list.num == 0) {
        strlcat(out, "<p>No clients connected.</p>", out_len);
        return;
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
        ESP_LOGW(TAG, "esp_netif_dhcps_get_clients_by_mac failed: %s", esp_err_to_name(err));
    }

    strlcat(out, "<table border='1' cellpadding='4'>"
                 "<tr><th>#</th><th>MAC</th><th>IP (DHCP)</th></tr>", out_len);

    for (int i = 0; i < n; i++) {
        char row[128];
        snprintf(row, sizeof(row),
                 "<tr><td>%d</td><td>" MACSTR "</td><td>" IPSTR "</td></tr>",
                 i + 1,
                 MAC2STR(pairs[i].mac),
                 IP2STR(&pairs[i].ip));
        strlcat(out, row, out_len);
    }

    strlcat(out, "</table>", out_len);
}

/**
 * MQTT broker panel for web UI.
 * Shows broker port, heap stats, latest OBK power msg,
 * connection info for AP/PPP clients.
 */
static void append_mqtt_panel(char *out, size_t out_len)
{
    ip4_addr_t ppp_ip = {0};
    if (ppp && ppp->netif) {
        ppp_ip = ppp->netif->ip_addr.u_addr.ip4;
    }

    char line[256];

    strlcat(out, "<h3>MQTT Broker</h3>", out_len);

    snprintf(line, sizeof(line),
             "<p><b>Status:</b> %s<br>"
             "<b>Port:</b> %d<br>"
             "<b>Free heap:</b> %lu bytes</p>",
             s_broker_running ? "<span style='color:green;'>RUNNING</span>"
                              : "<span style='color:red;'>STOPPED</span>",
             MQTT_BROKER_PORT,
             (unsigned long)esp_get_free_heap_size());
    strlcat(out, line, out_len);

    char power_copy[64];
    strlcpy(power_copy, "N/A", sizeof(power_copy));

    if (g_obk_mutex && xSemaphoreTake(g_obk_mutex, pdMS_TO_TICKS(5)) == pdTRUE) {
        strlcpy(power_copy, g_obk_power, sizeof(power_copy));
        xSemaphoreGive(g_obk_mutex);
    }

    snprintf(line, sizeof(line),
             "<p><b>Latest %s:</b> <code>%s</code></p>", OBK_POWER_TOPIC, power_copy);
    strlcat(out, line, out_len);

    strlcat(out, "<p><b>Connect from WiFi AP clients:</b><br>", out_len);
    snprintf(line, sizeof(line),
             "<code>mqtt://%s:%d</code></p>", AP_IP_ADDR, MQTT_BROKER_PORT);
    strlcat(out, line, out_len);

    strlcat(out, "<p><b>Connect from Linux PC over PPP:</b><br>", out_len);
    if (ppp_ip.addr != 0) {
        snprintf(line, sizeof(line),
                 "<code>mqtt://%s:%d</code></p>", ip4addr_ntoa(&ppp_ip), MQTT_BROKER_PORT);
    } else {
        snprintf(line, sizeof(line), "<i>PPP not up yet.</i></p>");
    }
    strlcat(out, line, out_len);
}

/**
 * HTTP GET "/" handler: HTML status/control page for router.
 */
static esp_err_t root_get_handler(httpd_req_t *req)
{
    const size_t page_len = 4096;
    char *page = (char *)malloc(page_len);
    if (!page) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
        return ESP_FAIL;
    }
    page[0] = 0;

    ip4_addr_t ppp_ip = {0}, ppp_gw = {0}, ppp_nm = {0};
    if (ppp && ppp->netif) {
        ppp_ip = ppp->netif->ip_addr.u_addr.ip4;
        ppp_gw = ppp->netif->gw.u_addr.ip4;
        ppp_nm = ppp->netif->netmask.u_addr.ip4;
    }

    esp_netif_ip_info_t ap_info = {0};
    esp_netif_get_ip_info(ap_netif, &ap_info);

    snprintf(page, page_len,
        "<!doctype html><html><head>"
        "<meta charset='utf-8'>"
        "<meta name='viewport' content='width=device-width, initial-scale=1'>"
        "<title>ESP32C3 PPP Router</title>"
        "<script>(function(){var periodSec=%d;var periodMs=periodSec*1000;"
        "var lastReload=new Date().getTime();"
        "function badgeEl(){return document.getElementById('refreshBadge');}"
        "function userIsEditing(){var ae=document.activeElement;if(!ae)return false;var tag=ae.tagName;return(tag==='INPUT'||tag==='TEXTAREA');}"
        "function setBadge(text,bg){var b=badgeEl();if(!b)return;b.textContent=text;if(bg)b.style.background=bg;}"
        "function updateBadge(){if(document.hidden){setBadge('Auto-refresh: Paused (tab hidden)','#f5e6a7');}"
        "else if(userIsEditing()){setBadge('Auto-refresh: Paused while editing','#f5c6c6');}"
        "else{setBadge('Auto-refresh: ON ('+periodSec+'s)','#d5f5d5');}}"
        "document.addEventListener('visibilitychange',updateBadge);"
        "document.addEventListener('focusin',updateBadge);"
        "document.addEventListener('focusout',updateBadge);"
        "updateBadge();"
        "setInterval(function(){updateBadge();if(document.hidden)return;if(userIsEditing())return;var now=new Date().getTime();if(now-lastReload<periodMs-50)return;lastReload=now;window.location.reload();},periodMs);"
        "})();</script>"
        "<style>body{font-family:sans-serif;margin:20px;}table{border-collapse:collapse;}th,td{border:1px solid #ccc;padding:6px 10px;}input{padding:6px;margin:4px 0;}</style>"
        "</head><body>"
        "<h2>ESP32-C3 PPP-over-USB + SoftAP Router (no NAT)</h2>"
        "<div id='refreshBadge' style='position:fixed;top:10px;right:10px;background:#eee;border:1px solid #ccc;"
        "padding:6px 10px;border-radius:8px;font-size:12px;opacity:0.9;z-index:9999;'>Auto-refresh: …</div>"
        "<h3>PPP Link</h3>"
        "<p><b>PPP IP:</b> " IPSTR "<br><b>PPP GW:</b> " IPSTR "<br><b>PPP Netmask:</b> " IPSTR "</p><hr>"
        "<h3>Change AP SSID / Password</h3>"
        "<form method='POST' action='/set'>SSID:<br><input name='ssid' maxlength='32' value='%s'><br>"
        "Password:<br><input name='pass' maxlength='64' value='%s'><br>"
        "<small>Empty password = open network. WPA2 requires ≥8 chars.</small><br><br>"
        "<input type='submit' value='Save & Restart AP'></form><hr>",
        PAGE_REFRESH_SEC,
        IP2STR(&ppp_ip), IP2STR(&ppp_gw), IP2STR(&ppp_nm),
        g_ap_ssid, g_ap_pass
    );

    append_mqtt_panel(page, page_len);
    strlcat(page, "<hr>", page_len);
    append_station_table(page, page_len);
    strlcat(page, "</body></html>", page_len);

    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, page, HTTPD_RESP_USE_STRLEN);

    free(page);
    return ESP_OK;
}

/**
 * Decode URL-encoded string in-place (minimal implementation).
 */
static void url_decode_inplace(char *s)
{
    char *p = s, *q = s;
    while (*p) {
        if (*p == '+') {
            *q++ = ' ';
            p++;
        } else if (*p == '%' && p[1] && p[2]) {
            char hex[3] = { p[1], p[2], 0 };
            *q++ = (char)strtol(hex, NULL, 16);
            p += 3;
        } else {
            *q++ = *p++;
        }
    }
    *q = 0;
}

/**
 * HTTP POST "/set" handler: accepts new SSID/password for AP.
 */
static esp_err_t set_post_handler(httpd_req_t *req)
{
    char buf[256];
    int len = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (len <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No data");
        return ESP_FAIL;
    }
    buf[len] = 0;

    char ssid[33] = {0};
    char pass[65] = {0};

    char *ssid_p = strstr(buf, "ssid=");
    char *pass_p = strstr(buf, "pass=");

    if (ssid_p) {
        ssid_p += 5;
        char *end = strchr(ssid_p, '&');
        size_t n = end ? (size_t)(end - ssid_p) : strlen(ssid_p);
        n = (n >= sizeof(ssid)) ? sizeof(ssid) - 1 : n;
        memcpy(ssid, ssid_p, n);
        ssid[n] = 0;
        url_decode_inplace(ssid);
    }

    if (pass_p) {
        pass_p += 5;
        char *end = strchr(pass_p, '&');
        size_t n = end ? (size_t)(end - pass_p) : strlen(pass_p);
        n = (n >= sizeof(pass)) ? sizeof(pass) - 1 : n;
        memcpy(pass, pass_p, n);
        pass[n] = 0;
        url_decode_inplace(pass);
    }

    if (strlen(ssid) == 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "SSID must not be empty");
        return ESP_FAIL;
    }
    if (strlen(pass) > 0 && strlen(pass) < 8) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Password must be >=8 or empty");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "New AP config: SSID='%s' PASS len=%d", ssid, (int)strlen(pass));

    esp_err_t err = save_ap_config_to_nvs(ssid, pass);
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "NVS save failed");
        return ESP_FAIL;
    }

    strlcpy(g_ap_ssid, ssid, sizeof(g_ap_ssid));
    strlcpy(g_ap_pass, pass, sizeof(g_ap_pass));
    apply_ap_config_and_restart();

    httpd_resp_set_status(req, "303 See Other");
    httpd_resp_set_hdr(req, "Location", "/");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

/**
 * Launches web server and registers main URI handlers.
 */
static void start_webserver(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 80;
    config.ctrl_port = 32768;
    config.stack_size = 8192;

    ESP_ERROR_CHECK(httpd_start(&s_httpd, &config));

    httpd_uri_t root = {
        .uri      = "/",
        .method   = HTTP_GET,
        .handler  = root_get_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(s_httpd, &root);

    httpd_uri_t set = {
        .uri      = "/set",
        .method   = HTTP_POST,
        .handler  = set_post_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(s_httpd, &set);

    ESP_LOGI(TAG, "Webserver started on http://%s/", AP_IP_ADDR);
}

/* =========================================================================
 *             MQTT Broker and Power Telemetry Handling
 * =========================================================================
 */

/**
 * Called for every brokered MQTT message.
 * Stores payload of solar power topic for display/UI.
 */
static void broker_message_cb(char *client, char *topic, char *data, int len, int qos, int retain)
{
    (void)client; (void)qos; (void)retain;

    if (!topic || !data || len <= 0) return;

    if (strcmp(topic, OBK_POWER_TOPIC) == 0) {
        if (g_obk_mutex && xSemaphoreTake(g_obk_mutex, 0) == pdTRUE) {
            int n = len;
            if (n >= (int)sizeof(g_obk_power)) n = sizeof(g_obk_power) - 1;
            memcpy(g_obk_power, data, n);
            g_obk_power[n] = 0;
            xSemaphoreGive(g_obk_mutex);
        }
    }
}

/**
 * MQTT broker main task: runs Mosquitto broker on local port.
 */
static void mqtt_broker_task(void *arg)
{
    struct mosq_broker_config cfg = {
        .host = "0.0.0.0",
        .port = MQTT_BROKER_PORT,
        .tls_cfg = NULL,
        .handle_message_cb = broker_message_cb
    };

    ESP_LOGI(TAG, "Mosquitto broker starting on port %d (host=%s)...", cfg.port, cfg.host);

    s_broker_running = true;
    int rc = mosq_broker_run(&cfg);
    s_broker_running = false;

    ESP_LOGW(TAG, "Mosquitto broker exited rc=%d", rc);
    s_broker_task = NULL;
    vTaskDelete(NULL);
}

/**
 * Starts MQTT broker in background FreeRTOS task.
 */
static void start_mqtt_broker(void)
{
    if (s_broker_task || s_broker_running) {
        ESP_LOGI(TAG, "Broker already running");
        return;
    }
    xTaskCreate(mqtt_broker_task, "mosq_broker", 8192, NULL, 8, &s_broker_task);
}

/* =========================================================================
 *                         Main entry point
 * =========================================================================
 */

/**
 * Application main entry point.
 * Sets up flash/NVS, TCP/IP, WiFi AP, PPP, HTTP server, MQTT broker, and OLED task.
 * Maintains PPP event loop for reconnect.
 */
void app_main(void)
{
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    s_event_group = xEventGroupCreate();

    g_obk_mutex = xSemaphoreCreateMutex();
    assert(g_obk_mutex);

    load_ap_config_from_nvs();
    wifi_init_softap();
    start_webserver();
    start_mqtt_broker();
    start_oled();

    usb_serial_jtag_driver_config_t usb_cfg = {
        .tx_buffer_size = 2048,
        .rx_buffer_size = 2048,
    };
    ESP_ERROR_CHECK(usb_serial_jtag_driver_install(&usb_cfg));

    ESP_LOGI(TAG, "Starting PPP over USB Serial/JTAG...");

    memset(&ppp_netif, 0, sizeof(ppp_netif));
    ppp = pppapi_pppos_create(&ppp_netif, ppp_output_cb, ppp_status_cb, NULL);
    assert(ppp);

    ppp_send_config(ppp, PPP_MRU_MTU, 0xFFFFFFFF, 0, 0);
    ppp_recv_config(ppp, PPP_MRU_MTU, 0xFFFFFFFF, 0, 0);
    ppp->netif->mtu = PPP_MRU_MTU;
    ppp_set_auth(ppp, PPPAUTHTYPE_NONE, NULL, NULL);
    ppp_set_usepeerdns(ppp, true);
    pppapi_set_default(ppp);

    xTaskCreate(ppp_usb_rx_task, "ppp_usb_rx", 4096, NULL, 10, NULL);
    pppapi_connect(ppp, 0);

    while (1) {
        EventBits_t bits = xEventGroupWaitBits(
            s_event_group,
            PPP_CONNECTED_BIT | PPP_DISCONN_BIT,
            pdTRUE,
            pdFALSE,
            portMAX_DELAY
        );

        if (bits & PPP_CONNECTED_BIT) {
            ESP_LOGI(TAG, "PPP link up; routing between AP <-> PPP active.");
        }

        if (bits & PPP_DISCONN_BIT) {
            ESP_LOGW(TAG, "PPP link down, reconnecting in 2s...");
            vTaskDelay(pdMS_TO_TICKS(2000));
            pppapi_connect(ppp, 0);
        }
    }
}
