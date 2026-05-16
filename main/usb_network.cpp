#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_heap_caps.h"
#include "tinyusb.h"
#include "tinyusb_default_config.h"
#include "tinyusb_net.h"
#include "tusb.h"
#include "lwip/esp_netif_net_stack.h"
#include "dhcpserver/dhcpserver_options.h"
#include "esp_timer.h"
#include "usb_network.h"

static const char* TAG = "USB_NET";
static esp_netif_t* s_usb_netif = NULL;
static bool s_usb_attached = false;
static bool s_usb_suspended = false;
static bool s_dhcp_started = false;
static esp_netif_ip_info_t s_usb_ip_info;
static esp_timer_handle_t s_dhcp_restart_timer = NULL;
static int s_tx_fail_count = 0;
static bool s_link_down = false;
static uint64_t s_usb_rx_total = 0;
static uint64_t s_usb_tx_total = 0;

static void check_heap(void)
{
    size_t free_heap = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    ESP_LOGI(TAG, "Heap free: %u", (unsigned)free_heap);
}

static void tx_buffer_free(void* buffer, void* ctx)
{
    free(buffer);
}

static esp_err_t netif_transmit(void* h, void* buffer, size_t len)
{
    if (!tud_ready() || s_link_down) {
        return ESP_OK;
    }

    esp_err_t ret = tinyusb_net_send_sync(buffer, (uint16_t)len, NULL, pdMS_TO_TICKS(500));
    if (ret != ESP_OK) {
        s_tx_fail_count++;
        if (s_tx_fail_count <= 3 || (s_tx_fail_count % 200) == 0) {
            ESP_LOGW(TAG, "TX failed: %d, len=%u (fail_cnt=%d, link_down=%d)",
                     ret, (unsigned)len, s_tx_fail_count, s_link_down);
        }
        if (!s_link_down && s_tx_fail_count > 30) {
            s_link_down = true;
            ESP_LOGW(TAG, "Link down after %d consecutive TX failures", s_tx_fail_count);
            tud_network_link_state(0, false);
        }
        return ESP_OK;
    }

    s_tx_fail_count = 0;
    s_usb_tx_total += len;
    return ESP_OK;
}

static esp_err_t netif_recv_callback(void* buffer, uint16_t len, void* ctx)
{
    if (!s_usb_netif) {
        return ESP_OK;
    }

    void* buf_copy = malloc(len);
    if (!buf_copy) {
        ESP_LOGE(TAG, "RX malloc(%u) OOM!", len);
        check_heap();
        return ESP_ERR_NO_MEM;
    }
    memcpy(buf_copy, buffer, len);

    esp_err_t ret = esp_netif_receive(s_usb_netif, buf_copy, len, NULL);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "RX esp_netif_receive err: %d, len=%u", ret, len);
    } else {
        s_usb_rx_total += len;
    }
    return ret;
}

static void l2_free(void* h, void* buffer)
{
    free(buffer);
}

static void apply_dhcp_options(void)
{
    if (!s_usb_netif) return;

    uint32_t lease_seconds = 3600;
    esp_netif_dhcps_option(s_usb_netif, ESP_NETIF_OP_SET, ESP_NETIF_IP_ADDRESS_LEASE_TIME,
                           &lease_seconds, sizeof(lease_seconds));

    uint8_t dns_enable = 0x02;
    esp_netif_dhcps_option(s_usb_netif, ESP_NETIF_OP_SET, ESP_NETIF_DOMAIN_NAME_SERVER,
                           &dns_enable, sizeof(dns_enable));

    esp_netif_dns_info_t dns;
    memset(&dns, 0, sizeof(dns));
    dns.ip.type = ESP_IPADDR_TYPE_V4;
    IP4_ADDR(&dns.ip.u_addr.ip4, 8, 8, 8, 8);
    esp_netif_set_dns_info(s_usb_netif, ESP_NETIF_DNS_MAIN, &dns);
}

static void dhcp_restart_timer_cb(void* arg)
{
    if (!s_usb_netif) return;

    esp_netif_action_start(s_usb_netif, NULL, 0, NULL);

    esp_err_t ret = esp_netif_dhcps_stop(s_usb_netif);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "DHCP stopped for restart");
    } else if (ret != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STOPPED) {
        ESP_LOGW(TAG, "DHCP stop err: %d", ret);
    }

    vTaskDelay(pdMS_TO_TICKS(200));

    ret = esp_netif_dhcps_start(s_usb_netif);
    if (ret == ESP_OK) {
        apply_dhcp_options();
        ESP_LOGI(TAG, "DHCP server restarted, options applied");
    } else {
        ESP_LOGW(TAG, "DHCP start err: %d", ret);
    }
}

static void ncm_init_callback(void* ctx)
{
    ESP_LOGI(TAG, "NCM interface (re)initialized by host");

    if (s_dhcp_started && s_dhcp_restart_timer) {
        ESP_LOGI(TAG, "Re-enumeration detected, restarting DHCP...");
        esp_timer_stop(s_dhcp_restart_timer);
        esp_timer_start_once(s_dhcp_restart_timer, 300000);
    }
}

static void usb_device_event_handler(tinyusb_event_t *event, void *arg)
{
    if (event->id == TINYUSB_EVENT_ATTACHED) {
        ESP_LOGI(TAG, "USB attached");
        s_usb_attached = true;
        s_usb_suspended = false;
    } else if (event->id == TINYUSB_EVENT_DETACHED) {
        ESP_LOGI(TAG, "USB detached");
        s_usb_attached = false;
        s_usb_suspended = false;
    }
#ifdef CONFIG_TINYUSB_SUSPEND_CALLBACK
    else if (event->id == TINYUSB_EVENT_SUSPENDED) {
        ESP_LOGI(TAG, "USB suspended (remote_wakeup=%d)", event->suspended.remote_wakeup);
        s_usb_suspended = true;
    }
#endif
#ifdef CONFIG_TINYUSB_RESUME_CALLBACK
    else if (event->id == TINYUSB_EVENT_RESUMED) {
        ESP_LOGI(TAG, "USB resumed");
        s_usb_suspended = false;

        tud_network_link_state(0, false);
        vTaskDelay(pdMS_TO_TICKS(50));
        tud_network_link_state(0, true);

        s_tx_fail_count = 0;
        s_link_down = false;

        if (s_dhcp_started && s_dhcp_restart_timer) {
            ESP_LOGI(TAG, "Resume detected, restarting DHCP...");
            esp_timer_stop(s_dhcp_restart_timer);
            esp_timer_start_once(s_dhcp_restart_timer, 500000);
        }
    }
#endif
}


esp_err_t usb_network_init(void)
{
    if (s_usb_netif != NULL) {
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Initializing USB NCM network...");

    const tinyusb_config_t tusb_cfg = {
        .port = TINYUSB_PORT_FULL_SPEED_0,
        .phy = {
            .skip_setup = false,
            .self_powered = false,
            .vbus_monitor_io = -1,
        },
        .task = TINYUSB_TASK_DEFAULT(),
        .descriptor = {
            .device = NULL,
            .qualifier = NULL,
            .string = NULL,
            .string_count = 0,
            .full_speed_config = NULL,
            .high_speed_config = NULL,
        },
        .event_cb = usb_device_event_handler,
        .event_arg = NULL,
    };
    ESP_ERROR_CHECK(tinyusb_driver_install(&tusb_cfg));
    ESP_LOGI(TAG, "TinyUSB installed");

    vTaskDelay(pdMS_TO_TICKS(100));

    const tinyusb_net_config_t net_config = {
        .mac_addr = {0x02, 0x02, 0x11, 0x22, 0x33, 0x01},
        .on_recv_callback = netif_recv_callback,
        .free_tx_buffer = tx_buffer_free,
        .on_init_callback = ncm_init_callback,
        .user_context = NULL,
    };
    ESP_ERROR_CHECK(tinyusb_net_init(&net_config));
    ESP_LOGI(TAG, "TinyUSB NCM config done");

    uint8_t lwip_mac[6] = {0x02, 0x02, 0x11, 0x22, 0x33, 0x01};

    memset(&s_usb_ip_info, 0, sizeof(s_usb_ip_info));
    IP4_ADDR(&s_usb_ip_info.ip, 192, 168, 5, 1);
    IP4_ADDR(&s_usb_ip_info.gw, 192, 168, 5, 1);
    IP4_ADDR(&s_usb_ip_info.netmask, 255, 255, 255, 0);

    esp_netif_inherent_config_t base_cfg = {
        .flags = (esp_netif_flags_t)(ESP_NETIF_FLAG_AUTOUP | ESP_NETIF_DHCP_SERVER),
        .mac = {0},
        .ip_info = &s_usb_ip_info,
        .get_ip_event = 0,
        .lost_ip_event = 0,
        .if_key = "usb_ncm",
        .if_desc = "usb ncm config device",
        .route_prio = 10,
        .bridge_info = NULL,
        .mtu = 0
    };

    esp_netif_driver_ifconfig_t driver_cfg = {
        .handle = (void*)1,
        .transmit = netif_transmit,
        .transmit_wrap = NULL,
        .driver_free_rx_buffer = l2_free,
        .driver_set_mac_filter = NULL,
    };

    esp_netif_config_t cfg = {
        .base = &base_cfg,
        .driver = &driver_cfg,
        .stack = _g_esp_netif_netstack_default_eth,
    };

    s_usb_netif = esp_netif_new(&cfg);
    if (s_usb_netif == NULL) {
        return ESP_FAIL;
    }
    esp_netif_set_mac(s_usb_netif, lwip_mac);
    apply_dhcp_options();
    esp_netif_action_start(s_usb_netif, NULL, 0, NULL);

    s_dhcp_started = true;

    ESP_LOGI(TAG, "USB Network ready: IP=192.168.5.1, GW=192.168.5.1, DNS=8.8.8.8");
    check_heap();

    const esp_timer_create_args_t timer_args = {
        .callback = dhcp_restart_timer_cb,
        .arg = NULL,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "dhcp_restart",
        .skip_unhandled_events = false,
    };
    esp_timer_create(&timer_args, &s_dhcp_restart_timer);

    return ESP_OK;
}

esp_netif_t* usb_network_get_netif(void)
{
    return s_usb_netif;
}

void usb_network_reconnect(void)
{
    if (!s_usb_netif) return;
    esp_netif_dhcps_start(s_usb_netif);
    ESP_LOGI(TAG, "USB reconnect done");
}

uint64_t usb_network_get_rx_total(void)
{
    return s_usb_rx_total;
}

uint64_t usb_network_get_tx_total(void)
{
    return s_usb_tx_total;
}