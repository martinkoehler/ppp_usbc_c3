/*
 * PPP-over-USB + WiFi SoftAP Router (ESP32-C3)
 *
 * MQTT telemetry subscriber for the broker running on the FRITZ!Box.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "mqtt_telemetry.h"
#include "ppp.h"

#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "esp_event.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "lwip/ip4_addr.h"
#include "mqtt_client.h"
#include "nvs.h"

#define MQTT_NVS_NAMESPACE "mqttcfg"
#define MQTT_NVS_AUTO_KEY "auto"
#define MQTT_NVS_BROKER_KEY "host"
#define MQTT_NVS_ROOT_KEY "root"
#define MQTT_TOPIC_MAX_LEN (MQTT_ROOT_TOPIC_MAX_LEN + 16)

static const char *TAG = "mqtt_telemetry";

static SemaphoreHandle_t s_mutex;
static TaskHandle_t s_task;
static esp_mqtt_client_handle_t s_client;
static mqtt_telemetry_config_t s_config = {
    .broker_auto = true,
    .broker_host = "",
    .root_topic = MQTT_DEFAULT_ROOT_TOPIC,
};
static bool s_reconfigure_requested;
static bool s_broker_connected;
static char s_power_topic[MQTT_TOPIC_MAX_LEN];
static char s_connected_topic[MQTT_TOPIC_MAX_LEN];
static char s_broker_uri[64];
static char s_active_broker_host[MQTT_BROKER_HOST_MAX_LEN + 1];
static char s_power[64] = "N/A";
static int64_t s_power_updated_us;
static int8_t s_obk_connected_state = -1;
static char s_in_topic[MQTT_TOPIC_MAX_LEN];
static char s_in_payload[64];
static size_t s_in_len;

static void trim_whitespace(char *value)
{
    char *start;
    char *end;
    size_t len;
    if (!value || !*value) return;
    start = value;
    while (*start && isspace((unsigned char)*start)) start++;
    end = start + strlen(start);
    while (end > start && isspace((unsigned char)end[-1])) end--;
    len = (size_t)(end - start);
    if (start != value && len > 0) memmove(value, start, len);
    value[len] = 0;
}

static bool valid_broker_host(const char *host)
{
    ip4_addr_t address;
    return host && *host && strlen(host) <= MQTT_BROKER_HOST_MAX_LEN &&
           ip4addr_aton(host, &address) != 0;
}

static bool valid_root_topic(const char *root)
{
    size_t len;
    if (!root) return false;
    len = strlen(root);
    if (len == 0 || len > MQTT_ROOT_TOPIC_MAX_LEN) return false;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)root[i];
        if (!(isalnum(c) || c == '-' || c == '_' || c == '.')) return false;
    }
    return true;
}

static void load_config_from_nvs(void)
{
    nvs_handle_t nvs;
    if (nvs_open(MQTT_NVS_NAMESPACE, NVS_READONLY, &nvs) != ESP_OK) return;

    uint8_t auto_mode = 1;
    char host[sizeof(s_config.broker_host)] = "";
    char root[sizeof(s_config.root_topic)] = MQTT_DEFAULT_ROOT_TOPIC;
    size_t host_len = sizeof(host);
    size_t root_len = sizeof(root);
    if (nvs_get_u8(nvs, MQTT_NVS_AUTO_KEY, &auto_mode) != ESP_OK) {
        auto_mode = 1;
    }
    if (nvs_get_str(nvs, MQTT_NVS_BROKER_KEY, host, &host_len) != ESP_OK ||
        (host[0] && !valid_broker_host(host))) {
        host[0] = 0;
    }
    if (nvs_get_str(nvs, MQTT_NVS_ROOT_KEY, root, &root_len) != ESP_OK ||
        !valid_root_topic(root)) {
        strlcpy(root, MQTT_DEFAULT_ROOT_TOPIC, sizeof(root));
    }
    nvs_close(nvs);
    s_config.broker_auto = auto_mode != 0 || !valid_broker_host(host);
    strlcpy(s_config.broker_host, host, sizeof(s_config.broker_host));
    strlcpy(s_config.root_topic, root, sizeof(s_config.root_topic));
}

static esp_err_t save_config_to_nvs(bool auto_mode, const char *host,
                                    const char *root)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(MQTT_NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK) return err;
    err = nvs_set_u8(nvs, MQTT_NVS_AUTO_KEY, auto_mode ? 1 : 0);
    if (err == ESP_OK) err = nvs_set_str(nvs, MQTT_NVS_BROKER_KEY, host);
    if (err == ESP_OK) err = nvs_set_str(nvs, MQTT_NVS_ROOT_KEY, root);
    if (err == ESP_OK) err = nvs_commit(nvs);
    nvs_close(nvs);
    return err;
}

static void set_broker_connected(bool connected)
{
    if (!s_mutex) return;
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(20)) == pdTRUE) {
        s_broker_connected = connected;
        if (connected) s_obk_connected_state = -1;
        xSemaphoreGive(s_mutex);
    }
}

static void handle_message(const char *topic, const char *data, int len)
{
    if (!topic || !data || len <= 0 || !s_mutex) return;
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(20)) != pdTRUE) return;

    if (strcmp(topic, s_power_topic) == 0) {
        int copy = len;
        if (copy >= (int)sizeof(s_power)) copy = sizeof(s_power) - 1;
        memcpy(s_power, data, (size_t)copy);
        s_power[copy] = 0;
        s_power_updated_us = esp_timer_get_time();
    } else if (strcmp(topic, s_connected_topic) == 0) {
        char state[16];
        int copy = len;
        if (copy >= (int)sizeof(state)) copy = sizeof(state) - 1;
        memcpy(state, data, (size_t)copy);
        state[copy] = 0;
        trim_whitespace(state);
        if (strcasecmp(state, "online") == 0) s_obk_connected_state = 1;
        else if (strcasecmp(state, "offline") == 0) s_obk_connected_state = 0;
    }
    xSemaphoreGive(s_mutex);
}

static void mqtt_event_handler(void *handler_args, esp_event_base_t base,
                               int32_t event_id, void *event_data)
{
    (void)handler_args;
    (void)base;
    esp_mqtt_event_handle_t event = event_data;
    if (!event) return;

    switch (event_id) {
        case MQTT_EVENT_CONNECTED:
            set_broker_connected(true);
            esp_mqtt_client_subscribe(event->client, s_power_topic, 0);
            esp_mqtt_client_subscribe(event->client, s_connected_topic, 0);
            ESP_LOGI(TAG, "Connected; subscribed to %s and %s",
                     s_power_topic, s_connected_topic);
            break;
        case MQTT_EVENT_DISCONNECTED:
        case MQTT_EVENT_ERROR:
            set_broker_connected(false);
            break;
        case MQTT_EVENT_DATA: {
            if (event->current_data_offset == 0) {
                size_t topic_len = event->topic_len < (int)(sizeof(s_in_topic) - 1)
                    ? (size_t)event->topic_len : sizeof(s_in_topic) - 1;
                memcpy(s_in_topic, event->topic, topic_len);
                s_in_topic[topic_len] = 0;
                s_in_len = 0;
            }
            size_t copy = (size_t)event->data_len;
            if (s_in_len + copy >= sizeof(s_in_payload)) {
                copy = s_in_len < sizeof(s_in_payload) - 1
                    ? sizeof(s_in_payload) - 1 - s_in_len : 0;
            }
            if (copy > 0) {
                memcpy(s_in_payload + s_in_len, event->data, copy);
                s_in_len += copy;
            }
            if (event->current_data_offset + event->data_len >= event->total_data_len) {
                s_in_payload[s_in_len] = 0;
                handle_message(s_in_topic, s_in_payload, (int)s_in_len);
            }
            break;
        }
        default:
            break;
    }
}

static void destroy_client(void)
{
    if (!s_client) return;
    esp_mqtt_client_stop(s_client);
    esp_mqtt_client_destroy(s_client);
    s_client = NULL;
    s_active_broker_host[0] = 0;
    set_broker_connected(false);
}

static esp_err_t create_client(const char *broker_host)
{
    mqtt_telemetry_config_t config;
    mqtt_telemetry_get_config(&config);
    snprintf(s_broker_uri, sizeof(s_broker_uri), "mqtt://%s:%d",
             broker_host, MQTT_TELEMETRY_PORT);
    snprintf(s_power_topic, sizeof(s_power_topic), "%s/power/get",
             config.root_topic);
    snprintf(s_connected_topic, sizeof(s_connected_topic), "%s/connected",
             config.root_topic);

    esp_mqtt_client_config_t mqtt_config = {
        .broker.address.uri = s_broker_uri,
    };
    s_client = esp_mqtt_client_init(&mqtt_config);
    if (!s_client) return ESP_ERR_NO_MEM;
    esp_err_t err = esp_mqtt_client_register_event(
        s_client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
    if (err == ESP_OK) err = esp_mqtt_client_start(s_client);
    if (err != ESP_OK) {
        esp_mqtt_client_destroy(s_client);
        s_client = NULL;
        return err;
    }
    strlcpy(s_active_broker_host, broker_host,
            sizeof(s_active_broker_host));
    ESP_LOGI(TAG, "MQTT client started for %s", s_broker_uri);
    return ESP_OK;
}

static void mqtt_task(void *arg)
{
    (void)arg;
    for (;;) {
        bool reconfigure = false;
        char desired_host[MQTT_BROKER_HOST_MAX_LEN + 1];
        if (xSemaphoreTake(s_mutex, portMAX_DELAY) == pdTRUE) {
            reconfigure = s_reconfigure_requested;
            s_reconfigure_requested = false;
            xSemaphoreGive(s_mutex);
        }
        if (reconfigure) destroy_client();
        bool broker_available = mqtt_telemetry_get_effective_broker_host(
            desired_host, sizeof(desired_host));
        if (s_client && (!broker_available ||
                         strcmp(desired_host, s_active_broker_host) != 0)) {
            destroy_client();
        }
        if (!s_client && broker_available) {
            esp_err_t err = create_client(desired_host);
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "Unable to start MQTT client: %s",
                         esp_err_to_name(err));
            }
        }
        vTaskDelay(pdMS_TO_TICKS(s_client ? 500 : 2000));
    }
}

esp_err_t mqtt_telemetry_start(void)
{
    if (s_task) return ESP_OK;
    if (!s_mutex) {
        s_mutex = xSemaphoreCreateMutex();
        if (!s_mutex) return ESP_ERR_NO_MEM;
    }
    load_config_from_nvs();
    if (xTaskCreate(mqtt_task, "mqtt_telemetry", 6144, NULL, 7, &s_task) != pdPASS) {
        s_task = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

bool mqtt_telemetry_is_broker_connected(void)
{
    bool connected = false;
    if (s_mutex && xSemaphoreTake(s_mutex, pdMS_TO_TICKS(5)) == pdTRUE) {
        connected = s_broker_connected;
        xSemaphoreGive(s_mutex);
    }
    return connected;
}

void mqtt_telemetry_get_config(mqtt_telemetry_config_t *out)
{
    if (!out) return;
    if (s_mutex && xSemaphoreTake(s_mutex, portMAX_DELAY) == pdTRUE) {
        *out = s_config;
        xSemaphoreGive(s_mutex);
    } else {
        *out = s_config;
    }
}

bool mqtt_telemetry_get_effective_broker_host(char *out, size_t out_len)
{
    if (!out || out_len == 0) return false;
    out[0] = 0;
    mqtt_telemetry_config_t config;
    mqtt_telemetry_get_config(&config);
    if (!config.broker_auto) {
        strlcpy(out, config.broker_host, out_len);
        return valid_broker_host(out);
    }

    ip4_addr_t gateway = {0};
    ppp_get_ip_info(NULL, &gateway, NULL);
    if (gateway.addr == 0) return false;
    return ip4addr_ntoa_r(&gateway, out, (int)out_len) != NULL;
}

esp_err_t mqtt_telemetry_set_config(bool broker_auto, const char *broker_host,
                                    const char *root_topic)
{
    if (!broker_host || (!broker_auto && !valid_broker_host(broker_host)) ||
        (broker_host[0] && !valid_broker_host(broker_host)) ||
        !valid_root_topic(root_topic)) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = save_config_to_nvs(broker_auto, broker_host, root_topic);
    if (err != ESP_OK) return err;
    if (xSemaphoreTake(s_mutex, portMAX_DELAY) != pdTRUE) return ESP_ERR_TIMEOUT;
    s_config.broker_auto = broker_auto;
    strlcpy(s_config.broker_host, broker_host, sizeof(s_config.broker_host));
    strlcpy(s_config.root_topic, root_topic, sizeof(s_config.root_topic));
    strlcpy(s_power, "N/A", sizeof(s_power));
    s_power_updated_us = 0;
    s_obk_connected_state = -1;
    s_broker_connected = false;
    s_reconfigure_requested = true;
    xSemaphoreGive(s_mutex);
    return ESP_OK;
}

void mqtt_telemetry_get_power(char *out, size_t out_len)
{
    if (!out || out_len == 0) return;
    strlcpy(out, "N/A", out_len);
    if (s_mutex && xSemaphoreTake(s_mutex, pdMS_TO_TICKS(5)) == pdTRUE) {
        int64_t age = s_power_updated_us == 0
            ? INT64_MAX : esp_timer_get_time() - s_power_updated_us;
        if (s_broker_connected &&
            age <= (int64_t)OBK_POWER_STALE_TIMEOUT_MS * 1000) {
            strlcpy(out, s_power, out_len);
        }
        xSemaphoreGive(s_mutex);
    }
}

int mqtt_telemetry_get_obk_connected_state(void)
{
    int state = -2;
    if (s_mutex && xSemaphoreTake(s_mutex, pdMS_TO_TICKS(5)) == pdTRUE) {
        state = s_broker_connected ? s_obk_connected_state : -2;
        xSemaphoreGive(s_mutex);
    }
    return state;
}
