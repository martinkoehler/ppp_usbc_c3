/*
 * PPP-over-USB + WiFi SoftAP Router (ESP32-C3)
 *
 * PPP-over-USB Serial/JTAG module.
 *
 * Author: Martin Köhler [martinkoehler]
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "ppp.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"

#include "esp_log.h"
#include "esp_netif.h"

#include "lwip/err.h"
#include "lwip/sys.h"
#include "lwip/netif.h"
#include "lwip/ip4_addr.h"
#include "netif/ppp/ppp.h"
#include "netif/ppp/pppapi.h"
#include "netif/ppp/pppos.h"
#include "netif/ppp/ppp_impl.h"

#include "driver/usb_serial_jtag.h"

static const char *TAG = "ppp";

/* PPP config */
#define PPP_MRU_MTU 512

/* PPP state */
static ppp_pcb *ppp = NULL;
static struct netif ppp_netif;

static EventGroupHandle_t s_event_group;
#define PPP_CONNECTED_BIT BIT0
#define PPP_DISCONN_BIT   BIT1

static bool s_ppp_up = false;

/**
 * @brief PPP RX task: reads bytes from USB Serial/JTAG and feeds to PPP stack.
 */
static void ppp_usb_rx_task(void *arg)
{
    (void)arg;
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
 * @brief PPP Output callback: Called by lwIP when PPP needs to transmit data.
 */
static u32_t ppp_output_cb(ppp_pcb *pcb, const void *data, u32_t len, void *ctx)
{
    (void)pcb; (void)ctx;

    int written = usb_serial_jtag_write_bytes(data, len, pdMS_TO_TICKS(1000));
    if (written < 0) {
        ESP_LOGW(TAG, "USB write error");
        return 0;
    }
    return (u32_t)written;
}

/**
 * @brief PPP link status callback.
 *
 * Updates s_ppp_up and sets event bits for reconnect loop.
 */
static void ppp_status_cb(ppp_pcb *pcb, int err_code, void *ctx)
{
    (void)ctx;

    switch (err_code) {
        case PPPERR_NONE: {
            ESP_LOGI(TAG, "PPP connected");
            ip4_addr_t ip = pcb->netif->ip_addr.u_addr.ip4;
            ip4_addr_t gw = pcb->netif->gw.u_addr.ip4;
            ip4_addr_t nm = pcb->netif->netmask.u_addr.ip4;

            ESP_LOGI(TAG, "PPP IP: " IPSTR, IP2STR(&ip));
            ESP_LOGI(TAG, "PPP GW: " IPSTR, IP2STR(&gw));
            ESP_LOGI(TAG, "PPP NM: " IPSTR, IP2STR(&nm));

            /* Re-assert PPP as the default route after link-up. */
            // pppapi_set_default(ppp);

            s_ppp_up = true;
            xEventGroupSetBits(s_event_group, PPP_CONNECTED_BIT);
            break;
        }
        case PPPERR_USER:
        default:
            ESP_LOGW(TAG, "PPP error/closed: %d", err_code);
            s_ppp_up = false;
            xEventGroupSetBits(s_event_group, PPP_DISCONN_BIT);
            break;
    }
}

/**
 * @brief Background reconnect loop.
 *
 * This reproduces the while(1) wait/reconnect logic from app_main,
 * but keeps it private inside the PPP module.
 */
static void ppp_reconnect_task(void *arg)
{
    (void)arg;

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

void ppp_usb_start(void)
{
    if (ppp) {
        ESP_LOGI(TAG, "PPP already started");
        return;
    }

    s_event_group = xEventGroupCreate();
    configASSERT(s_event_group);

    usb_serial_jtag_driver_config_t usb_cfg = {
        .tx_buffer_size = 2048,
        .rx_buffer_size = 2048,
    };
    ESP_ERROR_CHECK(usb_serial_jtag_driver_install(&usb_cfg));

    ESP_LOGI(TAG, "Starting PPP over USB Serial/JTAG...");

    memset(&ppp_netif, 0, sizeof(ppp_netif));
    ppp = pppapi_pppos_create(&ppp_netif, ppp_output_cb, ppp_status_cb, NULL);
    configASSERT(ppp);

    /* Apply constrained MRU/MTU */
    ppp_send_config(ppp, PPP_MRU_MTU, 0xFFFFFFFF, 0, 0);
    ppp_recv_config(ppp, PPP_MRU_MTU, 0xFFFFFFFF, 0, 0);
    ppp->netif->mtu = PPP_MRU_MTU;

    /* No authentication; peer provides DNS */
    ppp_set_auth(ppp, PPPAUTHTYPE_NONE, NULL, NULL);
    ppp_set_usepeerdns(ppp, true);

    /* Make PPP default route in lwIP */
    pppapi_set_default(ppp);

    xTaskCreate(ppp_usb_rx_task, "ppp_usb_rx", 4096, NULL, 10, NULL);
    xTaskCreate(ppp_reconnect_task, "ppp_reconn", 4096, NULL, 9, NULL);

    pppapi_connect(ppp, 0);
}

bool ppp_is_up(void)
{
    return s_ppp_up;
}

void ppp_get_ip_info(ip4_addr_t *ip, ip4_addr_t *gw, ip4_addr_t *nm)
{
    if (ip) ip->addr = 0;
    if (gw) gw->addr = 0;
    if (nm) nm->addr = 0;

    if (ppp && ppp->netif) {
        if (ip) *ip = ppp->netif->ip_addr.u_addr.ip4;
        if (gw) *gw = ppp->netif->gw.u_addr.ip4;
        if (nm) *nm = ppp->netif->netmask.u_addr.ip4;
    }
}

ip4_addr_t ppp_get_ip(void)
{
    ip4_addr_t ip = { .addr = 0 };
    if (ppp && ppp->netif) {
        ip = ppp->netif->ip_addr.u_addr.ip4;
    }
    return ip;
}
