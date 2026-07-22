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
#include "mqtt_telemetry.h"
#include "oled.h"
#include "ppp.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <ctype.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_http_server.h"
#include "esp_http_client.h"
#include "esp_tls_crypto.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_mac.h"
#include "esp_ota_ops.h"
#include "lwip/ip4_addr.h"
#include "net/if.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

/* Default AP subnet shown in UI */
#define AP_IP_ADDR "192.168.4.1"
#define AJAX_REFRESH_SEC 10
#define AP_MAX_CONN 4
#define AP_MIN_CHANNEL 1
#define AP_MAX_CHANNEL 11

static const char *TAG = "web_server";
static httpd_handle_t s_httpd = NULL;
static SemaphoreHandle_t s_server_mutex = NULL;
static bool s_ota_in_progress = false;
static int s_ota_progress = -1;
static portMUX_TYPE s_ota_lock = portMUX_INITIALIZER_UNLOCKED;
static int s_health_status = 0;
static esp_err_t s_health_err = ESP_ERR_INVALID_STATE;
static int64_t s_health_checked_at_us = 0;
static portMUX_TYPE s_health_lock = portMUX_INITIALIZER_UNLOCKED;
static bool s_auth_enabled = true;
static portMUX_TYPE s_auth_lock = portMUX_INITIALIZER_UNLOCKED;

#define ADMIN_USERNAME "admin"

static void set_ota_state(bool in_progress, int progress)
{
    portENTER_CRITICAL(&s_ota_lock);
    s_ota_in_progress = in_progress;
    s_ota_progress = progress;
    portEXIT_CRITICAL(&s_ota_lock);
}

static bool web_admin_authorized(httpd_req_t *req)
{
    if (!web_server_is_auth_enabled()) {
        return true;
    }

    char user_info[sizeof(ADMIN_USERNAME) + 1 + 64];
    char expected[128] = "Basic ";
    char received[128];
    char password[65];
    size_t encoded_len = 0;
    ap_get_config_snapshot(NULL, 0, password, sizeof(password), NULL);

    int n = snprintf(user_info, sizeof(user_info), "%s:%s",
                     ADMIN_USERNAME, password);
    if (n < 0 || n >= (int)sizeof(user_info)) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                            "Unable to prepare authentication");
        return false;
    }

    if (esp_crypto_base64_encode(
            (unsigned char *)expected + 6, sizeof(expected) - 6,
            &encoded_len, (const unsigned char *)user_info, strlen(user_info)) != 0 ||
        encoded_len + 6 >= sizeof(expected)) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                            "Unable to prepare authentication");
        return false;
    }
    expected[6 + encoded_len] = 0;

    size_t header_len = httpd_req_get_hdr_value_len(req, "Authorization");
    if (header_len == 0 || header_len >= sizeof(received) ||
        httpd_req_get_hdr_value_str(req, "Authorization", received,
                                    sizeof(received)) != ESP_OK ||
        strcmp(received, expected) != 0) {
        httpd_resp_set_status(req, "401 Unauthorized");
        httpd_resp_set_hdr(req, "WWW-Authenticate",
                           "Basic realm=\"ESP32-C3 Router\"");
        httpd_resp_sendstr(req, "Authentication required");
        return false;
    }

    return true;
}

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

static void html_escape(const char *in, char *out, size_t out_len)
{
    size_t used = 0;
    while (*in && used + 1 < out_len) {
        const char *replacement = NULL;
        switch (*in) {
            case '&': replacement = "&amp;"; break;
            case '<': replacement = "&lt;"; break;
            case '>': replacement = "&gt;"; break;
            case '\"': replacement = "&quot;"; break;
            case '\'': replacement = "&#39;"; break;
            default: break;
        }
        if (replacement) {
            size_t replacement_len = strlen(replacement);
            if (used + replacement_len >= out_len) {
                break;
            }
            memcpy(out + used, replacement, replacement_len);
            used += replacement_len;
        } else {
            out[used++] = *in;
        }
        in++;
    }
    out[used] = 0;
}

/* --------------------------------------------------------------------------
 * HTTP Handlers
 * -------------------------------------------------------------------------- */

static esp_err_t root_get_handler(httpd_req_t *req)
{
    if (!web_admin_authorized(req)) {
        return ESP_OK;
    }

    const size_t page_len = 10240;
    char *page = (char *)malloc(page_len);
    if (!page) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
        return ESP_FAIL;
    }
    page[0] = 0;

    ip4_addr_t ppp_ip = {0}, ppp_gw = {0}, ppp_nm = {0};
    ppp_get_ip_info(&ppp_ip, &ppp_gw, &ppp_nm);

    char ssid[33];
    ap_channel_status_t channel_status;
    ap_get_config_snapshot(ssid, sizeof(ssid), NULL, 0, &channel_status);
    char escaped_ssid[256];
    html_escape(ssid, escaped_ssid, sizeof(escaped_ssid));
    mqtt_telemetry_config_t mqtt_config;
    mqtt_telemetry_get_config(&mqtt_config);
    char escaped_mqtt_host[64];
    char escaped_mqtt_root[192];
    html_escape(mqtt_config.broker_host, escaped_mqtt_host,
                sizeof(escaped_mqtt_host));
    html_escape(mqtt_config.root_topic, escaped_mqtt_root,
                sizeof(escaped_mqtt_root));

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
        "window.toggleManualChannel=function(){"
        "var auto=document.getElementById('channelAuto');"
        "var row=document.getElementById('manualChannelRow');"
        "var input=document.getElementById('manualChannel');"
        "if(!auto||!row||!input){return;}"
        "row.style.display=auto.checked?'none':'block';input.disabled=auto.checked;"
        "};"
        "function refreshPanels(){"
        "if(otaInProgress){return Promise.resolve();}"
        "return fetch('/status/all').then(function(resp){return resp.json();}).then(function(data){"
        "if(!data){return;}"
        "setHtml('mqttStatus',data.mqtt.connected?'<span style=\"color:green;\">CONNECTED</span>':'<span style=\"color:red;\">UNREACHABLE</span>');"
        "setText('mqttPort',data.mqtt.port);"
        "setText('mqttHost',data.mqtt.host||'');"
        "setText('mqttPowerTopic',data.mqtt.power_topic||'');"
        "setText('mqttConnectedTopic',data.mqtt.connected_topic||'');"
        "setText('freeHeap',data.mqtt.free_heap);"
        "setText('obkPower',data.mqtt.obk_power||'');"
        "var conn='';"
        "if(!data.mqtt.connected){conn='<span style=\"color:red;\">BROKER UNREACHABLE</span>';"
        "}else if(data.mqtt.obk_connected===true){conn='<span style=\"color:green;\">OBK ONLINE</span>';"
        "}else if(data.mqtt.obk_connected===false){conn='<span style=\"color:red;\">OBK OFFLINE</span>';"
        "}else{conn='<span style=\"color:gray;\">UNKNOWN</span>';}"
        "setHtml('obkConn',conn);"
        "setText('displayState',data.display_enabled?'ON':'OFF');"
        "setText('apChannel',data.ap.channel||'');"
        "setText('channelMode',data.ap.channel_auto?'Automatic':'Manual');"
        "var scan=data.ap.scan_in_progress?'Scanning...':data.ap.last_scan;"
        "if(data.ap.last_scan_age_sec!==null&&!data.ap.scan_in_progress){scan+=' ('+data.ap.last_scan_age_sec+'s ago)';}"
        "setText('channelScan',scan);"
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
        "document.addEventListener('DOMContentLoaded',window.toggleManualChannel);updateBadge();"
        "})();</script>"
        "<style>body{font-family:sans-serif;margin:20px;}table{border-collapse:collapse;}th,td{border:1px solid #ccc;padding:6px 10px;}input{padding:6px;margin:4px 0;}</style>"
        "</head><body>"
        "<h2>ESP32-C3 PPP-over-USB + SoftAP Router (no NAT)</h2>"
        "<h3>PPP Link</h3>"
        "<p><b>PPP IP:</b> <span id='pppIp'>" IPSTR "</span><br>"
        "<b>PPP GW:</b> <span id='pppGw'>" IPSTR "</span><br>"
        "<b>PPP Netmask:</b> <span id='pppNm'>" IPSTR "</span></p><hr>"
        "<h3>Change AP Settings</h3>"
        "<form method='POST' action='/set'>SSID:<br><input name='ssid' maxlength='32' value='%s'><br>"
        "Password:<br><input type='password' name='pass' maxlength='64' value='' placeholder='Leave blank to keep current'><br>"
        "<label><input type='checkbox' name='open' value='1'> Use an open network</label><br>"
        "<label><input id='channelAuto' type='checkbox' name='channel_auto' value='1' onchange='toggleManualChannel()'%s> Automatically select channel</label><br>"
        "<div id='manualChannelRow'>Channel:<br><input id='manualChannel' name='channel' type='number' min='1' max='11' step='1' value='%u'></div>"
        "<small>Automatic selection scans after one idle minute and only rescans after the AP has been idle. Leave password blank to keep it unchanged.</small><br><br>"
        "<input type='submit' value='Save & Restart AP'></form><hr>"
        "<h3>OLED Diagnostics</h3>"
        "<form method='POST' action='/oled/debug'><button type='submit'>Toggle Debug Page</button></form>"
        "<p><small>Use this control to test the display without pressing the GPIO9 BOOT button.</small></p><hr>"
        "<h3>MQTT Display Source</h3>"
        "<form method='POST' action='/mqtt/display'>"
        "FRITZ!Box MQTT broker IPv4:<br><input name='broker_host' maxlength='15' value='%s' required><br>"
        "<small>Port 1883 is used.</small><br>"
        "Grafana root topic:<br><input name='root_topic' maxlength='63' value='%s' required><br>"
        "<small>For example OBK-681; /power/get and /connected are appended automatically.</small><br>"
        "<label><input type='checkbox' name='display_enabled' value='1'%s> OLED enabled</label><br><br>"
        "<button type='submit'>Save MQTT & Display Settings</button></form><hr>"
        "<h3>OTA Firmware Update</h3>"
        "<p>Select a firmware <code>.bin</code> file built for this device. The device will reboot after upload.</p>"
        "<input type='file' id='otaFile' accept='.bin'><br>"
        "<button type='button' id='otaBtn' onclick='startOtaUpload()'>Upload & Update</button>"
        "<span id='otaBadge' style='display:none;margin-left:8px;padding:2px 6px;border-radius:10px;background:#f0ad4e;color:#222;font-size:12px;'>BUSY</span>"
        "<div id='otaStatus' style='margin-top:8px;color:#444;'></div><hr>"
        "<h3>MQTT Telemetry</h3>"
        "<p><b>Broker status:</b> <span id='mqttStatus'></span><br>"
        "<b>Broker:</b> <code id='mqttHost'></code>:<span id='mqttPort'></span><br>"
        "<b>Power topic:</b> <code id='mqttPowerTopic'></code><br>"
        "<b>Connection topic:</b> <code id='mqttConnectedTopic'></code><br>"
        "<b>OLED:</b> <span id='displayState'></span><br>"
        "<b>Free heap:</b> <span id='freeHeap'></span> bytes</p>"
        "<p><b>Latest power:</b> <code id='obkPower'></code></p>"
        "<p><b>OBK connected:</b> <span id='obkConn'></span></p>"
        "<p><b>Current AP channel:</b> <span id='apChannel'>%u</span></p>"
        "<p><b>Channel selection:</b> <span id='channelMode'>%s</span><br>"
        "<b>Last automatic scan:</b> <span id='channelScan'>%s</span></p>"
        "<hr>"
        "<h3>Connected Clients</h3>"
        "<table><thead><tr><th>#</th><th>MAC</th><th>IP (DHCP)</th></tr></thead>"
        "<tbody id='clientTableBody'><tr><td colspan='3'>Loading...</td></tr></tbody></table><hr>",
        AJAX_REFRESH_SEC,
        IP2STR(&ppp_ip), IP2STR(&ppp_gw), IP2STR(&ppp_nm),
        escaped_ssid,
        channel_status.channel_auto ? " checked" : "",
        channel_status.manual_channel,
        escaped_mqtt_host,
        escaped_mqtt_root,
        oled_is_enabled() ? " checked" : "",
        channel_status.active_channel,
        channel_status.channel_auto ? "Automatic" : "Manual",
        channel_status.last_scan_time_us == 0 ? "Never" :
            esp_err_to_name(channel_status.last_scan_result)
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
    mqtt_telemetry_get_power(obk_power_raw, sizeof(obk_power_raw));
    json_escape(obk_power_raw, obk_power, sizeof(obk_power));

    int conn_state = mqtt_telemetry_get_obk_connected_state();
    const char *conn_bool = "null";
    if (conn_state > 0) {
        conn_bool = "true";
    } else if (conn_state == 0) {
        conn_bool = "false";
    }

    mqtt_telemetry_config_t mqtt_config;
    mqtt_telemetry_get_config(&mqtt_config);
    char mqtt_power_topic[MQTT_ROOT_TOPIC_MAX_LEN + 16];
    char mqtt_connected_topic[MQTT_ROOT_TOPIC_MAX_LEN + 16];
    snprintf(mqtt_power_topic, sizeof(mqtt_power_topic), "%s/power/get",
             mqtt_config.root_topic);
    snprintf(mqtt_connected_topic, sizeof(mqtt_connected_topic), "%s/connected",
             mqtt_config.root_topic);

    ap_channel_status_t channel_status;
    ap_get_config_snapshot(NULL, 0, NULL, 0, &channel_status);
    int64_t scan_age_sec = channel_status.last_scan_time_us > 0
        ? (esp_timer_get_time() - channel_status.last_scan_time_us) / 1000000
        : -1;
    char scan_age_json[24];
    if (scan_age_sec < 0) {
        strlcpy(scan_age_json, "null", sizeof(scan_age_json));
    } else {
        snprintf(scan_age_json, sizeof(scan_age_json), "%lld",
                 (long long)scan_age_sec);
    }
    snprintf(page, page_len,
             "{"
             "\"schema_version\":3,"
             "\"mqtt\":{"
             "\"connected\":%s,"
             "\"host\":\"%s\","
             "\"port\":%d,"
             "\"root_topic\":\"%s\","
             "\"power_topic\":\"%s\","
             "\"connected_topic\":\"%s\","
             "\"free_heap\":%lu,"
             "\"obk_power\":\"%s\","
             "\"obk_connected\":%s,"
             "\"obk_connected_state\":%d"
             "},"
             "\"display_enabled\":%s,"
             "\"ap\":{"
             "\"channel\":%u,"
             "\"channel_auto\":%s,"
             "\"manual_channel\":%u,"
             "\"scan_in_progress\":%s,"
             "\"last_scan\":\"%s\","
             "\"last_scan_age_sec\":%s"
             "},"
             "\"ppp\":{"
             "\"ip\":\"" IPSTR "\","
             "\"gw\":\"" IPSTR "\","
             "\"nm\":\"" IPSTR "\""
             "},"
             "\"clients\":[",
             mqtt_telemetry_is_broker_connected() ? "true" : "false",
             mqtt_config.broker_host,
             MQTT_TELEMETRY_PORT,
             mqtt_config.root_topic,
             mqtt_power_topic,
             mqtt_connected_topic,
             (unsigned long)esp_get_free_heap_size(),
             obk_power,
             conn_bool,
             conn_state,
             oled_is_enabled() ? "true" : "false",
             channel_status.active_channel,
             channel_status.channel_auto ? "true" : "false",
             channel_status.manual_channel,
             channel_status.scan_in_progress ? "true" : "false",
             channel_status.last_scan_time_us == 0 ? "Never" :
                 esp_err_to_name(channel_status.last_scan_result),
             scan_age_json,
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

static bool store_health_result(int status, esp_err_t err,
                                int *status_out, esp_err_t *err_out)
{
    portENTER_CRITICAL(&s_health_lock);
    s_health_status = status;
    s_health_err = err;
    s_health_checked_at_us = esp_timer_get_time();
    portEXIT_CRITICAL(&s_health_lock);

    if (status_out) {
        *status_out = status;
    }
    if (err_out) {
        *err_out = err;
    }
    return err == ESP_OK && status == 200;
}

bool web_server_health_check_ex(int *status_out, esp_err_t *err_out)
{
    if (!s_server_mutex) {
        return store_health_result(0, ESP_ERR_INVALID_STATE,
                                   status_out, err_out);
    }
    xSemaphoreTake(s_server_mutex, portMAX_DELAY);
    if (!s_httpd) {
        xSemaphoreGive(s_server_mutex);
        return store_health_result(0, ESP_ERR_INVALID_STATE,
                                   status_out, err_out);
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
        xSemaphoreGive(s_server_mutex);
        return store_health_result(0, ESP_ERR_NO_MEM, status_out, err_out);
    }

    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    xSemaphoreGive(s_server_mutex);

    return store_health_result(status, err, status_out, err_out);
}

bool web_server_health_check(void)
{
    return web_server_health_check_ex(NULL, NULL);
}

void web_server_get_cached_health(int *status_out, esp_err_t *err_out,
                                  int64_t *checked_at_us_out)
{
    portENTER_CRITICAL(&s_health_lock);
    if (status_out) {
        *status_out = s_health_status;
    }
    if (err_out) {
        *err_out = s_health_err;
    }
    if (checked_at_us_out) {
        *checked_at_us_out = s_health_checked_at_us;
    }
    portEXIT_CRITICAL(&s_health_lock);
}

bool web_server_is_running(void)
{
    if (!s_server_mutex) return false;
    xSemaphoreTake(s_server_mutex, portMAX_DELAY);
    bool running = s_httpd != NULL;
    xSemaphoreGive(s_server_mutex);
    return running;
}

bool web_server_is_ota_in_progress(void)
{
    bool in_progress;
    portENTER_CRITICAL(&s_ota_lock);
    in_progress = s_ota_in_progress;
    portEXIT_CRITICAL(&s_ota_lock);
    return in_progress;
}

int web_server_get_ota_progress(void)
{
    int progress;
    portENTER_CRITICAL(&s_ota_lock);
    progress = s_ota_progress;
    portEXIT_CRITICAL(&s_ota_lock);
    return progress;
}

bool web_server_is_auth_enabled(void)
{
    bool enabled;
    portENTER_CRITICAL(&s_auth_lock);
    enabled = s_auth_enabled;
    portEXIT_CRITICAL(&s_auth_lock);
    return enabled;
}

bool web_server_toggle_authentication(void)
{
    bool enabled;
    portENTER_CRITICAL(&s_auth_lock);
    s_auth_enabled = !s_auth_enabled;
    enabled = s_auth_enabled;
    portEXIT_CRITICAL(&s_auth_lock);

    ESP_LOGW(TAG, "Web Basic authentication %s by BOOT-button gesture",
             enabled ? "enabled" : "disabled");
    return enabled;
}

void web_server_stop(void)
{
    if (!s_server_mutex) return;
    xSemaphoreTake(s_server_mutex, portMAX_DELAY);
    if (s_httpd) {
        httpd_stop(s_httpd);
        s_httpd = NULL;
    }
    xSemaphoreGive(s_server_mutex);
}

void web_server_restart(void)
{
    web_server_stop();
    vTaskDelay(pdMS_TO_TICKS(200));
    esp_err_t err = web_server_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to restart webserver: %s", esp_err_to_name(err));
    }
}

static int hex_value(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static bool url_decode_component(const char *src, size_t src_len,
                                 char *out, size_t out_len)
{
    size_t written = 0;
    for (size_t i = 0; i < src_len; i++) {
        char value;
        if (src[i] == '+') {
            value = ' ';
        } else if (src[i] == '%') {
            if (i + 2 >= src_len) {
                return false;
            }
            int high = hex_value(src[i + 1]);
            int low = hex_value(src[i + 2]);
            if (high < 0 || low < 0) {
                return false;
            }
            value = (char)((high << 4) | low);
            i += 2;
            if (value == 0) {
                return false;
            }
        } else {
            value = src[i];
        }

        if (written + 1 >= out_len) {
            return false;
        }
        out[written++] = value;
    }
    out[written] = 0;
    return true;
}

static bool parse_form_field(const char *buf, const char *key, char *out, size_t out_len)
{
    size_t key_len = strlen(key);
    const char *field = buf;

    while (*field) {
        const char *end = strchr(field, '&');
        if (!end) {
            end = field + strlen(field);
        }
        const char *equals = memchr(field, '=', (size_t)(end - field));
        if (equals && (size_t)(equals - field) == key_len &&
            memcmp(field, key, key_len) == 0) {
            return url_decode_component(equals + 1, (size_t)(end - equals - 1),
                                        out, out_len);
        }
        field = *end ? end + 1 : end;
    }
    return false;
}

static esp_err_t receive_request_body(httpd_req_t *req, char *buf, size_t buf_size)
{
    if (req->content_len <= 0 || (size_t)req->content_len >= buf_size) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid form size");
        return ESP_FAIL;
    }

    size_t received = 0;
    unsigned timeout_count = 0;
    while (received < (size_t)req->content_len) {
        int len = httpd_req_recv(req, buf + received,
                                 (size_t)req->content_len - received);
        if (len == HTTPD_SOCK_ERR_TIMEOUT) {
            if (++timeout_count >= 3) {
                httpd_resp_send_err(req, HTTPD_408_REQ_TIMEOUT,
                                    "Timed out receiving form data");
                return ESP_FAIL;
            }
            continue;
        }
        if (len <= 0) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                "Incomplete form data");
            return ESP_FAIL;
        }
        timeout_count = 0;
        received += (size_t)len;
    }
    buf[received] = 0;
    return ESP_OK;
}

static bool valid_wpa_password(const char *pass)
{
    size_t len = strlen(pass);
    if (len == 0 || (len >= 8 && len <= 63)) {
        return true;
    }
    if (len != 64) {
        return false;
    }
    for (size_t i = 0; i < len; i++) {
        if (!isxdigit((unsigned char)pass[i])) {
            return false;
        }
    }
    return true;
}

static esp_err_t set_post_handler(httpd_req_t *req)
{
    if (!web_admin_authorized(req)) {
        return ESP_OK;
    }

    char buf[512];
    if (receive_request_body(req, buf, sizeof(buf)) != ESP_OK) {
        return ESP_FAIL;
    }

    char ssid[33] = {0};
    char pass[65] = {0};
    char channel_raw[4] = {0};
    char open_raw[2] = {0};
    char channel_auto_raw[2] = {0};

    if (!parse_form_field(buf, "ssid", ssid, sizeof(ssid)) ||
        !parse_form_field(buf, "pass", pass, sizeof(pass))) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid form data");
        return ESP_FAIL;
    }
    bool open_network = parse_form_field(buf, "open", open_raw, sizeof(open_raw)) &&
                        strcmp(open_raw, "1") == 0;
    bool channel_auto = parse_form_field(buf, "channel_auto",
                                         channel_auto_raw,
                                         sizeof(channel_auto_raw)) &&
                        strcmp(channel_auto_raw, "1") == 0;

    if (!open_network && pass[0] == 0) {
        ap_get_config_snapshot(NULL, 0, pass, sizeof(pass), NULL);
    } else if (open_network) {
        pass[0] = 0;
    }

    ap_channel_status_t current_channel_status;
    ap_get_config_snapshot(NULL, 0, NULL, 0, &current_channel_status);
    long channel = current_channel_status.manual_channel;
    char *endptr = NULL;
    if (!channel_auto) {
        if (!parse_form_field(buf, "channel", channel_raw,
                              sizeof(channel_raw))) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                "Manual channel is required");
            return ESP_FAIL;
        }
        channel = strtol(channel_raw, &endptr, 10);
    }

    if (strlen(ssid) == 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "SSID must not be empty");
        return ESP_FAIL;
    }
    if (!valid_wpa_password(pass)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                            "Password must be empty, 8-63 characters, or 64 hex digits");
        return ESP_FAIL;
    }
    if (!channel_auto &&
        (channel_raw[0] == 0 || endptr == channel_raw || *endptr != '\0' ||
         channel < AP_MIN_CHANNEL || channel > AP_MAX_CHANNEL)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Channel must be between 1 and 11");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "New AP config: SSID='%s' PASS len=%d mode=%s CH=%ld",
             ssid, (int)strlen(pass), channel_auto ? "auto" : "manual",
             channel);

    esp_err_t err = ap_set_credentials_and_restart(
        ssid, pass, channel_auto, (uint8_t)channel);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to apply AP configuration: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                            "AP configuration was not changed");
        return ESP_FAIL;
    }

    httpd_resp_set_status(req, "303 See Other");
    httpd_resp_set_hdr(req, "Location", "/");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

static esp_err_t mqtt_display_post_handler(httpd_req_t *req)
{
    if (!web_admin_authorized(req)) {
        return ESP_OK;
    }

    char buf[256];
    if (receive_request_body(req, buf, sizeof(buf)) != ESP_OK) {
        return ESP_FAIL;
    }

    char broker_host[MQTT_BROKER_HOST_MAX_LEN + 1] = {0};
    char root_topic[MQTT_ROOT_TOPIC_MAX_LEN + 1] = {0};
    char display_raw[2] = {0};
    if (!parse_form_field(buf, "broker_host", broker_host,
                          sizeof(broker_host)) ||
        !parse_form_field(buf, "root_topic", root_topic,
                          sizeof(root_topic))) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                            "Broker IPv4 and root topic are required");
        return ESP_FAIL;
    }
    bool display_enabled =
        parse_form_field(buf, "display_enabled", display_raw,
                         sizeof(display_raw)) &&
        strcmp(display_raw, "1") == 0;

    bool previous_display_enabled = oled_is_enabled();
    esp_err_t err = oled_set_enabled(display_enabled);
    if (err == ESP_OK) {
        err = mqtt_telemetry_set_config(broker_host, root_topic);
    }
    if (err != ESP_OK) {
        if (oled_is_enabled() != previous_display_enabled) {
            oled_set_enabled(previous_display_enabled);
        }
        ESP_LOGE(TAG, "Failed to apply MQTT/display configuration: %s",
                 esp_err_to_name(err));
        httpd_resp_send_err(
            req, err == ESP_ERR_INVALID_ARG ? HTTPD_400_BAD_REQUEST
                                            : HTTPD_500_INTERNAL_SERVER_ERROR,
            err == ESP_ERR_INVALID_ARG
                ? "Use a valid IPv4 address and a root containing only letters, digits, '.', '_' or '-'"
                : "MQTT/display configuration was not saved");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "MQTT/display config changed: broker=%s:%d root=%s OLED=%s",
             broker_host, MQTT_TELEMETRY_PORT, root_topic,
             display_enabled ? "on" : "off");
    httpd_resp_set_status(req, "303 See Other");
    httpd_resp_set_hdr(req, "Location", "/");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

static esp_err_t oled_debug_post_handler(httpd_req_t *req)
{
    if (!web_admin_authorized(req)) {
        return ESP_OK;
    }

    oled_request_debug_toggle();
    httpd_resp_set_status(req, "303 See Other");
    httpd_resp_set_hdr(req, "Location", "/");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

static esp_err_t ota_post_handler(httpd_req_t *req)
{
    if (!web_admin_authorized(req)) {
        return ESP_OK;
    }

    set_ota_state(true, 0);

    if (req->content_len == 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Empty OTA image");
        set_ota_state(false, -1);
        return ESP_FAIL;
    }

    const esp_partition_t *update_partition = esp_ota_get_next_update_partition(NULL);
    if (!update_partition) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "No OTA partition");
        set_ota_state(false, -1);
        return ESP_FAIL;
    }

    esp_ota_handle_t ota_handle = 0;
    esp_err_t err = esp_ota_begin(update_partition, OTA_SIZE_UNKNOWN, &ota_handle);
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OTA begin failed");
        set_ota_state(false, -1);
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
            set_ota_state(false, -1);
            return ESP_FAIL;
        }

        err = esp_ota_write(ota_handle, buf, recv_len);
        if (err != ESP_OK) {
            esp_ota_abort(ota_handle);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OTA write failed");
            set_ota_state(false, -1);
            return ESP_FAIL;
        }
        remaining -= recv_len;
        size_t written = total - remaining;
        if (total > 0) {
            set_ota_state(true, (int)((written * 100U) / total));
        }
    }

    err = esp_ota_end(ota_handle);
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OTA end failed");
        set_ota_state(false, -1);
        return ESP_FAIL;
    }

    err = esp_ota_set_boot_partition(update_partition);
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OTA set boot partition failed");
        set_ota_state(false, -1);
        return ESP_FAIL;
    }

    set_ota_state(true, 100);
    httpd_resp_sendstr(req, "OK");
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
    return ESP_OK;
}

/* --------------------------------------------------------------------------
 * Server start
 * -------------------------------------------------------------------------- */

esp_err_t web_server_start(void)
{
    if (!s_server_mutex) {
        s_server_mutex = xSemaphoreCreateMutex();
        if (!s_server_mutex) {
            return ESP_ERR_NO_MEM;
        }
    }
    xSemaphoreTake(s_server_mutex, portMAX_DELAY);

    if (s_httpd) {
        ESP_LOGI(TAG, "Webserver already running");
        xSemaphoreGive(s_server_mutex);
        return ESP_OK;
    }

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 80;
    config.ctrl_port = 32768;
    config.stack_size = 8192;
    config.recv_wait_timeout = 15;
    config.send_wait_timeout = 15;
    config.lru_purge_enable = true;
    config.keep_alive_enable = false;

    esp_err_t err = httpd_start(&s_httpd, &config);
    if (err != ESP_OK) {
        xSemaphoreGive(s_server_mutex);
        return err;
    }

    httpd_uri_t root = {
        .uri      = "/",
        .method   = HTTP_GET,
        .handler  = root_get_handler,
        .user_ctx = NULL
    };
    err = httpd_register_uri_handler(s_httpd, &root);
    if (err != ESP_OK) goto register_failed;

    httpd_uri_t set = {
        .uri      = "/set",
        .method   = HTTP_POST,
        .handler  = set_post_handler,
        .user_ctx = NULL
    };
    err = httpd_register_uri_handler(s_httpd, &set);
    if (err != ESP_OK) goto register_failed;

    httpd_uri_t mqtt_display = {
        .uri      = "/mqtt/display",
        .method   = HTTP_POST,
        .handler  = mqtt_display_post_handler,
        .user_ctx = NULL
    };
    err = httpd_register_uri_handler(s_httpd, &mqtt_display);
    if (err != ESP_OK) goto register_failed;

    httpd_uri_t oled_debug = {
        .uri      = "/oled/debug",
        .method   = HTTP_POST,
        .handler  = oled_debug_post_handler,
        .user_ctx = NULL
    };
    err = httpd_register_uri_handler(s_httpd, &oled_debug);
    if (err != ESP_OK) goto register_failed;

    httpd_uri_t ota = {
        .uri      = "/ota",
        .method   = HTTP_POST,
        .handler  = ota_post_handler,
        .user_ctx = NULL
    };
    err = httpd_register_uri_handler(s_httpd, &ota);
    if (err != ESP_OK) goto register_failed;

    httpd_uri_t status_all = {
        .uri      = "/status/all",
        .method   = HTTP_GET,
        .handler  = status_all_get_handler,
        .user_ctx = NULL
    };
    err = httpd_register_uri_handler(s_httpd, &status_all);
    if (err != ESP_OK) goto register_failed;

    ESP_LOGI(TAG, "Webserver started on http://%s/", AP_IP_ADDR);
    xSemaphoreGive(s_server_mutex);
    return ESP_OK;

register_failed:
    ESP_LOGE(TAG, "Failed to register HTTP handler: %s", esp_err_to_name(err));
    httpd_stop(s_httpd);
    s_httpd = NULL;
    xSemaphoreGive(s_server_mutex);
    return err;
}
