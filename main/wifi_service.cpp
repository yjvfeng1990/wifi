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
#include "wifi_service.h"
#include "usb_network.h"

static const char* TAG = "WIFI_SRV";
static const char* NVS_NAMESPACE = "wifi_cfg";
static const char* NVS_KEY_SSID = "ssid";
static const char* NVS_KEY_PASS = "password";
static const char* NVS_KEY_MODE = "mode";
static const char* NVS_KEY_AP_SSID = "ap_ssid";
static const char* NVS_KEY_AP_PASS = "ap_pass";

static wifi_op_mode_t s_mode = WIFI_OP_MODE_STA;
static wifi_state_t s_state = WIFI_STATE_DISCONNECTED;
static char s_ip[16] = "0.0.0.0";
static int s_rssi = 0;
static bool s_was_connected = false;
static bool s_napt_enabled = false;
static int s_ap_clients = 0;
static EventGroupHandle_t s_wifi_events;
static esp_netif_t* s_sta_netif = NULL;
static esp_netif_t* s_ap_netif = NULL;
static esp_timer_handle_t s_rssi_timer = NULL;
static char s_ap_ssid[33] = "ESP32-S3-Config";
static char s_ap_password[65] = "12345678";

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT BIT1

typedef enum {
    WIFI_CMD_CONNECT = 0,
    WIFI_CMD_SET_MODE,
    WIFI_CMD_START_AP,
    WIFI_CMD_STOP_AP,
} wifi_cmd_type_t;

typedef struct {
    wifi_cmd_type_t type;
    char ssid[33];
    char password[65];
    wifi_op_mode_t mode;
} wifi_cmd_t;

static QueueHandle_t s_cmd_queue = NULL;
static SemaphoreHandle_t s_status_mutex = NULL;

static void wifi_cmd_task(void* arg)
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
                ESP_LOGI(TAG, "CMD: Set mode to %s", cmd.mode == WIFI_OP_MODE_AP ? "AP" : "STA");
                wifi_service_set_mode(cmd.mode);
                break;
            case WIFI_CMD_START_AP:
                ESP_LOGI(TAG, "CMD: Start AP %s", cmd.ssid[0] ? cmd.ssid : "(default)");
                wifi_service_start_ap(cmd.ssid[0] ? cmd.ssid : NULL, cmd.password[0] ? cmd.password : NULL);
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
    if (s_state == WIFI_STATE_CONNECTED) {
        wifi_ap_record_t ap_info;
        if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
            s_rssi = ap_info.rssi;
        }
    }
}

static void rssi_timer_callback(void* arg)
{
    update_rssi();
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
            wifi_event_sta_disconnected_t* disconn = (wifi_event_sta_disconnected_t*)event_data;
            ESP_LOGI(TAG, "WiFi disconnected, reason: %d", disconn->reason);

            if (s_status_mutex) xSemaphoreTake(s_status_mutex, portMAX_DELAY);
            s_state = WIFI_STATE_DISCONNECTED;
            s_ip[0] = '\0';
            s_rssi = 0;
            if (s_status_mutex) xSemaphoreGive(s_status_mutex);

            xEventGroupClearBits(s_wifi_events, WIFI_CONNECTED_BIT);

            if (s_napt_enabled) {
                esp_netif_t* usb_netif = usb_network_get_netif();
                if (usb_netif) {
                    esp_netif_napt_disable(usb_netif);
                }
                if (s_status_mutex) xSemaphoreTake(s_status_mutex, portMAX_DELAY);
                s_napt_enabled = false;
                if (s_status_mutex) xSemaphoreGive(s_status_mutex);
            }

            if (s_was_connected && disconn->reason != WIFI_REASON_ASSOC_LEAVE) {
                s_was_connected = false;
                esp_wifi_connect();
            }
            break;
        }
        case WIFI_EVENT_AP_STACONNECTED: {
            wifi_event_ap_staconnected_t* event = (wifi_event_ap_staconnected_t*)event_data;
            if (s_status_mutex) xSemaphoreTake(s_status_mutex, portMAX_DELAY);
            s_ap_clients++;
            int clients = s_ap_clients;
            if (s_status_mutex) xSemaphoreGive(s_status_mutex);
            ESP_LOGI(TAG, "AP client connected: " MACSTR ", total: %d",
                     MAC2STR(event->mac), clients);
            break;
        }
        case WIFI_EVENT_AP_STADISCONNECTED: {
            wifi_event_ap_stadisconnected_t* event = (wifi_event_ap_stadisconnected_t*)event_data;
            if (s_status_mutex) xSemaphoreTake(s_status_mutex, portMAX_DELAY);
            s_ap_clients--;
            int clients = s_ap_clients;
            if (s_status_mutex) xSemaphoreGive(s_status_mutex);
            ESP_LOGI(TAG, "AP client disconnected: " MACSTR ", total: %d",
                     MAC2STR(event->mac), clients);
            break;
        }
        default:
            break;
        }
    } else if (event_base == IP_EVENT) {
        if (event_id == IP_EVENT_STA_GOT_IP) {
            ip_event_got_ip_t* event = (ip_event_got_ip_t*)event_data;
            snprintf(s_ip, sizeof(s_ip), IPSTR, IP2STR(&event->ip_info.ip));
            ESP_LOGI(TAG, "Got IP: %s, GW: " IPSTR, s_ip, IP2STR(&event->ip_info.gw));

            if (s_status_mutex) xSemaphoreTake(s_status_mutex, portMAX_DELAY);
            s_state = WIFI_STATE_CONNECTED;
            s_was_connected = true;
            if (s_status_mutex) xSemaphoreGive(s_status_mutex);

            xEventGroupSetBits(s_wifi_events, WIFI_CONNECTED_BIT);
            update_rssi();

            esp_netif_set_default_netif(s_sta_netif);

            esp_netif_t* usb_netif = usb_network_get_netif();
            if (usb_netif) {
                esp_err_t napt_ret = esp_netif_napt_enable(usb_netif);
                if (napt_ret == ESP_OK) {
                    ESP_LOGI(TAG, "NAPT enabled: WiFi -> USB sharing active");
                    if (s_status_mutex) xSemaphoreTake(s_status_mutex, portMAX_DELAY);
                    s_napt_enabled = true;
                    if (s_status_mutex) xSemaphoreGive(s_status_mutex);
                } else {
                    ESP_LOGE(TAG, "NAPT enable failed: 0x%x", napt_ret);
                }
            }
        }
    }
}

void wifi_service_init(void)
{
    s_wifi_events = xEventGroupCreate();
    s_cmd_queue = xQueueCreate(8, sizeof(wifi_cmd_t));
    s_status_mutex = xSemaphoreCreateMutex();

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    s_sta_netif = esp_netif_create_default_wifi_sta();
    s_ap_netif = esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler, NULL));

    esp_timer_create_args_t timer_args = {
        .callback = rssi_timer_callback,
        .arg = NULL,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "rssi_timer",
        .skip_unhandled_events = false
    };
    esp_timer_create(&timer_args, &s_rssi_timer);
    esp_timer_start_periodic(s_rssi_timer, 3000000);

    nvs_handle_t handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle) == ESP_OK) {
        uint8_t mode_val = WIFI_OP_MODE_STA;
        size_t len = sizeof(mode_val);
        nvs_get_blob(handle, NVS_KEY_MODE, &mode_val, &len);
        s_mode = (wifi_op_mode_t)mode_val;
        nvs_close(handle);
    }

    if (s_mode == WIFI_OP_MODE_AP) {
        esp_wifi_set_mode(WIFI_MODE_AP);
        wifi_config_t ap_cfg = {};
        strcpy((char*)ap_cfg.ap.ssid, s_ap_ssid);
        strcpy((char*)ap_cfg.ap.password, s_ap_password);
        ap_cfg.ap.max_connection = 4;
        ap_cfg.ap.authmode = WIFI_AUTH_WPA2_PSK;
        ap_cfg.ap.channel = 1;
        esp_wifi_set_config(WIFI_IF_AP, &ap_cfg);
        ESP_ERROR_CHECK(esp_wifi_start());
        ESP_LOGI(TAG, "WiFi AP mode started: SSID=%s", s_ap_ssid);
    } else {
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
        ESP_ERROR_CHECK(esp_wifi_start());
        ESP_LOGI(TAG, "WiFi STA mode initialized");
    }

    xTaskCreate(wifi_cmd_task, "wifi_cmd", 4096, NULL, 5, NULL);
    ESP_LOGI(TAG, "WiFi command task started");
}

void wifi_service_connect(const char* ssid, const char* password)
{
    if (s_status_mutex) xSemaphoreTake(s_status_mutex, portMAX_DELAY);
    s_state = WIFI_STATE_CONNECTING;
    s_rssi = 0;
    if (s_status_mutex) xSemaphoreGive(s_status_mutex);

    if (s_mode != WIFI_OP_MODE_STA) {
        esp_wifi_set_mode(WIFI_MODE_STA);
        if (s_status_mutex) xSemaphoreTake(s_status_mutex, portMAX_DELAY);
        s_mode = WIFI_OP_MODE_STA;
        if (s_status_mutex) xSemaphoreGive(s_status_mutex);
    }

    wifi_config_t wifi_config = {};
    strcpy((char*)wifi_config.sta.ssid, ssid);
    strcpy((char*)wifi_config.sta.password, password);
    wifi_config.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;

    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_LOGI(TAG, "Connecting to SSID: %s", ssid);
    esp_wifi_connect();
}

void wifi_service_disconnect(void)
{
    if (s_mode == WIFI_OP_MODE_STA) {
        esp_wifi_disconnect();
        if (s_status_mutex) xSemaphoreTake(s_status_mutex, portMAX_DELAY);
        s_state = WIFI_STATE_DISCONNECTED;
        if (s_status_mutex) xSemaphoreGive(s_status_mutex);
    }
}

void wifi_service_save_config(const char* ssid, const char* password)
{
    nvs_handle_t handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle) == ESP_OK) {
        nvs_set_str(handle, NVS_KEY_SSID, ssid);
        nvs_set_str(handle, NVS_KEY_PASS, password);
        uint8_t mode_val = (uint8_t)WIFI_OP_MODE_STA;
        nvs_set_blob(handle, NVS_KEY_MODE, &mode_val, sizeof(mode_val));
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
    if (mode == s_mode) return;

    if (s_mode == WIFI_OP_MODE_STA) {
        esp_wifi_disconnect();
    }

    if (mode == WIFI_OP_MODE_AP) {
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
        wifi_config_t ap_cfg = {};
        strcpy((char*)ap_cfg.ap.ssid, s_ap_ssid);
        strcpy((char*)ap_cfg.ap.password, s_ap_password);
        ap_cfg.ap.max_connection = 4;
        ap_cfg.ap.authmode = WIFI_AUTH_WPA2_PSK;
        ap_cfg.ap.channel = 1;
        esp_wifi_set_config(WIFI_IF_AP, &ap_cfg);

        if (s_status_mutex) xSemaphoreTake(s_status_mutex, portMAX_DELAY);
        s_mode = mode;
        s_state = WIFI_STATE_CONNECTED;
        if (s_status_mutex) xSemaphoreGive(s_status_mutex);
    } else {
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));

        if (s_status_mutex) xSemaphoreTake(s_status_mutex, portMAX_DELAY);
        s_mode = mode;
        s_state = WIFI_STATE_DISCONNECTED;
        s_ip[0] = '\0';
        s_rssi = 0;
        if (s_status_mutex) xSemaphoreGive(s_status_mutex);
    }

    nvs_handle_t handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle) == ESP_OK) {
        uint8_t mode_val = (uint8_t)mode;
        nvs_set_blob(handle, NVS_KEY_MODE, &mode_val, sizeof(mode_val));
        nvs_commit(handle);
        nvs_close(handle);
    }

    ESP_LOGI(TAG, "Mode changed to: %s", mode == WIFI_OP_MODE_AP ? "AP" : "STA");
}

wifi_op_mode_t wifi_service_get_mode(void)
{
    return s_mode;
}

void wifi_service_start_ap(const char* ssid, const char* password)
{
    if (ssid && strlen(ssid) > 0) {
        strcpy(s_ap_ssid, ssid);
    }
    if (password && strlen(password) > 0) {
        strcpy(s_ap_password, password);
    }

    nvs_handle_t handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle) == ESP_OK) {
        nvs_set_str(handle, NVS_KEY_AP_SSID, s_ap_ssid);
        nvs_set_str(handle, NVS_KEY_AP_PASS, s_ap_password);
        nvs_commit(handle);
        nvs_close(handle);
    }

    wifi_service_set_mode(WIFI_OP_MODE_AP);
}

void wifi_service_stop_ap(void)
{
    wifi_service_set_mode(WIFI_OP_MODE_STA);
}

void wifi_service_get_ap_config(char* ssid, size_t ssid_len, char* password, size_t pass_len)
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

    if (s_status_mutex) xSemaphoreTake(s_status_mutex, portMAX_DELAY);
    status->mode = s_mode;
    status->state = s_state;
    strcpy(status->ip, s_ip);
    status->rssi = s_rssi;
    status->ap_clients = s_ap_clients;
    if (s_status_mutex) xSemaphoreGive(s_status_mutex);

    strcpy(status->ap_ssid, s_ap_ssid);
    strcpy(status->ap_password, s_ap_password);

    wifi_config_t cfg;
    if (esp_wifi_get_config(WIFI_IF_STA, &cfg) == ESP_OK) {
        strcpy(status->sta_ssid, (char*)cfg.sta.ssid);
        strcpy(status->sta_password, (char*)cfg.sta.password);
    }
}

void wifi_service_get_status_json(char* buffer, size_t bufferSize)
{
    WiFiStatus status;
    wifi_service_get_status(&status);

    const char* state_str = "unknown";
    switch (status.state) {
        case WIFI_STATE_DISCONNECTED: state_str = "disconnected"; break;
        case WIFI_STATE_CONNECTING:   state_str = "connecting"; break;
        case WIFI_STATE_CONNECTED:    state_str = "connected"; break;
    }

    snprintf(buffer, bufferSize,
        "{\"mode\":\"%s\",\"state\":\"%s\",\"ssid\":\"%s\",\"password\":\"%s\","
        "\"ip\":\"%s\",\"rssi\":%d,\"ap_ssid\":\"%s\",\"ap_password\":\"%s\","
        "\"ap_clients\":%d}",
        status.mode == WIFI_OP_MODE_AP ? "ap" : "sta",
        state_str, status.sta_ssid, status.sta_password,
        status.ip, status.rssi,
        status.ap_ssid, status.ap_password, status.ap_clients);
}