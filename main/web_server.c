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
#include <stdbool.h>

#include "esp_log.h"
#include "esp_http_server.h"
#include "esp_http_client.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_mac.h"
#include "esp_ota_ops.h"
#include "lwip/ip4_addr.h"
#include "net/if.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* Default AP subnet shown in UI */
#define AP_IP_ADDR "192.168.4.1"
#define AJAX_REFRESH_SEC 10
#define AP_MAX_CONN 4

static const char *TAG = "web_server";
static httpd_handle_t s_httpd = NULL;
static bool s_ota_in_progress = false;
static int s_ota_progress = -1;

/* --------------------------------------------------------------------------
 * Helper: JSON escaping (minimal: quotes, backslashes, control chars)
 * -------------------------------------------------------------------------- */

static void json_escape(const char *in, char *out, size_t out_len)
{
    size_t o = 0;
    if (!in || out_len == 0) {
        return;
    }
    for (size_t i = 0; in[i] && o + 2 < out_len; i++) {
        unsigned char c = (unsigned char)in[i];
        switch (c) {
        case '\"':
        case '\\':
            out[o++] = '\\';
            out[o++] = (char)c;
            break;
        case '\n':
            out[o++] = '\\';
            out[o++] = 'n';
            break;
        case '\r':
            out[o++] = '\\';
            out[o++] = 'r';
            break;
        case '\t':
            out[o++] = '\\';
            out[o++] = 't';
            break;
        default:
            if (c < 0x20) {
                /* Skip other control chars */
            } else {
                out[o++] = (char)c;
            }
            break;
        }
        if (o + 1 >= out_len) {
            break;
        }
    }
    out[o] = 0;
}

/* --------------------------------------------------------------------------
 * HTTP Handlers
 * -------------------------------------------------------------------------- */

static esp_err_t root_get_handler(httpd_req_t *req)
{
    const size_t page_len = 8192;
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
        "<script>(function(){var refreshMs=%d*1000;var otaInProgress=false;"
        "var hiddenMs=refreshMs*6;"
        "var timer=null;"
        "var backoff=0;"
        "function updateBadge(){"
        "var btn=document.getElementById('otaBtn');"
        "var badge=document.getElementById('otaBadge');"
        "if(!btn){return;}"
        "btn.disabled=otaInProgress;"
        "btn.textContent=otaInProgress?'Uploading...':'Upload & Update';"
        "if(badge){badge.style.display=otaInProgress?'inline-block':'none';}"
        "}"
        "function nextDelay(){"
        "var base=document.hidden?hiddenMs:refreshMs;"
        "if(backoff<=0){return base;}"
        "var delay=base*backoff;"
        "return delay>60000?60000:delay;"
        "}"
        "function schedule(ms){if(timer){clearTimeout(timer);}timer=setTimeout(tick,ms);}"
        "function setText(id,val){var el=document.getElementById(id);if(el)el.textContent=val;}"
        "function setHtml(id,val){var el=document.getElementById(id);if(el)el.innerHTML=val;}"
        "function refreshPanels(){"
        "if(otaInProgress){return Promise.resolve();}"
        "return fetch('/status/all').then(function(resp){return resp.json();}).then(function(data){"
        "if(!data){return;}"
        "setHtml('mqttStatus',data.mqtt.running?'<span style=\"color:green;\">RUNNING</span>':'<span style=\"color:red;\">STOPPED</span>');"
        "setText('mqttPort',data.mqtt.port);"
        "setText('freeHeap',data.mqtt.free_heap);"
        "setText('obkPower',data.mqtt.obk_power||'');"
        "var conn='';"
        "if(data.mqtt.obk_connected===true){conn='<span style=\"color:green;\">OBK ONLINE</span>';"
        "}else if(data.mqtt.obk_connected===false){conn='<span style=\"color:red;\">OBK OFFLINE</span>';"
        "}else{conn='<span style=\"color:gray;\">UNKNOWN</span>';}"
        "setHtml('obkConn',conn);"
        "setText('mqttApUri',data.mqtt.ap_uri||'');"
        "setText('mqttPppUri',data.mqtt.ppp_up?data.mqtt.ppp_uri:'PPP not up yet.');"
        "setText('pppIp',data.ppp.ip||'0.0.0.0');"
        "setText('pppGw',data.ppp.gw||'0.0.0.0');"
        "setText('pppNm',data.ppp.nm||'0.0.0.0');"
        "var body=document.getElementById('clientTableBody');"
        "if(body){body.innerHTML='';"
        "if(!data.clients||!data.clients.length){body.innerHTML='<tr><td colspan=\"3\">No clients connected.</td></tr>';}else{"
        "data.clients.forEach(function(c,idx){"
        "var ip=c.ip||'0.0.0.0';"
        "var ipCell=ip==='0.0.0.0'?ip:'<a href=\"http://'+ip+'\" target=\"_blank\" rel=\"noopener\">'+ip+'</a>';"
        "body.innerHTML+=('<tr><td>'+(idx+1)+'</td><td>'+c.mac+'</td><td>'+ipCell+'</td></tr>');"
        "});"
        "}}"
        "backoff=0;"
        "}).catch(function(){backoff=backoff>0?Math.min(backoff*2,6):2;});"
        "}"
        "function tick(){"
        "refreshPanels().finally(function(){schedule(nextDelay());});"
        "}"
        "document.addEventListener('visibilitychange',function(){schedule(nextDelay());});"
        "tick();"
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
        "updateBadge();"
        "})();</script>"
        "<style>body{font-family:sans-serif;margin:20px;}table{border-collapse:collapse;}th,td{border:1px solid #ccc;padding:6px 10px;}input{padding:6px;margin:4px 0;}</style>"
        "</head><body>"
        "<h2>ESP32-C3 PPP-over-USB + SoftAP Router (no NAT)</h2>"
        "<h3>PPP Link</h3>"
        "<p><b>PPP IP:</b> <span id='pppIp'>" IPSTR "</span><br>"
        "<b>PPP GW:</b> <span id='pppGw'>" IPSTR "</span><br>"
        "<b>PPP Netmask:</b> <span id='pppNm'>" IPSTR "</span></p><hr>"
        "<h3>Change AP SSID / Password</h3>"
        "<form method='POST' action='/set'>SSID:<br><input name='ssid' maxlength='32' value='%s'><br>"
        "Password:<br><input name='pass' maxlength='64' value='%s'><br>"
        "<small>Empty password = open network. WPA2 requires ≥8 chars.</small><br><br>"
        "<input type='submit' value='Save & Restart AP'></form><hr>"
        "<h3>OTA Firmware Update</h3>"
        "<p>Select a firmware <code>.bin</code> file built for this device. The device will reboot after upload.</p>"
        "<input type='file' id='otaFile' accept='.bin'><br>"
        "<button type='button' id='otaBtn' onclick='startOtaUpload()'>Upload & Update</button>"
        "<span id='otaBadge' style='display:none;margin-left:8px;padding:2px 6px;border-radius:10px;background:#f0ad4e;color:#222;font-size:12px;'>BUSY</span>"
        "<div id='otaStatus' style='margin-top:8px;color:#444;'></div><hr>"
        "<h3>MQTT Broker</h3>"
        "<p><b>Status:</b> <span id='mqttStatus'></span><br>"
        "<b>Port:</b> <span id='mqttPort'></span><br>"
        "<b>Free heap:</b> <span id='freeHeap'></span> bytes</p>"
        "<p><b>Latest " OBK_POWER_TOPIC ":</b> <code id='obkPower'></code></p>"
        "<p><b>OBK connected:</b> <span id='obkConn'></span></p>"
        "<p><b>Connect from WiFi AP clients:</b><br><code id='mqttApUri'></code></p>"
        "<p><b>Connect from Linux PC over PPP:</b><br><code id='mqttPppUri'></code></p><hr>"
        "<h3>Connected Clients</h3>"
        "<table><thead><tr><th>#</th><th>MAC</th><th>IP (DHCP)</th></tr></thead>"
        "<tbody id='clientTableBody'><tr><td colspan='3'>Loading...</td></tr></tbody></table><hr>",
        AJAX_REFRESH_SEC,
        IP2STR(&ppp_ip), IP2STR(&ppp_gw), IP2STR(&ppp_nm),
        ssid ? ssid : "", pass ? pass : ""
    );

    strlcat(page, "</body></html>", page_len);

    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, page, HTTPD_RESP_USE_STRLEN);

    free(page);
    return ESP_OK;
}

static esp_err_t status_all_get_handler(httpd_req_t *req)
{
    const size_t page_len = 2048;
    char *page = (char *)malloc(page_len);
    if (!page) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
        return ESP_FAIL;
    }
    page[0] = 0;

    ip4_addr_t ppp_ip = {0}, ppp_gw = {0}, ppp_nm = {0};
    ppp_get_ip_info(&ppp_ip, &ppp_gw, &ppp_nm);

    char obk_power_raw[64];
    char obk_power[128];
    mqtt_broker_get_obk_power(obk_power_raw, sizeof(obk_power_raw));
    json_escape(obk_power_raw, obk_power, sizeof(obk_power));

    int conn_state = mqtt_broker_get_obk_connected_state();
    const char *conn_bool = "null";
    if (conn_state > 0) {
        conn_bool = "true";
    } else if (conn_state == 0) {
        conn_bool = "false";
    }

    char mqtt_ap_uri[64];
    snprintf(mqtt_ap_uri, sizeof(mqtt_ap_uri), "mqtt://%s:%d", AP_IP_ADDR, MQTT_BROKER_PORT);

    char mqtt_ppp_uri[64];
    bool ppp_up = (ppp_ip.addr != 0);
    if (ppp_ip.addr != 0) {
        snprintf(mqtt_ppp_uri, sizeof(mqtt_ppp_uri), "mqtt://%s:%d",
                 ip4addr_ntoa(&ppp_ip), MQTT_BROKER_PORT);
    } else {
        mqtt_ppp_uri[0] = 0;
    }

    snprintf(page, page_len,
             "{"
             "\"schema_version\":1,"
             "\"mqtt\":{"
             "\"running\":%s,"
             "\"port\":%d,"
             "\"free_heap\":%lu,"
             "\"obk_power\":\"%s\","
             "\"obk_connected\":%s,"
             "\"obk_connected_state\":%d,"
             "\"ap_uri\":\"%s\","
             "\"ppp_uri\":\"%s\","
             "\"ppp_up\":%s"
             "},"
             "\"ppp\":{"
             "\"ip\":\"" IPSTR "\","
             "\"gw\":\"" IPSTR "\","
             "\"nm\":\"" IPSTR "\""
             "},"
             "\"clients\":[",
             mqtt_broker_is_running() ? "true" : "false",
             MQTT_BROKER_PORT,
             (unsigned long)esp_get_free_heap_size(),
             obk_power,
             conn_bool,
             conn_state,
             mqtt_ap_uri,
             mqtt_ppp_uri,
             ppp_up ? "true" : "false",
             IP2STR(&ppp_ip), IP2STR(&ppp_gw), IP2STR(&ppp_nm));

    wifi_sta_list_t sta_list = {0};
    esp_wifi_ap_get_sta_list(&sta_list);

    esp_netif_t *ap_netif = ap_get_netif();
    esp_netif_pair_mac_ip_t pairs[AP_MAX_CONN];
    int n = sta_list.num;
    if (n > AP_MAX_CONN) n = AP_MAX_CONN;

    for (int i = 0; i < n; i++) {
        memcpy(pairs[i].mac, sta_list.sta[i].mac, 6);
        pairs[i].ip.addr = 0;
    }

    if (ap_netif && n > 0) {
        esp_err_t err = esp_netif_dhcps_get_clients_by_mac(ap_netif, n, pairs);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "esp_netif_dhcps_get_clients_by_mac failed: %s",
                     esp_err_to_name(err));
        }
    }

    for (int i = 0; i < n; i++) {
        char mac_str[32];
        char ip_str[IP4ADDR_STRLEN_MAX];
        snprintf(mac_str, sizeof(mac_str), MACSTR, MAC2STR(pairs[i].mac));
        if (pairs[i].ip.addr != 0) {
            esp_ip4addr_ntoa(&pairs[i].ip, ip_str, sizeof(ip_str));
        } else {
            snprintf(ip_str, sizeof(ip_str), IPSTR, IP2STR(&pairs[i].ip));
        }

        strlcat(page, "{\"mac\":\"", page_len);
        strlcat(page, mac_str, page_len);
        strlcat(page, "\",\"ip\":\"", page_len);
        strlcat(page, ip_str, page_len);
        strlcat(page, "\"}", page_len);
        if (i + 1 < n) {
            strlcat(page, ",", page_len);
        }
    }

    strlcat(page, "]}", page_len);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Connection", "close");
    httpd_resp_send(req, page, HTTPD_RESP_USE_STRLEN);
    free(page);
    return ESP_OK;
}

bool web_server_health_check_ex(int *status_out, esp_err_t *err_out)
{
    if (!s_httpd) {
        if (status_out) {
            *status_out = 0;
        }
        if (err_out) {
            *err_out = ESP_ERR_INVALID_STATE;
        }
        return false;
    }

    struct ifreq ifr = {0};
    struct ifreq *ifp = NULL;
    esp_netif_t *ap_netif = ap_get_netif();
    if (ap_netif) {
        char ifname[IFNAMSIZ] = {0};
        if (esp_netif_get_netif_impl_name(ap_netif, ifname) == ESP_OK) {
            strlcpy(ifr.ifr_name, ifname, sizeof(ifr.ifr_name));
            ifp = &ifr;
        }
    }

    esp_http_client_config_t cfg = {
        .url = "http://" AP_IP_ADDR "/status/all",
        .method = HTTP_METHOD_GET,
        .timeout_ms = 1500,
        .disable_auto_redirect = true,
        .if_name = ifp
    };

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        if (status_out) {
            *status_out = 0;
        }
        if (err_out) {
            *err_out = ESP_ERR_NO_MEM;
        }
        return false;
    }

    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (status_out) {
        *status_out = status;
    }
    if (err_out) {
        *err_out = err;
    }
    return (err == ESP_OK && status == 200);
}

bool web_server_health_check(void)
{
    return web_server_health_check_ex(NULL, NULL);
}

bool web_server_is_running(void)
{
    return (s_httpd != NULL);
}

bool web_server_is_ota_in_progress(void)
{
    return s_ota_in_progress;
}

int web_server_get_ota_progress(void)
{
    return s_ota_progress;
}

void web_server_stop(void)
{
    if (s_httpd) {
        httpd_stop(s_httpd);
        s_httpd = NULL;
    }
}

void web_server_restart(void)
{
    web_server_stop();
    vTaskDelay(pdMS_TO_TICKS(200));
    web_server_start();
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
    s_ota_in_progress = true;
    s_ota_progress = 0;

    if (req->content_len == 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Empty OTA image");
        s_ota_in_progress = false;
        s_ota_progress = -1;
        return ESP_FAIL;
    }

    const esp_partition_t *update_partition = esp_ota_get_next_update_partition(NULL);
    if (!update_partition) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "No OTA partition");
        s_ota_in_progress = false;
        s_ota_progress = -1;
        return ESP_FAIL;
    }

    esp_ota_handle_t ota_handle = 0;
    esp_err_t err = esp_ota_begin(update_partition, OTA_SIZE_UNKNOWN, &ota_handle);
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OTA begin failed");
        s_ota_in_progress = false;
        s_ota_progress = -1;
        return ESP_FAIL;
    }

    size_t remaining = req->content_len;
    size_t total = req->content_len;
    char buf[1024];
    while (remaining > 0) {
        int recv_len = httpd_req_recv(req, buf, remaining > sizeof(buf) ? sizeof(buf) : remaining);
        if (recv_len == HTTPD_SOCK_ERR_TIMEOUT) {
            continue;
        }
        if (recv_len <= 0) {
            esp_ota_abort(ota_handle);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OTA receive failed");
            s_ota_in_progress = false;
            s_ota_progress = -1;
            return ESP_FAIL;
        }

        err = esp_ota_write(ota_handle, buf, recv_len);
        if (err != ESP_OK) {
            esp_ota_abort(ota_handle);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OTA write failed");
            s_ota_in_progress = false;
            s_ota_progress = -1;
            return ESP_FAIL;
        }
        remaining -= recv_len;
        size_t written = total - remaining;
        if (total > 0) {
            s_ota_progress = (int)((written * 100U) / total);
        }
    }

    err = esp_ota_end(ota_handle);
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OTA end failed");
        s_ota_in_progress = false;
        s_ota_progress = -1;
        return ESP_FAIL;
    }

    err = esp_ota_set_boot_partition(update_partition);
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OTA set boot partition failed");
        s_ota_in_progress = false;
        s_ota_progress = -1;
        return ESP_FAIL;
    }

    s_ota_progress = 100;
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
    config.recv_wait_timeout = 15;
    config.send_wait_timeout = 15;
    config.lru_purge_enable = true;
    config.keep_alive_enable = false;

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

    httpd_uri_t status_all = {
        .uri      = "/status/all",
        .method   = HTTP_GET,
        .handler  = status_all_get_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(s_httpd, &status_all);

    ESP_LOGI(TAG, "Webserver started on http://%s/", AP_IP_ADDR);
}
