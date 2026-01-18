/*
 * PPP-over-USB + WiFi SoftAP Router (ESP32-C3)
 *
 * HTTP status/config web UI module.
 *
 * Author: Martin Köhler [martinkoehler]
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "web_server.h"
#include "ap_config.h"
#include "mqtt_broker.h"
#include "ppp.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "esp_log.h"
#include "esp_http_server.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_mac.h"
#include "esp_ota_ops.h"
#include "lwip/ip4_addr.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* Default AP subnet shown in UI */
#define AP_IP_ADDR "192.168.4.1"
#define PAGE_REFRESH_SEC 3
#define AP_MAX_CONN 4

static const char *TAG = "web_server";
static httpd_handle_t s_httpd = NULL;

/* --------------------------------------------------------------------------
 * Helper: append HTML table of connected stations
 * -------------------------------------------------------------------------- */

static void append_station_table(char *out, size_t out_len)
{
    wifi_sta_list_t sta_list = {0};
    esp_wifi_ap_get_sta_list(&sta_list);

    strlcat(out, "<h3>Connected Clients</h3>", out_len);

    if (sta_list.num == 0) {
        strlcat(out, "<p>No clients connected.</p>", out_len);
        return;
    }

    esp_netif_t *ap_netif = ap_get_netif();
    if (!ap_netif) {
        strlcat(out, "<p>AP netif not ready.</p>", out_len);
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
        ESP_LOGW(TAG, "esp_netif_dhcps_get_clients_by_mac failed: %s",
                 esp_err_to_name(err));
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

/* --------------------------------------------------------------------------
 * Helper: append MQTT broker status panel
 * -------------------------------------------------------------------------- */

static void append_mqtt_panel(char *out, size_t out_len)
{
    ip4_addr_t ppp_ip = ppp_get_ip();

    char line[256];
    strlcat(out, "<h3>MQTT Broker</h3>", out_len);

    snprintf(line, sizeof(line),
             "<p><b>Status:</b> %s<br>"
             "<b>Port:</b> %d<br>"
             "<b>Free heap:</b> %lu bytes</p>",
             mqtt_broker_is_running()
                ? "<span style='color:green;'>RUNNING</span>"
                : "<span style='color:red;'>STOPPED</span>",
             MQTT_BROKER_PORT,
             (unsigned long)esp_get_free_heap_size());
    strlcat(out, line, out_len);

    char power_copy[64];
    mqtt_broker_get_obk_power(power_copy, sizeof(power_copy));

    snprintf(line, sizeof(line),
             "<p><b>Latest %s:</b> <code>%s</code></p>",
             OBK_POWER_TOPIC, power_copy);
    strlcat(out, line, out_len);

    strlcat(out, "<p><b>Connect from WiFi AP clients:</b><br>", out_len);
    snprintf(line, sizeof(line),
             "<code>mqtt://%s:%d</code></p>", AP_IP_ADDR, MQTT_BROKER_PORT);
    strlcat(out, line, out_len);

    strlcat(out, "<p><b>Connect from Linux PC over PPP:</b><br>", out_len);
    if (ppp_ip.addr != 0) {
        snprintf(line, sizeof(line),
                 "<code>mqtt://%s:%d</code></p>",
                 ip4addr_ntoa(&ppp_ip), MQTT_BROKER_PORT);
    } else {
        snprintf(line, sizeof(line), "<i>PPP not up yet.</i></p>");
    }
    strlcat(out, line, out_len);
}

/* --------------------------------------------------------------------------
 * HTTP Handlers
 * -------------------------------------------------------------------------- */

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
    ppp_get_ip_info(&ppp_ip, &ppp_gw, &ppp_nm);

    const char *ssid = ap_get_ssid();
    const char *pass = ap_get_pass();

    snprintf(page, page_len,
        "<!doctype html><html><head>"
        "<meta charset='utf-8'>"
        "<meta name='viewport' content='width=device-width, initial-scale=1'>"
        "<title>ESP32C3 PPP Router</title>"
        "<script>(function(){var periodSec=%d;var periodMs=periodSec*1000;var otaInProgress=false;"
        "var lastReload=new Date().getTime();"
        "function badgeEl(){return document.getElementById('refreshBadge');}"
        "function userIsEditing(){var ae=document.activeElement;if(!ae)return false;var tag=ae.tagName;return(tag==='INPUT'||tag==='TEXTAREA');}"
        "function setBadge(text,bg){var b=badgeEl();if(!b)return;b.textContent=text;if(bg)b.style.background=bg;}"
        "function updateBadge(){if(otaInProgress){setBadge('Auto-refresh: Paused (OTA upload)','#c6d8f5');}"
        "else if(document.hidden){setBadge('Auto-refresh: Paused (tab hidden)','#f5e6a7');}"
        "else if(userIsEditing()){setBadge('Auto-refresh: Paused while editing','#f5c6c6');}"
        "else{setBadge('Auto-refresh: ON ('+periodSec+'s)','#d5f5d5');}}"
        "document.addEventListener('visibilitychange',updateBadge);"
        "document.addEventListener('focusin',updateBadge);"
        "document.addEventListener('focusout',updateBadge);"
        "updateBadge();"
        "setInterval(function(){updateBadge();if(otaInProgress)return;if(document.hidden)return;if(userIsEditing())return;var now=new Date().getTime();if(now-lastReload<periodMs-50)return;lastReload=now;window.location.reload();},periodMs);"
        "window.startOtaUpload=function(){"
        "var fileInput=document.getElementById('otaFile');"
        "var statusEl=document.getElementById('otaStatus');"
        "if(!fileInput||!fileInput.files||!fileInput.files.length){statusEl.textContent='Select a firmware .bin file first.';return;}"
        "var file=fileInput.files[0];"
        "otaInProgress=true;updateBadge();"
        "statusEl.textContent='Uploading '+file.name+' ('+file.size+' bytes)...';"
        "fetch('/ota',{method:'POST',headers:{'Content-Type':'application/octet-stream','X-OTA-Filename':file.name},body:file})"
        ".then(function(resp){return resp.text().then(function(text){return {ok:resp.ok,text:text};});})"
        ".then(function(result){if(result.ok){statusEl.textContent='Upload complete. Device will reboot shortly.';}else{otaInProgress=false;updateBadge();statusEl.textContent='OTA failed: '+result.text;}})"
        ".catch(function(err){otaInProgress=false;updateBadge();statusEl.textContent='OTA failed: '+err;});"
        "};"
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
        "<input type='submit' value='Save & Restart AP'></form><hr>"
        "<h3>OTA Firmware Update</h3>"
        "<p>Select a firmware <code>.bin</code> file built for this device. The device will reboot after upload.</p>"
        "<input type='file' id='otaFile' accept='.bin'><br>"
        "<button type='button' onclick='startOtaUpload()'>Upload & Update</button>"
        "<div id='otaStatus' style='margin-top:8px;color:#444;'></div><hr>",
        PAGE_REFRESH_SEC,
        IP2STR(&ppp_ip), IP2STR(&ppp_gw), IP2STR(&ppp_nm),
        ssid ? ssid : "", pass ? pass : ""
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

/* Minimal URL decode (in-place), as in original */
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

    esp_err_t err = ap_set_credentials_and_restart(ssid, pass);
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "NVS save failed");
        return ESP_FAIL;
    }

    httpd_resp_set_status(req, "303 See Other");
    httpd_resp_set_hdr(req, "Location", "/");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

static esp_err_t ota_post_handler(httpd_req_t *req)
{
    if (req->content_len == 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Empty OTA image");
        return ESP_FAIL;
    }

    const esp_partition_t *update_partition = esp_ota_get_next_update_partition(NULL);
    if (!update_partition) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "No OTA partition");
        return ESP_FAIL;
    }

    esp_ota_handle_t ota_handle = 0;
    esp_err_t err = esp_ota_begin(update_partition, OTA_SIZE_UNKNOWN, &ota_handle);
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OTA begin failed");
        return ESP_FAIL;
    }

    size_t remaining = req->content_len;
    char buf[1024];
    while (remaining > 0) {
        int recv_len = httpd_req_recv(req, buf, remaining > sizeof(buf) ? sizeof(buf) : remaining);
        if (recv_len <= 0) {
            esp_ota_abort(ota_handle);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OTA receive failed");
            return ESP_FAIL;
        }

        err = esp_ota_write(ota_handle, buf, recv_len);
        if (err != ESP_OK) {
            esp_ota_abort(ota_handle);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OTA write failed");
            return ESP_FAIL;
        }
        remaining -= recv_len;
    }

    err = esp_ota_end(ota_handle);
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OTA end failed");
        return ESP_FAIL;
    }

    err = esp_ota_set_boot_partition(update_partition);
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OTA set boot partition failed");
        return ESP_FAIL;
    }

    httpd_resp_sendstr(req, "OK");
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
    return ESP_OK;
}

/* --------------------------------------------------------------------------
 * Server start
 * -------------------------------------------------------------------------- */

void web_server_start(void)
{
    if (s_httpd) {
        ESP_LOGI(TAG, "Webserver already running");
        return;
    }

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

    httpd_uri_t ota = {
        .uri      = "/ota",
        .method   = HTTP_POST,
        .handler  = ota_post_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(s_httpd, &ota);

    ESP_LOGI(TAG, "Webserver started on http://%s/", AP_IP_ADDR);
}
