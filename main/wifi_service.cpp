#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_mac.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "lwip/inet.h"
#include "lwip/netif.h"
#include "lwip/lwip_napt.h"
#include "lwip/tcpip.h"
#include "wifi_service.h"
#include "usb_network.h"

static const char* TAG = "WIFI_SRV";
static const char* NVS_NAMESPACE = "wifi_cfg";
static const char* NVS_KEY_SSID = "ssid";
static const char* NVS_KEY_PASS = "password";
static const char* NVS_KEY_MODE = "mode";
static const char* NVS_KEY_AP_SSID = "ap_ssid";
static const char* NVS_KEY_AP_PASS = "ap_pass";

#define RSSI_TIMER_PERIOD_US    3000000
#define STATS_TIMER_PERIOD_US   1000000
#define CMD_QUEUE_SIZE          8
#define CMD_TASK_STACK_SIZE     4096
#define CMD_TASK_PRIORITY       5
#define WIFI_CONNECTED_BIT      BIT0
#define WIFI_FAIL_BIT           BIT1
#define AP_MAX_CONNECTIONS      4
#define AP_DEFAULT_CHANNEL      1
#define DEFAULT_AP_SSID         "ESP32-S3-AP"
#define DEFAULT_AP_PASSWORD     "12345678"

static bool           s_sta_active      = true;
static bool           s_ap_active       = false;
static wifi_state_t   s_sta_state       = WIFI_STATE_DISCONNECTED;
static char           s_sta_ip[16]      = "0.0.0.0";
static int            s_sta_rssi        = 0;
static bool           s_sta_was_connected = false;
static bool           s_napt_enabled    = false;
static int            s_ap_clients      = 0;
static char           s_ap_ssid[33]     = DEFAULT_AP_SSID;
static char           s_ap_password[65] = DEFAULT_AP_PASSWORD;
static EventGroupHandle_t s_wifi_events = NULL;
static esp_netif_t*   s_sta_netif       = NULL;
static esp_netif_t*   s_ap_netif        = NULL;
static esp_timer_handle_t s_rssi_timer  = NULL;
static esp_timer_handle_t s_stats_timer = NULL;
static QueueHandle_t  s_cmd_queue       = NULL;
static SemaphoreHandle_t s_status_mutex = NULL;

static dhcp_client_info_t s_dhcp_clients[DHCP_CLIENT_MAX];
static int                s_dhcp_client_count = 0;
static SemaphoreHandle_t  s_dhcp_mutex = NULL;

static wifi_scan_item_t    s_scan_cache[WIFI_SCAN_MAX_RESULTS];
static int                 s_scan_count = 0;
static bool                s_scan_ready = false;
static bool                s_scan_running = false;

static void collect_scan_results(void);

static struct netif* find_netif_by_name(const char* name);

static uint64_t s_sta_down_last = 0;
static uint64_t s_sta_up_last   = 0;
static uint64_t s_ap_down_last  = 0;
static uint64_t s_ap_up_last    = 0;
static uint64_t s_usb_down_last = 0;
static uint64_t s_usb_up_last   = 0;
static uint32_t s_sta_down_bps  = 0;
static uint32_t s_sta_up_bps    = 0;
static uint32_t s_ap_down_bps   = 0;
static uint32_t s_ap_up_bps     = 0;
static uint32_t s_usb_down_bps  = 0;
static uint32_t s_usb_up_bps    = 0;

static netif_linkoutput_fn s_sta_orig_linkoutput = NULL;
static netif_linkoutput_fn s_ap_orig_linkoutput  = NULL;
static netif_input_fn     s_sta_orig_input       = NULL;
static netif_input_fn     s_ap_orig_input        = NULL;
static uint64_t s_sta_up_hook   = 0;
static uint64_t s_ap_down_hook   = 0;
static uint64_t s_sta_down_hook = 0;
static uint64_t s_ap_up_hook    = 0;

typedef enum {
    WIFI_CMD_CONNECT = 0,
    WIFI_CMD_SET_MODE,
    WIFI_CMD_START_AP,
    WIFI_CMD_STOP_AP,
} wifi_cmd_type_t;

typedef struct {
    wifi_cmd_type_t type;
    char            ssid[33];
    char            password[65];
    wifi_op_mode_t  mode;
} wifi_cmd_t;

static void cmd_task(void* arg)
{
    wifi_cmd_t cmd;
    while (1) {
        if (xQueueReceive(s_cmd_queue, &cmd, portMAX_DELAY)) {
            switch (cmd.type) {
            case WIFI_CMD_CONNECT:
                ESP_LOGI(TAG, "CMD: Connect to %s", cmd.ssid);
                wifi_service_connect(cmd.ssid, cmd.password);
                break;
            case WIFI_CMD_SET_MODE:
                ESP_LOGI(TAG, "CMD: Set mode to %d", (int)cmd.mode);
                wifi_service_set_mode(cmd.mode);
                break;
            case WIFI_CMD_START_AP:
                ESP_LOGI(TAG, "CMD: Start AP %s", cmd.ssid[0] ? cmd.ssid : "(default)");
                wifi_service_start_ap(cmd.ssid[0] ? cmd.ssid : NULL,
                                      cmd.password[0] ? cmd.password : NULL);
                break;
            case WIFI_CMD_STOP_AP:
                ESP_LOGI(TAG, "CMD: Stop AP");
                wifi_service_stop_ap();
                break;
            }
        }
    }
}

void wifi_service_post_connect(const char* ssid, const char* password)
{
    if (!s_cmd_queue) return;
    wifi_cmd_t cmd = {};
    cmd.type = WIFI_CMD_CONNECT;
    strncpy(cmd.ssid, ssid, sizeof(cmd.ssid) - 1);
    strncpy(cmd.password, password, sizeof(cmd.password) - 1);
    xQueueSend(s_cmd_queue, &cmd, 0);
}

void wifi_service_post_set_mode(wifi_op_mode_t mode)
{
    if (!s_cmd_queue) return;
    wifi_cmd_t cmd = {};
    cmd.type = WIFI_CMD_SET_MODE;
    cmd.mode = mode;
    xQueueSend(s_cmd_queue, &cmd, 0);
}

void wifi_service_post_start_ap(const char* ssid, const char* password)
{
    if (!s_cmd_queue) return;
    wifi_cmd_t cmd = {};
    cmd.type = WIFI_CMD_START_AP;
    if (ssid) strncpy(cmd.ssid, ssid, sizeof(cmd.ssid) - 1);
    if (password) strncpy(cmd.password, password, sizeof(cmd.password) - 1);
    xQueueSend(s_cmd_queue, &cmd, 0);
}

void wifi_service_post_stop_ap(void)
{
    if (!s_cmd_queue) return;
    wifi_cmd_t cmd = {};
    cmd.type = WIFI_CMD_STOP_AP;
    xQueueSend(s_cmd_queue, &cmd, 0);
}

static void update_rssi(void)
{
    if (s_sta_state == WIFI_STATE_CONNECTED) {
        wifi_ap_record_t ap_info;
        if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
            xSemaphoreTake(s_status_mutex, portMAX_DELAY);
            s_sta_rssi = ap_info.rssi;
            xSemaphoreGive(s_status_mutex);
        }
    }
}

static void rssi_timer_callback(void* arg)
{
    update_rssi();
}

static uint32_t calc_bps(uint64_t* last, uint64_t current)
{
    uint64_t diff = (current >= *last) ? (current - *last) : 0;
    *last = current;
    return (uint32_t)diff;
}

static void update_stats(void)
{
    uint64_t sta_down = s_sta_down_hook;
    uint64_t sta_up   = s_sta_up_hook;
    uint64_t ap_down  = s_ap_down_hook;
    uint64_t ap_up    = s_ap_up_hook;
    uint64_t usb_down = usb_network_get_rx_total();
    uint64_t usb_up   = usb_network_get_tx_total();

    xSemaphoreTake(s_status_mutex, portMAX_DELAY);
    s_sta_down_bps = calc_bps(&s_sta_down_last, sta_down);
    s_sta_up_bps   = calc_bps(&s_sta_up_last,   sta_up);
    s_ap_down_bps  = calc_bps(&s_ap_down_last,  ap_down);
    s_ap_up_bps    = calc_bps(&s_ap_up_last,    ap_up);
    s_usb_down_bps = calc_bps(&s_usb_down_last, usb_down);
    s_usb_up_bps   = calc_bps(&s_usb_up_last,   usb_up);
    xSemaphoreGive(s_status_mutex);
}

static void stats_timer_callback(void* arg)
{
    update_stats();
}

typedef struct {
    struct netif* nif;
    u8_t          enable;
} napt_cb_ctx_t;

static void napt_callback(void* ctx)
{
    napt_cb_ctx_t* c = (napt_cb_ctx_t*)ctx;
    if (c && c->nif) {
        ip_napt_enable_netif(c->nif, c->enable);
        ESP_LOGI(TAG, "NAPT: %s on %c%c%d",
                 c->enable ? "enabled" : "disabled",
                 c->nif->name[0], c->nif->name[1], c->nif->num);
    }
    free(ctx);
}

static void enable_napt_for_netif(esp_netif_t* esp_netif)
{
    if (!esp_netif) return;
    char name[8] = {0};
    esp_netif_get_netif_impl_name(esp_netif, name);
    struct netif* nif = find_netif_by_name(name);
    if (!nif) {
        ESP_LOGW(TAG, "NAPT: netif %s not found in netif_list", name);
        return;
    }
    napt_cb_ctx_t* ctx = (napt_cb_ctx_t*)calloc(1, sizeof(napt_cb_ctx_t));
    if (!ctx) return;
    ctx->nif    = nif;
    ctx->enable = 1;
    tcpip_callback(napt_callback, ctx);
}

static void disable_napt_for_netif(esp_netif_t* esp_netif)
{
    if (!esp_netif) return;
    char name[8] = {0};
    esp_netif_get_netif_impl_name(esp_netif, name);
    struct netif* nif = find_netif_by_name(name);
    if (!nif) return;
    napt_cb_ctx_t* ctx = (napt_cb_ctx_t*)calloc(1, sizeof(napt_cb_ctx_t));
    if (!ctx) return;
    ctx->nif    = nif;
    ctx->enable = 0;
    tcpip_callback(napt_callback, ctx);
}

static void enable_napt(void)
{
    esp_netif_t* usb_netif = usb_network_get_netif();
    enable_napt_for_netif(usb_netif);
    enable_napt_for_netif(s_ap_netif);

    s_napt_enabled = true;
    ESP_LOGI(TAG, "NAPT enabled: WiFi STA -> USB+AP sharing active");
}

static void disable_napt(void)
{
    if (!s_napt_enabled) return;

    esp_netif_t* usb_netif = usb_network_get_netif();
    disable_napt_for_netif(usb_netif);
    disable_napt_for_netif(s_ap_netif);

    s_napt_enabled = false;
    ESP_LOGI(TAG, "NAPT disabled");
}

static wifi_mode_t compute_wifi_mode(void)
{
    if (s_sta_active && s_ap_active) return WIFI_MODE_APSTA;
    if (s_ap_active)                 return WIFI_MODE_AP;
    return WIFI_MODE_STA;
}

static void apply_wifi_mode(wifi_mode_t new_mode, bool restart_wifi)
{
    wifi_mode_t current_mode = WIFI_MODE_NULL;
    esp_wifi_get_mode(&current_mode);

    if (current_mode != new_mode || restart_wifi) {
        ESP_LOGI(TAG, "WiFi mode: %d -> %d", (int)current_mode, (int)new_mode);
        ESP_ERROR_CHECK(esp_wifi_set_mode(new_mode));

        if (new_mode == WIFI_MODE_AP || new_mode == WIFI_MODE_APSTA) {
            wifi_config_t ap_cfg = {};
            strncpy((char*)ap_cfg.ap.ssid, s_ap_ssid, sizeof(ap_cfg.ap.ssid) - 1);
            strncpy((char*)ap_cfg.ap.password, s_ap_password, sizeof(ap_cfg.ap.password) - 1);
            ap_cfg.ap.max_connection = AP_MAX_CONNECTIONS;
            ap_cfg.ap.authmode = WIFI_AUTH_WPA2_PSK;
            ap_cfg.ap.channel = AP_DEFAULT_CHANNEL;
            ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_cfg));
            ESP_LOGI(TAG, "AP configured: SSID=%s", s_ap_ssid);
        }
    }
}

static void save_mode_to_nvs(wifi_op_mode_t mode)
{
    nvs_handle_t handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle) == ESP_OK) {
        nvs_set_u8(handle, NVS_KEY_MODE, (uint8_t)mode);
        nvs_commit(handle);
        nvs_close(handle);
    }
}

static err_t sta_linkoutput_hook(struct netif* netif, struct pbuf* p)
{
    s_sta_up_hook += p->tot_len;
    return s_sta_orig_linkoutput(netif, p);
}

static err_t ap_linkoutput_hook(struct netif* netif, struct pbuf* p)
{
    s_ap_down_hook += p->tot_len;
    return s_ap_orig_linkoutput(netif, p);
}

static err_t sta_input_hook(struct pbuf* p, struct netif* netif)
{
    s_sta_down_hook += p->tot_len;
    return s_sta_orig_input(p, netif);
}

static err_t ap_input_hook(struct pbuf* p, struct netif* netif)
{
    s_ap_up_hook += p->tot_len;
    return s_ap_orig_input(p, netif);
}

static struct netif* find_netif_by_name(const char* name)
{
    struct netif* cursor = netif_list;
    while (cursor) {
        if (cursor->name[0] == name[0] && cursor->name[1] == name[1] && cursor->num == (u8_t)(name[2] - '0')) {
            return cursor;
        }
        cursor = cursor->next;
    }
    return NULL;
}

static void install_sta_netif_hook(void)
{
    if (s_sta_orig_linkoutput) return;

    char name[8] = {0};
    esp_netif_get_netif_impl_name(s_sta_netif, name);
    struct netif* nif = find_netif_by_name(name);
    if (nif) {
        if (nif->linkoutput) {
            s_sta_orig_linkoutput = nif->linkoutput;
            nif->linkoutput = sta_linkoutput_hook;
        }
        if (nif->input) {
            s_sta_orig_input = nif->input;
            nif->input = sta_input_hook;
        }
        ESP_LOGI(TAG, "STA hooks installed on %c%c%d", name[0], name[1], name[2] - '0');
    }
}

static void install_ap_netif_hook(void)
{
    if (s_ap_orig_linkoutput) return;

    char name[8] = {0};
    esp_netif_get_netif_impl_name(s_ap_netif, name);
    struct netif* nif = find_netif_by_name(name);
    if (nif) {
        if (nif->linkoutput) {
            s_ap_orig_linkoutput = nif->linkoutput;
            nif->linkoutput = ap_linkoutput_hook;
        }
        if (nif->input) {
            s_ap_orig_input = nif->input;
            nif->input = ap_input_hook;
        }
        ESP_LOGI(TAG, "AP hooks installed on %c%c%d", name[0], name[1], name[2] - '0');
    }
}

static void wifi_event_handler(void* arg, esp_event_base_t event_base,
                               int32_t event_id, void* event_data)
{
    if (event_base == WIFI_EVENT) {
        switch (event_id) {
        case WIFI_EVENT_STA_START:
            ESP_LOGI(TAG, "WiFi STA started");
            break;

        case WIFI_EVENT_STA_CONNECTED:
            ESP_LOGI(TAG, "WiFi STA connected");
            update_rssi();
            break;

        case WIFI_EVENT_STA_DISCONNECTED: {
            wifi_event_sta_disconnected_t* disconn =
                (wifi_event_sta_disconnected_t*)event_data;
            ESP_LOGI(TAG, "WiFi disconnected, reason: %d", disconn->reason);

            xSemaphoreTake(s_status_mutex, portMAX_DELAY);
            s_sta_state = WIFI_STATE_DISCONNECTED;
            s_sta_ip[0] = '\0';
            s_sta_rssi  = 0;
            bool was_connected = s_sta_was_connected;
            s_sta_was_connected = false;
            xSemaphoreGive(s_status_mutex);

            xEventGroupClearBits(s_wifi_events, WIFI_CONNECTED_BIT);
            disable_napt();

            if (was_connected && disconn->reason != WIFI_REASON_ASSOC_LEAVE) {
                esp_wifi_connect();
            }
            break;
        }

        case WIFI_EVENT_AP_STACONNECTED: {
            wifi_event_ap_staconnected_t* event =
                (wifi_event_ap_staconnected_t*)event_data;
            xSemaphoreTake(s_status_mutex, portMAX_DELAY);
            int clients = ++s_ap_clients;
            xSemaphoreGive(s_status_mutex);
            ESP_LOGI(TAG, "AP client connected: " MACSTR ", total: %d",
                     MAC2STR(event->mac), clients);
            break;
        }

        case WIFI_EVENT_AP_STADISCONNECTED: {
            wifi_event_ap_stadisconnected_t* event =
                (wifi_event_ap_stadisconnected_t*)event_data;
            xSemaphoreTake(s_status_mutex, portMAX_DELAY);
            int clients = --s_ap_clients;
            xSemaphoreGive(s_status_mutex);
            ESP_LOGI(TAG, "AP client disconnected: " MACSTR ", total: %d",
                     MAC2STR(event->mac), clients);

            xSemaphoreTake(s_dhcp_mutex, portMAX_DELAY);
            for (int i = 0; i < s_dhcp_client_count; i++) {
                if (s_dhcp_clients[i].source == DHCP_CLIENT_SRC_AP &&
                    memcmp(s_dhcp_clients[i].mac, event->mac, 6) == 0) {
                    s_dhcp_clients[i] = s_dhcp_clients[s_dhcp_client_count - 1];
                    s_dhcp_client_count--;
                    break;
                }
            }
            xSemaphoreGive(s_dhcp_mutex);
            break;
        }

        case WIFI_EVENT_AP_START:
            ESP_LOGI(TAG, "WiFi AP started");

            xSemaphoreTake(s_status_mutex, portMAX_DELAY);
            s_ap_clients = 0;
            xSemaphoreGive(s_status_mutex);

            xSemaphoreTake(s_dhcp_mutex, portMAX_DELAY);
            for (int i = s_dhcp_client_count - 1; i >= 0; i--) {
                if (s_dhcp_clients[i].source == DHCP_CLIENT_SRC_AP) {
                    s_dhcp_clients[i] = s_dhcp_clients[s_dhcp_client_count - 1];
                    s_dhcp_client_count--;
                }
            }
            xSemaphoreGive(s_dhcp_mutex);

            if (s_ap_netif) {
                uint32_t lease_seconds = 3600;
                esp_netif_dhcps_option(s_ap_netif, ESP_NETIF_OP_SET,
                    ESP_NETIF_IP_ADDRESS_LEASE_TIME, &lease_seconds, sizeof(lease_seconds));

                esp_netif_dhcps_stop(s_ap_netif);

                uint8_t dns_enable = 0x02;
                esp_netif_dhcps_option(s_ap_netif, ESP_NETIF_OP_SET,
                    ESP_NETIF_DOMAIN_NAME_SERVER, &dns_enable, sizeof(dns_enable));

                esp_netif_dns_info_t ap_dns;
                memset(&ap_dns, 0, sizeof(ap_dns));
                ap_dns.ip.type = ESP_IPADDR_TYPE_V4;
                IP4_ADDR(&ap_dns.ip.u_addr.ip4, 8, 8, 8, 8);
                esp_netif_set_dns_info(s_ap_netif, ESP_NETIF_DNS_MAIN, &ap_dns);

                esp_err_t dhcps_ret = esp_netif_dhcps_start(s_ap_netif);
                ESP_LOGI(TAG, "AP DHCP started: ret=%d, DNS=8.8.8.8", dhcps_ret);
            }

            if (s_napt_enabled) {
                enable_napt_for_netif(s_ap_netif);
                ESP_LOGI(TAG, "NAPT re-applied for AP netif");
            }
            break;

        default:
            break;
        }
    } else if (event_base == IP_EVENT) {
        if (event_id == IP_EVENT_STA_GOT_IP) {
            ip_event_got_ip_t* event = (ip_event_got_ip_t*)event_data;

            xSemaphoreTake(s_status_mutex, portMAX_DELAY);
            snprintf(s_sta_ip, sizeof(s_sta_ip), IPSTR, IP2STR(&event->ip_info.ip));
            s_sta_state = WIFI_STATE_CONNECTED;
            s_sta_was_connected = true;
            xSemaphoreGive(s_status_mutex);

            ESP_LOGI(TAG, "Got STA IP: %s, GW: " IPSTR,
                     s_sta_ip, IP2STR(&event->ip_info.gw));

            xEventGroupSetBits(s_wifi_events, WIFI_CONNECTED_BIT);
            update_rssi();

            install_sta_netif_hook();

            esp_netif_set_default_netif(s_sta_netif);
            enable_napt();
        } else if (event_id == IP_EVENT_ASSIGNED_IP_TO_CLIENT) {
            ip_event_assigned_ip_to_client_t* evt =
                (ip_event_assigned_ip_to_client_t*)event_data;

            xSemaphoreTake(s_dhcp_mutex, portMAX_DELAY);
            bool found = false;
            for (int i = 0; i < s_dhcp_client_count; i++) {
                if (s_dhcp_clients[i].source == DHCP_CLIENT_SRC_USB &&
                    memcmp(s_dhcp_clients[i].mac, evt->mac, 6) == 0) {
                    snprintf(s_dhcp_clients[i].ip, sizeof(s_dhcp_clients[i].ip),
                             IPSTR, IP2STR(&evt->ip));
                    found = true;
                    break;
                }
            }
            if (!found && s_dhcp_client_count < DHCP_CLIENT_MAX) {
                dhcp_client_info_t* c = &s_dhcp_clients[s_dhcp_client_count];
                c->source = (evt->esp_netif == s_ap_netif) ? DHCP_CLIENT_SRC_AP
                                                           : DHCP_CLIENT_SRC_USB;
                memcpy(c->mac, evt->mac, 6);
                snprintf(c->ip, sizeof(c->ip), IPSTR, IP2STR(&evt->ip));
                s_dhcp_client_count++;
            }
            xSemaphoreGive(s_dhcp_mutex);

            ESP_LOGI(TAG, "DHCP assigned: " MACSTR " -> " IPSTR " (%s)",
                     MAC2STR(evt->mac), IP2STR(&evt->ip),
                     (evt->esp_netif == s_ap_netif) ? "AP" : "USB");
        }
    }
}

static wifi_op_mode_t derive_op_mode(void)
{
    if (s_sta_active && s_ap_active) return WIFI_OP_MODE_APSTA;
    if (s_ap_active)                 return WIFI_OP_MODE_AP;
    return WIFI_OP_MODE_STA;
}

void wifi_service_init(void)
{
    s_wifi_events  = xEventGroupCreate();
    s_cmd_queue    = xQueueCreate(CMD_QUEUE_SIZE, sizeof(wifi_cmd_t));
    s_status_mutex = xSemaphoreCreateMutex();
    s_dhcp_mutex   = xSemaphoreCreateMutex();

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    s_sta_netif = esp_netif_create_default_wifi_sta();
    s_ap_netif  = esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(
        IP_EVENT, IP_EVENT_ASSIGNED_IP_TO_CLIENT, wifi_event_handler, NULL));

    esp_timer_create_args_t timer_args = {
        .callback              = rssi_timer_callback,
        .arg                   = NULL,
        .dispatch_method       = ESP_TIMER_TASK,
        .name                  = "rssi_timer",
        .skip_unhandled_events = false
    };
    esp_timer_create(&timer_args, &s_rssi_timer);
    esp_timer_start_periodic(s_rssi_timer, RSSI_TIMER_PERIOD_US);

    esp_timer_create_args_t stats_timer_args = {
        .callback              = stats_timer_callback,
        .arg                   = NULL,
        .dispatch_method       = ESP_TIMER_TASK,
        .name                  = "stats_timer",
        .skip_unhandled_events = false
    };
    esp_timer_create(&stats_timer_args, &s_stats_timer);
    esp_timer_start_periodic(s_stats_timer, STATS_TIMER_PERIOD_US);

    wifi_op_mode_t saved_mode = WIFI_OP_MODE_STA;
    nvs_handle_t handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle) == ESP_OK) {
        uint8_t val = 0;
        if (nvs_get_u8(handle, NVS_KEY_MODE, &val) == ESP_OK) {
            saved_mode = (wifi_op_mode_t)val;
        }

        size_t len = sizeof(s_ap_ssid);
        nvs_get_str(handle, NVS_KEY_AP_SSID, s_ap_ssid, &len);

        len = sizeof(s_ap_password);
        nvs_get_str(handle, NVS_KEY_AP_PASS, s_ap_password, &len);

        nvs_close(handle);
    }

    switch (saved_mode) {
    case WIFI_OP_MODE_AP:
        s_sta_active = false;
        s_ap_active  = true;
        break;
    case WIFI_OP_MODE_APSTA:
        s_sta_active = true;
        s_ap_active  = true;
        break;
    case WIFI_OP_MODE_STA:
    default:
        s_sta_active = true;
        s_ap_active  = false;
        break;
    }

    wifi_mode_t target_mode = compute_wifi_mode();
    apply_wifi_mode(target_mode, true);
    ESP_ERROR_CHECK(esp_wifi_start());

    install_ap_netif_hook();

    if (s_sta_active && s_ap_active) {
        ESP_LOGI(TAG, "WiFi APSTA mode started: AP SSID=%s", s_ap_ssid);
    } else if (s_ap_active) {
        ESP_LOGI(TAG, "WiFi AP mode started: SSID=%s", s_ap_ssid);
    } else {
        ESP_LOGI(TAG, "WiFi STA mode initialized");
    }

    xTaskCreate(cmd_task, "wifi_cmd", CMD_TASK_STACK_SIZE,
                NULL, CMD_TASK_PRIORITY, NULL);
    ESP_LOGI(TAG, "WiFi command task started");
}

void wifi_service_connect(const char* ssid, const char* password)
{
    xSemaphoreTake(s_status_mutex, portMAX_DELAY);
    s_sta_state = WIFI_STATE_CONNECTING;
    s_sta_rssi  = 0;
    xSemaphoreGive(s_status_mutex);

    if (!s_sta_active) {
        s_sta_active = true;
        apply_wifi_mode(compute_wifi_mode(), false);
    }

    esp_wifi_scan_stop();
    esp_wifi_disconnect();

    wifi_config_t wifi_config = {};
    strncpy((char*)wifi_config.sta.ssid, ssid, sizeof(wifi_config.sta.ssid) - 1);
    strncpy((char*)wifi_config.sta.password, password, sizeof(wifi_config.sta.password) - 1);
    wifi_config.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;

    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_LOGI(TAG, "Connecting to SSID: %s", ssid);
    esp_wifi_connect();
}

void wifi_service_disconnect(void)
{
    if (!s_sta_active) return;
    esp_wifi_disconnect();
    xSemaphoreTake(s_status_mutex, portMAX_DELAY);
    s_sta_state = WIFI_STATE_DISCONNECTED;
    xSemaphoreGive(s_status_mutex);
}

void wifi_service_save_config(const char* ssid, const char* password)
{
    nvs_handle_t handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle) == ESP_OK) {
        nvs_set_str(handle, NVS_KEY_SSID, ssid);
        nvs_set_str(handle, NVS_KEY_PASS, password);
        nvs_set_u8(handle, NVS_KEY_MODE, (uint8_t)WIFI_OP_MODE_STA);
        nvs_commit(handle);
        nvs_close(handle);
        ESP_LOGI(TAG, "WiFi config saved to NVS");
    }
}

bool wifi_service_has_config(void)
{
    nvs_handle_t handle;
    size_t len = 0;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle) == ESP_OK) {
        esp_err_t ret = nvs_get_str(handle, NVS_KEY_SSID, NULL, &len);
        nvs_close(handle);
        return (ret == ESP_OK && len > 1);
    }
    return false;
}

void wifi_service_set_mode(wifi_op_mode_t mode)
{
    switch (mode) {
    case WIFI_OP_MODE_AP:
        if (s_sta_active) {
            esp_wifi_disconnect();
            s_sta_active = false;
        }
        s_ap_active = true;
        apply_wifi_mode(WIFI_MODE_AP, false);

        xSemaphoreTake(s_status_mutex, portMAX_DELAY);
        s_sta_state = WIFI_STATE_DISCONNECTED;
        s_sta_ip[0] = '\0';
        s_sta_rssi  = 0;
        xSemaphoreGive(s_status_mutex);
        break;

    case WIFI_OP_MODE_APSTA:
        s_sta_active = true;
        s_ap_active  = true;
        apply_wifi_mode(WIFI_MODE_APSTA, false);
        break;

    case WIFI_OP_MODE_STA:
    default:
        if (s_sta_active) {
            esp_wifi_disconnect();
        }
        s_ap_active = false;
        apply_wifi_mode(WIFI_MODE_STA, false);
        s_sta_active = true;

        xSemaphoreTake(s_status_mutex, portMAX_DELAY);
        s_sta_state = WIFI_STATE_DISCONNECTED;
        s_sta_ip[0] = '\0';
        s_sta_rssi  = 0;
        xSemaphoreGive(s_status_mutex);
        break;
    }

    save_mode_to_nvs(mode);

    ESP_LOGI(TAG, "Mode changed: STA=%s AP=%s",
             s_sta_active ? "ON" : "OFF",
             s_ap_active  ? "ON" : "OFF");
}

wifi_op_mode_t wifi_service_get_mode(void)
{
    return derive_op_mode();
}

void wifi_service_start_ap(const char* ssid, const char* password)
{
    if (ssid && strlen(ssid) > 0) {
        strncpy(s_ap_ssid, ssid, sizeof(s_ap_ssid) - 1);
    }
    if (password && strlen(password) > 0) {
        strncpy(s_ap_password, password, sizeof(s_ap_password) - 1);
    }

    nvs_handle_t handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle) == ESP_OK) {
        nvs_set_str(handle, NVS_KEY_AP_SSID, s_ap_ssid);
        nvs_set_str(handle, NVS_KEY_AP_PASS, s_ap_password);
        nvs_commit(handle);
        nvs_close(handle);
    }

    if (s_ap_active) {
        wifi_config_t ap_cfg = {};
        strncpy((char*)ap_cfg.ap.ssid, s_ap_ssid, sizeof(ap_cfg.ap.ssid) - 1);
        strncpy((char*)ap_cfg.ap.password, s_ap_password, sizeof(ap_cfg.ap.password) - 1);
        ap_cfg.ap.max_connection = AP_MAX_CONNECTIONS;
        ap_cfg.ap.authmode = WIFI_AUTH_WPA2_PSK;
        ap_cfg.ap.channel = AP_DEFAULT_CHANNEL;
        esp_wifi_set_config(WIFI_IF_AP, &ap_cfg);
        ESP_LOGI(TAG, "AP updated: SSID=%s", s_ap_ssid);
        return;
    }

    s_ap_active = true;
    wifi_mode_t target = compute_wifi_mode();
    apply_wifi_mode(target, false);

    wifi_op_mode_t op_mode = derive_op_mode();
    save_mode_to_nvs(op_mode);

    ESP_LOGI(TAG, "AP started: SSID=%s, combined mode=%d",
             s_ap_ssid, (int)op_mode);
}

void wifi_service_stop_ap(void)
{
    s_ap_active = false;
    wifi_mode_t target = compute_wifi_mode();
    apply_wifi_mode(target, false);

    wifi_op_mode_t op_mode = derive_op_mode();
    save_mode_to_nvs(op_mode);

    ESP_LOGI(TAG, "AP stopped, mode=%d", (int)op_mode);
}

bool wifi_service_is_ap_active(void)
{
    return s_ap_active;
}

void wifi_service_get_ap_config(char* ssid, size_t ssid_len,
                                 char* password, size_t pass_len)
{
    if (ssid && ssid_len > 0) {
        strncpy(ssid, s_ap_ssid, ssid_len - 1);
        ssid[ssid_len - 1] = '\0';
    }
    if (password && pass_len > 0) {
        strncpy(password, s_ap_password, pass_len - 1);
        password[pass_len - 1] = '\0';
    }
}

void wifi_service_get_status(WiFiStatus* status)
{
    memset(status, 0, sizeof(WiFiStatus));

    xSemaphoreTake(s_status_mutex, portMAX_DELAY);
    status->mode       = derive_op_mode();
    status->sta_state  = s_sta_state;
    strncpy(status->sta_ip, s_sta_ip, sizeof(status->sta_ip) - 1);
    status->sta_rssi   = s_sta_rssi;
    status->ap_active  = s_ap_active;
    status->ap_clients = s_ap_clients;
    status->sta_down_bps = s_sta_down_bps;
    status->sta_up_bps   = s_sta_up_bps;
    status->ap_down_bps  = s_ap_down_bps;
    status->ap_up_bps    = s_ap_up_bps;
    status->usb_down_bps = s_usb_down_bps;
    status->usb_up_bps   = s_usb_up_bps;
    xSemaphoreGive(s_status_mutex);

    strncpy(status->ap_ssid, s_ap_ssid, sizeof(status->ap_ssid) - 1);
    strncpy(status->ap_password, s_ap_password, sizeof(status->ap_password) - 1);

    wifi_config_t cfg;
    if (esp_wifi_get_config(WIFI_IF_STA, &cfg) == ESP_OK) {
        strncpy(status->sta_ssid, (char*)cfg.sta.ssid, sizeof(status->sta_ssid) - 1);
        strncpy(status->sta_password, (char*)cfg.sta.password, sizeof(status->sta_password) - 1);
    }
}

void wifi_service_get_status_json(char* buffer, size_t buffer_size)
{
    WiFiStatus status;
    wifi_service_get_status(&status);

    const char* state_str = "unknown";
    switch (status.sta_state) {
        case WIFI_STATE_DISCONNECTED: state_str = "disconnected"; break;
        case WIFI_STATE_CONNECTING:   state_str = "connecting"; break;
        case WIFI_STATE_CONNECTED:    state_str = "connected"; break;
    }

    const char* mode_str = "sta";
    if (status.mode == WIFI_OP_MODE_APSTA) mode_str = "apsta";
    else if (status.mode == WIFI_OP_MODE_AP) mode_str = "ap";

    snprintf(buffer, buffer_size,
        "{"
        "\"mode\":\"%s\","
        "\"sta_state\":\"%s\","
        "\"sta_ssid\":\"%s\","
        "\"sta_password\":\"%s\","
        "\"sta_ip\":\"%s\","
        "\"sta_rssi\":%d,"
        "\"ap_active\":%s,"
        "\"ap_ssid\":\"%s\","
        "\"ap_password\":\"%s\","
        "\"ap_clients\":%d,"
        "\"sta_down_bps\":%lu,"
        "\"sta_up_bps\":%lu,"
        "\"ap_down_bps\":%lu,"
        "\"ap_up_bps\":%lu,"
        "\"usb_down_bps\":%lu,"
        "\"usb_up_bps\":%lu"
        "}",
        mode_str, state_str,
        status.sta_ssid, status.sta_password,
        status.sta_ip, status.sta_rssi,
        status.ap_active ? "true" : "false",
        status.ap_ssid, status.ap_password,
        status.ap_clients,
        (unsigned long)status.sta_down_bps,
        (unsigned long)status.sta_up_bps,
        (unsigned long)status.ap_down_bps,
        (unsigned long)status.ap_up_bps,
        (unsigned long)status.usb_down_bps,
        (unsigned long)status.usb_up_bps);
}

int wifi_service_get_dhcp_clients(dhcp_client_info_t* clients, int max_count)
{
    int count = 0;
    xSemaphoreTake(s_dhcp_mutex, portMAX_DELAY);
    for (int i = 0; i < s_dhcp_client_count && count < max_count; i++) {
        clients[count] = s_dhcp_clients[i];
        count++;
    }
    xSemaphoreGive(s_dhcp_mutex);
    return count;
}

void wifi_service_get_dhcp_clients_json(char* buffer, size_t buffer_size)
{
    dhcp_client_info_t clients[DHCP_CLIENT_MAX];
    int count = wifi_service_get_dhcp_clients(clients, DHCP_CLIENT_MAX);

    int pos = snprintf(buffer, buffer_size, "[");
    for (int i = 0; i < count; i++) {
        char mac_str[18];
        snprintf(mac_str, sizeof(mac_str),
                 MACSTR, MAC2STR(clients[i].mac));
        pos += snprintf(buffer + pos, buffer_size - pos,
                        "%s{\"source\":\"%s\",\"mac\":\"%s\",\"ip\":\"%s\"}",
                        i > 0 ? "," : "",
                        clients[i].source == DHCP_CLIENT_SRC_AP ? "ap" : "usb",
                        mac_str,
                        clients[i].ip);
    }
    snprintf(buffer + pos, buffer_size - pos, "]");
}

static int compare_rssi(const void* a, const void* b)
{
    return ((wifi_scan_item_t*)b)->rssi - ((wifi_scan_item_t*)a)->rssi;
}

void wifi_service_scan_start(void)
{
    if (!s_wifi_events) return;

    wifi_mode_t mode;
    esp_wifi_get_mode(&mode);
    if (mode != WIFI_MODE_STA && mode != WIFI_MODE_APSTA) {
        ESP_LOGW(TAG, "Scan: STA not active, cannot scan");
        return;
    }

    if (s_scan_running) {
        ESP_LOGW(TAG, "Scan already in progress");
        return;
    }

    esp_wifi_scan_stop();

    wifi_scan_config_t scan_cfg = {};
    scan_cfg.scan_type = WIFI_SCAN_TYPE_ACTIVE;
    scan_cfg.scan_time.active.min = 100;
    scan_cfg.scan_time.active.max = 300;

    s_scan_ready = false;
    s_scan_count = 0;
    s_scan_running = true;

    esp_err_t ret = esp_wifi_scan_start(&scan_cfg, false);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Scan start failed: %d (%s)", ret, esp_err_to_name(ret));
        s_scan_running = false;
    } else {
        ESP_LOGI(TAG, "WiFi scan started");
    }
}

static void collect_scan_results(void)
{
    uint16_t ap_num = 0;
    esp_wifi_scan_get_ap_num(&ap_num);
    if (ap_num == 0) {
        s_scan_count = 0;
        s_scan_ready = true;
        s_scan_running = false;
        return;
    }

    int count = (ap_num < WIFI_SCAN_MAX_RESULTS) ? ap_num : WIFI_SCAN_MAX_RESULTS;
    wifi_ap_record_t* ap_records = (wifi_ap_record_t*)malloc(ap_num * sizeof(wifi_ap_record_t));
    if (!ap_records) {
        s_scan_count = 0;
        s_scan_ready = true;
        s_scan_running = false;
        return;
    }

    esp_wifi_scan_get_ap_records(&ap_num, ap_records);

    xSemaphoreTake(s_status_mutex, portMAX_DELAY);
    for (int i = 0; i < count; i++) {
        strncpy(s_scan_cache[i].ssid, (char*)ap_records[i].ssid,
                sizeof(s_scan_cache[i].ssid) - 1);
        s_scan_cache[i].ssid[sizeof(s_scan_cache[i].ssid) - 1] = '\0';
        s_scan_cache[i].rssi = ap_records[i].rssi;
        s_scan_cache[i].channel = ap_records[i].primary;
        s_scan_cache[i].authmode = (int)ap_records[i].authmode;
    }
    qsort(s_scan_cache, count, sizeof(wifi_scan_item_t), compare_rssi);
    s_scan_count = count;
    xSemaphoreGive(s_status_mutex);

    free(ap_records);
    s_scan_ready = true;
    s_scan_running = false;
    ESP_LOGI(TAG, "Scan done: %d APs found", count);
}

void wifi_service_get_scan_json(char* buffer, size_t buffer_size)
{
    xSemaphoreTake(s_status_mutex, portMAX_DELAY);
    int count = s_scan_count;
    int pos = snprintf(buffer, buffer_size, "{");
    pos += snprintf(buffer + pos, buffer_size - pos,
                    "\"running\":%s,\"count\":%d,\"results\":[",
                    s_scan_running ? "true" : "false", count);
    for (int i = 0; i < count; i++) {
        const char* auth_str = "open";
        if (s_scan_cache[i].authmode == WIFI_AUTH_WPA2_PSK ||
            s_scan_cache[i].authmode == WIFI_AUTH_WPA3_PSK ||
            s_scan_cache[i].authmode == WIFI_AUTH_WPA2_WPA3_PSK)
            auth_str = "secure";
        else if (s_scan_cache[i].authmode != WIFI_AUTH_OPEN)
            auth_str = "wep";
        pos += snprintf(buffer + pos, buffer_size - pos,
                        "%s{\"ssid\":\"%s\",\"rssi\":%d,\"channel\":%d,\"auth\":\"%s\"}",
                        i > 0 ? "," : "",
                        s_scan_cache[i].ssid,
                        s_scan_cache[i].rssi,
                        s_scan_cache[i].channel,
                        auth_str);
    }
    snprintf(buffer + pos, buffer_size - pos, "]}");
    xSemaphoreGive(s_status_mutex);
}