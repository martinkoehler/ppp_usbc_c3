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

#include "lwip/err.h"
#include "lwip/sys.h"
#include "lwip/netif.h"
#include "lwip/ip4_addr.h"

// PPP headers
#include "netif/ppp/ppp.h"
#include "netif/ppp/pppapi.h"
#include "netif/ppp/pppos.h"
#include "netif/ppp/ppp_impl.h"


// USB Serial/JTAG
#include "driver/usb_serial_jtag.h"

// WiFi SoftAP
#include "esp_wifi.h"
#include "esp_mac.h"

// HTTP server
#include "esp_http_server.h"

// MQTT broker (Mosquitto port)
#include "mosq_broker.h"
// -------- MQTT broker state --------
static TaskHandle_t s_broker_task = NULL;
static bool s_broker_running = false;
#define MQTT_BROKER_PORT 1883

static const char *TAG = "ppp_usb_ap_web";

// --------- DEFAULT CONFIG (used if NVS empty) ----------
#define DEFAULT_AP_SSID     "ESP32C3-PPP-AP"
#define DEFAULT_AP_PASS     "12345678"
#define AP_CHANNEL          1
#define AP_MAX_CONN         4

// SoftAP subnet (different from PPP subnet!)
#define AP_IP_ADDR     "192.168.4.1"
#define AP_GATEWAY     "192.168.4.1"
#define AP_NETMASK     "255.255.255.0"
// ------------------------------------------------------

// NVS keys
#define NVS_NS   "apcfg"
#define NVS_KEY_SSID "ssid"
#define NVS_KEY_PASS "pass"

// Current AP creds in RAM
static char g_ap_ssid[33] = DEFAULT_AP_SSID;
static char g_ap_pass[65] = DEFAULT_AP_PASS;

// --- PPP state ---
static ppp_pcb *ppp = NULL;
static struct netif ppp_netif;

// esp-netif handle for AP
static esp_netif_t *ap_netif = NULL;

// HTTP server handle
static httpd_handle_t s_httpd = NULL;

// Event bits
static EventGroupHandle_t s_event_group;
#define PPP_CONNECTED_BIT BIT0
#define PPP_DISCONN_BIT   BIT1
#define PPP_MRU_MTU 512
// ======================================================
//                 NVS AP CONFIG
// ======================================================

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

// ======================================================
//              PPP over USB Serial/JTAG
// ======================================================

// RX task: reads from USB CDC, feeds PPP
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

// Output callback: PPP -> USB CDC
static u32_t ppp_output_cb(ppp_pcb *pcb, const void *data, u32_t len, void *ctx)
{
    int written = usb_serial_jtag_write_bytes(data, len, pdMS_TO_TICKS(1000));
    if (written < 0) {
        ESP_LOGW(TAG, "USB write error");
        return 0;
    }
    return (u32_t)written;
}

// PPP link status
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

	    ESP_LOGI(TAG, "PPP connected -> restarting webserver to bind on all netifs");

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

// ======================================================
//                 WiFi SoftAP
// ======================================================

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
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

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                               &wifi_event_handler, NULL));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));

    // Configure static AP IP
    ESP_ERROR_CHECK(esp_netif_dhcps_stop(ap_netif));
    esp_netif_ip_info_t ip_info = {0};
    ip4_addr_t tmp;

    ip4addr_aton(AP_IP_ADDR, &tmp);   ip_info.ip.addr = tmp.addr;
    ip4addr_aton(AP_GATEWAY, &tmp);   ip_info.gw.addr = tmp.addr;
    ip4addr_aton(AP_NETMASK, &tmp);   ip_info.netmask.addr = tmp.addr;

    ESP_ERROR_CHECK(esp_netif_set_ip_info(ap_netif, &ip_info));
    ESP_ERROR_CHECK(esp_netif_dhcps_start(ap_netif));

    // Apply AP config from globals
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

// ======================================================
//                  HTTP SERVER
// ======================================================

static void append_station_table(char *out, size_t out_len)
{
    wifi_sta_list_t sta_list = {0};
    esp_wifi_ap_get_sta_list(&sta_list);

    strlcat(out, "<h3>Connected Clients</h3>", out_len);

    if (sta_list.num == 0) {
        strlcat(out, "<p>No clients connected.</p>", out_len);
        return;
    }

    // Prepare MAC->IP pairs for DHCP server query
    esp_netif_pair_mac_ip_t pairs[AP_MAX_CONN];
    int n = sta_list.num;
    if (n > AP_MAX_CONN) n = AP_MAX_CONN;

    for (int i = 0; i < n; i++) {
        memcpy(pairs[i].mac, sta_list.sta[i].mac, 6);
        pairs[i].ip.addr = 0; // will be filled by dhcps
    }

    // Ask DHCP server for IPs corresponding to these MACs
    esp_err_t err = esp_netif_dhcps_get_clients_by_mac(ap_netif, n, pairs);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "esp_netif_dhcps_get_clients_by_mac failed: %s", esp_err_to_name(err));
    }

    strlcat(out,
            "<table border='1' cellpadding='4'>"
            "<tr><th>#</th><th>MAC</th><th>IP (DHCP)</th></tr>",
            out_len);

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

static void append_mqtt_panel(char *out, size_t out_len)
{
    // PPP info if available
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

    // Connection hints
    strlcat(out, "<p><b>Connect from WiFi AP clients:</b><br>", out_len);
    snprintf(line, sizeof(line),
             "<code>mqtt://%s:%d</code></p>",
             AP_IP_ADDR, MQTT_BROKER_PORT);
    strlcat(out, line, out_len);

    strlcat(out, "<p><b>Connect from Linux PC over PPP:</b><br>", out_len);
    if (ppp_ip.addr != 0) {
        snprintf(line, sizeof(line),
                 "<code>mqtt://%s:%d</code></p>",
                 ip4addr_ntoa(&ppp_ip), MQTT_BROKER_PORT);
    } else {
        snprintf(line, sizeof(line),
                 "<i>PPP not up yet.</i></p>");
    }
    strlcat(out, line, out_len);
}


static esp_err_t root_get_handler(httpd_req_t *req)
{
    // Keep stack tiny: build HTML on heap
    const size_t page_len = 4096;
    char *page = (char *)malloc(page_len);
    if (!page) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
        return ESP_FAIL;
    }
    page[0] = 0;

    // PPP info (if up)
    ip4_addr_t ppp_ip = {0}, ppp_gw = {0}, ppp_nm = {0};
    if (ppp && ppp->netif) {
        ppp_ip = ppp->netif->ip_addr.u_addr.ip4;
        ppp_gw = ppp->netif->gw.u_addr.ip4;
        ppp_nm = ppp->netif->netmask.u_addr.ip4;
    }

    // AP info
    esp_netif_ip_info_t ap_info = {0};
    esp_netif_get_ip_info(ap_netif, &ap_info);

    // Header + status + form
    snprintf(page, page_len,
        "<!doctype html><html><head>"
        "<meta charset='utf-8'>"
        "<meta name='viewport' content='width=device-width, initial-scale=1'>"
        "<title>ESP32C3 PPP Router</title>"
        "<style>"
        "body{font-family:sans-serif;margin:20px;}"
        "table{border-collapse:collapse;}"
        "th,td{border:1px solid #ccc;padding:6px 10px;}"
        "input{padding:6px;margin:4px 0;}"
        "</style>"
        "</head><body>"
        "<h2>ESP32-C3 PPP-over-USB + SoftAP Router (no NAT)</h2>"

        "<h3>Access Point</h3>"
        "<p><b>SSID:</b> %s<br>"
        "<b>Password:</b> %s</p>"
        "<p><b>AP IP:</b> " IPSTR "<br>"
        "<b>AP Netmask:</b> " IPSTR "</p>"

        "<h3>PPP Link</h3>"
        "<p><b>PPP IP:</b> " IPSTR "<br>"
        "<b>PPP GW:</b> " IPSTR "<br>"
        "<b>PPP Netmask:</b> " IPSTR "</p>"

        "<hr>"
        "<h3>Change AP SSID / Password</h3>"
        "<form method='POST' action='/set'>"
        "SSID:<br><input name='ssid' maxlength='32' value='%s'><br>"
        "Password:<br><input name='pass' maxlength='64' value='%s'><br>"
        "<small>Empty password = open network. WPA2 requires ≥8 chars.</small><br><br>"
        "<input type='submit' value='Save & Restart AP'>"
        "</form>"
        "<hr>",
        g_ap_ssid, g_ap_pass,
        IP2STR(&ap_info.ip), IP2STR(&ap_info.netmask),
        IP2STR(&ppp_ip), IP2STR(&ppp_gw), IP2STR(&ppp_nm),
        g_ap_ssid, g_ap_pass
    );

        // MQTT Broker panel
    append_mqtt_panel(page, page_len);

    strlcat(page, "<hr>", page_len);

    // Connected clients table (uses your DHCP MAC->IP helper)
    append_station_table(page, page_len);

    strlcat(page, "</body></html>", page_len);

    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, page, HTTPD_RESP_USE_STRLEN);

    free(page);
    return ESP_OK;
}




static void url_decode_inplace(char *s)
{
    // minimal www-form decode: + -> space, %HH -> byte
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

    // parse "ssid=...&pass=..."
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

    // Save to NVS
    esp_err_t err = save_ap_config_to_nvs(ssid, pass);
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "NVS save failed");
        return ESP_FAIL;
    }

    // Update globals + restart AP
    strlcpy(g_ap_ssid, ssid, sizeof(g_ap_ssid));
    strlcpy(g_ap_pass, pass, sizeof(g_ap_pass));
    apply_ap_config_and_restart();

    // redirect back to /
    httpd_resp_set_status(req, "303 See Other");
    httpd_resp_set_hdr(req, "Location", "/");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

static void start_webserver(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 80;
    config.ctrl_port = 32768; // avoid clashes in some setups
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

static void restart_webserver(void)
{
    if (s_httpd) {
        httpd_stop(s_httpd);
        s_httpd = NULL;
    }
    start_webserver();
}

// ======================================================
//                  MQTT Broker
// ======================================================

static void mqtt_broker_task(void *arg)
{
    struct mosq_broker_config cfg = {
        .host = "0.0.0.0",          // listen on all IPv4 interfaces (AP + PPP)
        .port = MQTT_BROKER_PORT,
        .tls_cfg = NULL,            // plain TCP for now
        .handle_message_cb = NULL   // optional: hook messages if you want
    };

    ESP_LOGI(TAG, "Mosquitto broker starting on port %d (host=%s)...",
             cfg.port, cfg.host);

    s_broker_running = true;
    int rc = mosq_broker_run(&cfg);   // blocks until stopped
    s_broker_running = false;

    ESP_LOGW(TAG, "Mosquitto broker exited rc=%d", rc);
    s_broker_task = NULL;
    vTaskDelete(NULL);
}

static void start_mqtt_broker_deferred(void)
{
    if (s_broker_task || s_broker_running) {
        ESP_LOGI(TAG, "Broker already running");
        return;
    }

    // Run broker in its own task; needs ~4–5kB stack minimum, 6–8kB is safer.
    xTaskCreate(
        mqtt_broker_task,
        "mosq_broker",
        8192,          // stack
        NULL,
        8,             // priority (below WiFi/PPP tasks)
        &s_broker_task
    );
}

/*
static void stop_mqtt_broker(void)
{
    if (!s_broker_running) return;
    ESP_LOGI(TAG, "Stopping Mosquitto broker...");
    mosq_broker_stop();  // causes mosq_broker_run() to return
} */


// ======================================================
//                      app_main
// ======================================================

void app_main(void)
{
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    s_event_group = xEventGroupCreate();

    // Load AP config from flash
    load_ap_config_from_nvs();

    // Start SoftAP (creates AP netif)
    wifi_init_softap();

    // Do not start webserver yet
    // start_webserver();

    // Init USB Serial/JTAG driver for PPP
    usb_serial_jtag_driver_config_t usb_cfg = {
        .tx_buffer_size = 2048,
        .rx_buffer_size = 2048,
    };
    ESP_ERROR_CHECK(usb_serial_jtag_driver_install(&usb_cfg));

    ESP_LOGI(TAG, "Starting PPP over USB Serial/JTAG...");

    // Create PPPoS interface
    memset(&ppp_netif, 0, sizeof(ppp_netif));
    ppp = pppapi_pppos_create(&ppp_netif, ppp_output_cb, ppp_status_cb, NULL);
    assert(ppp);

    // tell lwIP PPP to negotiate small MRU/MTU
    ppp_send_config(ppp, PPP_MRU_MTU, 0xFFFFFFFF, 0, 0);
    ppp_recv_config(ppp, PPP_MRU_MTU, 0xFFFFFFFF, 0, 0);

    ppp->netif->mtu = PPP_MRU_MTU;   // clamp outgoing packet size

    // No auth (matches your pppd "noauth")
    ppp_set_auth(ppp, PPPAUTHTYPE_NONE, NULL, NULL);
    ppp_set_usepeerdns(ppp, true);

    // Make PPP default route
    pppapi_set_default(ppp);

    // Start PPP RX task
    xTaskCreate(ppp_usb_rx_task, "ppp_usb_rx", 4096, NULL, 10, NULL);

    // Connect PPP
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
	    // safe here (app task), not tcpip thread
            ESP_LOGI(TAG, "Restarting webserver now that PPP is up");
            restart_webserver();

	    ESP_LOGI(TAG, "PPP connected -> starting MQTT broker");
            start_mqtt_broker_deferred();
        }

        if (bits & PPP_DISCONN_BIT) {
            ESP_LOGW(TAG, "PPP link down, reconnecting in 2s...");
            vTaskDelay(pdMS_TO_TICKS(2000));
            pppapi_connect(ppp, 0);
        }
    }
}

