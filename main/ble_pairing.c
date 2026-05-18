#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_bt_device.h"
#include "esp_gap_ble_api.h"
#include "esp_mac.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/timers.h"
#include "ble_pairing.h"
#include "wifi_now.h"

static const char* TAG = "BLE_PAIR";

#define BLE_MFG_ID              0x02E5
#define BLE_TAG_MARKER_0        'E'
#define BLE_TAG_MARKER_1        'N'

static SemaphoreHandle_t s_mutex              = NULL;
static SemaphoreHandle_t s_scan_mutex         = NULL;

static bool              s_advertising        = false;
static bool              s_scanning           = false;
static bool              s_auto_pair          = true;  // 自动配对开关
static char              s_dev_name[BLE_DEV_NAME_MAX] = "ESP32-S3-NOW";
static uint8_t           s_own_now_mac[6]     = {0};

static ble_discovered_device_t s_discovered[BLE_MAX_DISCOVERED];
static int                     s_discovered_count = 0;

static TimerHandle_t s_scan_timer = NULL;

static uint8_t build_adv_raw(uint8_t* buf, uint8_t buf_size)
{
    uint8_t pos = 0;

    if (pos + 3 <= buf_size) {
        buf[pos++] = 2;
        buf[pos++] = 0x01;
        buf[pos++] = 0x06;
    }

    uint8_t name_len = (uint8_t)strlen(s_dev_name);
    uint8_t name_max = buf_size - pos - 2;
    if (name_len > name_max) name_len = name_max;

    if (pos + 2 + name_len <= buf_size) {
        buf[pos++] = 1 + name_len;
        buf[pos++] = 0x09;
        memcpy(buf + pos, s_dev_name, name_len);
        pos += name_len;
    }

    uint8_t mfg_total_len = 10 + name_len;
    uint8_t mfg_max = buf_size - pos - 2;
    if (mfg_total_len > mfg_max) mfg_total_len = mfg_max;

    if (pos + 2 + mfg_total_len <= buf_size) {
        buf[pos++] = 1 + mfg_total_len;
        buf[pos++] = 0xFF;
        uint8_t* p = buf + pos;
        p[0] = (uint8_t)(BLE_MFG_ID & 0xFF);
        p[1] = (uint8_t)((BLE_MFG_ID >> 8) & 0xFF);
        p[2] = BLE_TAG_MARKER_0;
        p[3] = BLE_TAG_MARKER_1;
        memcpy(p + 4, s_own_now_mac, 6);
        int copy_len = mfg_total_len - 10;
        if (copy_len > 0) {
            memcpy(p + 10, s_dev_name, copy_len);
        }
        pos += mfg_total_len;
    }

    return pos;
}

static bool parse_scan_mfg_data(const uint8_t* adv_data, uint8_t adv_data_len,
                                 uint8_t* now_mac_out, char* name_out, int name_size)
{
    uint8_t idx = 0;
    while (idx < adv_data_len) {
        uint8_t len = adv_data[idx];
        if (len == 0 || idx + len >= adv_data_len) break;
        uint8_t type = adv_data[idx + 1];
        if (type == 0xFF && len >= 12) {
            uint16_t mfg_id = adv_data[idx + 2] | ((uint16_t)adv_data[idx + 3] << 8);
            if (mfg_id == BLE_MFG_ID &&
                adv_data[idx + 4] == BLE_TAG_MARKER_0 &&
                adv_data[idx + 5] == BLE_TAG_MARKER_1) {
                memcpy(now_mac_out, adv_data + idx + 6, 6);
                if (name_out && name_size > 0) {
                    int name_len = (int)len - 10;
                    if (name_len > name_size - 1) name_len = name_size - 1;
                    if (name_len > 0) {
                        memcpy(name_out, adv_data + idx + 12, name_len);
                        name_out[name_len] = '\0';
                    } else {
                        name_out[0] = '\0';
                    }
                }
                return true;
            }
        }
        idx += (len + 1);
    }
    return false;
}

static bool is_duplicate_mac(const uint8_t* mac)
{
    for (int i = 0; i < s_discovered_count; i++) {
        if (memcmp(s_discovered[i].mac, mac, 6) == 0) return true;
    }
    return false;
}

static bool is_duplicate_now_mac(const uint8_t* now_mac)
{
    for (int i = 0; i < s_discovered_count; i++) {
        if (memcmp(s_discovered[i].now_mac, now_mac, 6) == 0) return true;
    }
    return false;
}

static void gap_event_handler(esp_gap_ble_cb_event_t event,
                               esp_ble_gap_cb_param_t* params)
{
    switch (event) {
    case ESP_GAP_BLE_EXT_ADV_SET_PARAMS_COMPLETE_EVT:
        if (params->ext_adv_set_params.status == ESP_BT_STATUS_SUCCESS) {
            uint8_t raw[31];
            uint8_t raw_len = build_adv_raw(raw, sizeof(raw));
            esp_ble_gap_config_ext_adv_data_raw(0, raw_len, raw);
        }
        break;
    case ESP_GAP_BLE_EXT_ADV_DATA_SET_COMPLETE_EVT:
        if (params->ext_adv_data_set.status == ESP_BT_STATUS_SUCCESS) {
            esp_ble_gap_ext_adv_t ext_adv = {};
            ext_adv.instance = 0;
            ext_adv.duration = 0;
            ext_adv.max_events = 0;
            esp_ble_gap_ext_adv_start(1, &ext_adv);
        }
        break;
    case ESP_GAP_BLE_EXT_ADV_START_COMPLETE_EVT:
        if (params->ext_adv_start.status == ESP_BT_STATUS_SUCCESS) {
            s_advertising = true;
            ESP_LOGI(TAG, "Advertising started: name=%s, NOW_MAC=" MACSTR,
                     s_dev_name, MAC2STR(s_own_now_mac));
        }
        break;
    case ESP_GAP_BLE_EXT_ADV_STOP_COMPLETE_EVT:
        if (params->ext_adv_stop.status == ESP_BT_STATUS_SUCCESS) {
            s_advertising = false;
            ESP_LOGI(TAG, "Advertising stopped");
        }
        break;
    case ESP_GAP_BLE_EXT_SCAN_START_COMPLETE_EVT:
        if (params->ext_scan_start.status == ESP_BT_STATUS_SUCCESS) {
            s_scanning = true;
            ESP_LOGI(TAG, "Scan started");
        }
        break;
    case ESP_GAP_BLE_SCAN_STOP_COMPLETE_EVT:
        if (params->scan_stop_cmpl.status == ESP_BT_STATUS_SUCCESS) {
            s_scanning = false;
            ESP_LOGI(TAG, "Scan stopped, discovered=%d", s_discovered_count);
        }
        break;
    case ESP_GAP_BLE_EXT_ADV_REPORT_EVT: {
        const esp_ble_gap_ext_adv_report_t* report = &params->ext_adv_report.params;
        uint8_t now_mac[6];
        char name[BLE_DEV_NAME_MAX] = {0};
        if (parse_scan_mfg_data(report->adv_data, report->adv_data_len, now_mac, name, sizeof(name))) {
            bool already_discovered = is_duplicate_now_mac(now_mac);
            bool already_paired = wifi_now_is_peer_exists(now_mac);
            
            // 自动添加到ESP-NOW peer并发送配对请求（如果没有重复）
            if (s_auto_pair && !already_paired) {
                uint8_t channel = wifi_now_get_channel();
                bool added = wifi_now_add_peer(now_mac, channel);
                if (added) {
                    ESP_LOGI(TAG, "Auto-paired with %s (" MACSTR "), ch=%d",
                             name, MAC2STR(now_mac), channel);
                    wifi_now_save_peers();
                    
                    // 发送配对请求给对方，让对方也添加我们为peer
                    wifi_now_send_pair_request(now_mac);
                }
            } else if (already_paired && !already_discovered) {
                // 已经配对过但不是刚刚发现的，发送配对请求更新信息
                wifi_now_send_pair_request(now_mac);
            }
            
            xSemaphoreTake(s_scan_mutex, portMAX_DELAY);
            if (!is_duplicate_mac(report->addr) && s_discovered_count < BLE_MAX_DISCOVERED) {
                ble_discovered_device_t* dev = &s_discovered[s_discovered_count];
                memcpy(dev->mac, report->addr, 6);
                memcpy(dev->now_mac, now_mac, 6);
                strncpy(dev->name, name, sizeof(dev->name) - 1);
                dev->rssi = report->rssi;
                s_discovered_count++;
                ESP_LOGI(TAG, "Discovered: name=%s, NOW_MAC=" MACSTR
                         ", BLE_MAC=" MACSTR ", rssi=%d, paired=%s",
                         name, MAC2STR(now_mac),
                         MAC2STR(report->addr), report->rssi,
                         already_paired ? "yes" : "no");
            }
            xSemaphoreGive(s_scan_mutex);
        }
        break;
    }
    default:
        break;
    }
}

static void scan_timer_callback(TimerHandle_t timer)
{
    ESP_LOGI(TAG, "Scan timeout, stopping...");
    ble_pairing_stop_scan();
}

void ble_pairing_init(void)
{
    if (s_mutex) return;

    s_mutex      = xSemaphoreCreateMutex();
    s_scan_mutex = xSemaphoreCreateMutex();

    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    memcpy(s_own_now_mac, mac, 6);
    ESP_LOGI(TAG, "ESP-NOW MAC: " MACSTR, MAC2STR(s_own_now_mac));

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }

    ESP_ERROR_CHECK(esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT));

    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    ret = esp_bt_controller_init(&bt_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "BT controller init failed: %d", ret);
        return;
    }

    ret = esp_bt_controller_enable(ESP_BT_MODE_BLE);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "BT controller enable failed: %d", ret);
        return;
    }

    ret = esp_bluedroid_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Bluedroid init failed: %d", ret);
        return;
    }

    ret = esp_bluedroid_enable();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Bluedroid enable failed: %d", ret);
        return;
    }

    ESP_ERROR_CHECK(esp_ble_gap_register_callback(gap_event_handler));

    const uint8_t* bt_mac = esp_bt_dev_get_address();
    if (bt_mac) {
        ESP_LOGI(TAG, "BLE MAC: " MACSTR, MAC2STR(bt_mac));
    }

    nvs_handle_t handle;
    if (nvs_open("espnow_cfg", NVS_READONLY, &handle) == ESP_OK) {
        size_t len = sizeof(s_dev_name);
        nvs_get_str(handle, "ble_name", s_dev_name, &len);
        nvs_close(handle);
    }

    ESP_LOGI(TAG, "BLE pairing initialized, dev_name=%s", s_dev_name);
}

void ble_pairing_deinit(void)
{
    ble_pairing_stop_advertise();
    ble_pairing_stop_scan();

    if (s_mutex) {
        vSemaphoreDelete(s_mutex);
        s_mutex = NULL;
    }
    if (s_scan_mutex) {
        vSemaphoreDelete(s_scan_mutex);
        s_scan_mutex = NULL;
    }

    esp_bluedroid_disable();
    esp_bluedroid_deinit();
    esp_bt_controller_disable();
    esp_bt_controller_deinit();
}

ble_pair_state_t ble_pairing_get_state(void)
{
    if (s_advertising) return BLE_PAIR_STATE_ADVERTISING;
    if (s_scanning) return BLE_PAIR_STATE_SCANNING;
    return BLE_PAIR_STATE_OFF;
}

bool ble_pairing_start_advertise(const char* device_name)
{
    if (device_name && device_name[0]) {
        strncpy(s_dev_name, device_name, sizeof(s_dev_name) - 1);
        nvs_handle_t handle;
        if (nvs_open("espnow_cfg", NVS_READWRITE, &handle) == ESP_OK) {
            nvs_set_str(handle, "ble_name", s_dev_name);
            nvs_commit(handle);
            nvs_close(handle);
        }
    }

    esp_ble_gap_ext_adv_params_t ext_adv_params = {};
    ext_adv_params.type = ESP_BLE_GAP_SET_EXT_ADV_PROP_LEGACY_IND;
    ext_adv_params.interval_min = 160;
    ext_adv_params.interval_max = 160;
    ext_adv_params.channel_map = ADV_CHNL_ALL;
    ext_adv_params.own_addr_type = BLE_ADDR_TYPE_PUBLIC;
    ext_adv_params.primary_phy = ESP_BLE_GAP_PHY_1M;
    ext_adv_params.secondary_phy = ESP_BLE_GAP_PHY_1M;
    ext_adv_params.scan_req_notif = false;

    esp_err_t ret = esp_ble_gap_ext_adv_set_params(0, &ext_adv_params);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Set adv params failed: %d", ret);
        return false;
    }

    return true;
}

void ble_pairing_stop_advertise(void)
{
    if (!s_advertising) return;
    uint8_t instance = 0;
    esp_ble_gap_ext_adv_stop(1, (const uint8_t*)&instance);
    s_advertising = false;
    ESP_LOGI(TAG, "Advertising stopped");
}

bool ble_pairing_is_advertising(void)
{
    return s_advertising;
}

bool ble_pairing_start_scan(uint8_t duration_sec)
{
    xSemaphoreTake(s_scan_mutex, portMAX_DELAY);
    s_discovered_count = 0;
    memset(s_discovered, 0, sizeof(s_discovered));
    xSemaphoreGive(s_scan_mutex);

    esp_ble_ext_scan_params_t ext_scan_params = {};
    ext_scan_params.own_addr_type = BLE_ADDR_TYPE_PUBLIC;
    ext_scan_params.filter_policy = BLE_SCAN_FILTER_ALLOW_ALL;
    ext_scan_params.scan_duplicate = BLE_SCAN_DUPLICATE_DISABLE;
    ext_scan_params.cfg_mask = ESP_BLE_GAP_EXT_SCAN_CFG_UNCODE_MASK;
    ext_scan_params.uncoded_cfg.scan_type = BLE_SCAN_TYPE_ACTIVE;
    ext_scan_params.uncoded_cfg.scan_interval = 80;
    ext_scan_params.uncoded_cfg.scan_window = 48;
    ext_scan_params.coded_cfg.scan_type = BLE_SCAN_TYPE_ACTIVE;
    ext_scan_params.coded_cfg.scan_interval = 80;
    ext_scan_params.coded_cfg.scan_window = 48;

    esp_err_t ret = esp_ble_gap_set_ext_scan_params(&ext_scan_params);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Set scan params failed: %d", ret);
        return false;
    }

    uint32_t duration_ms = duration_sec * 1000;
    ret = esp_ble_gap_start_ext_scan(duration_ms, 0);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Start scan failed: %d", ret);
        return false;
    }

    if (s_scan_timer) {
        xTimerDelete(s_scan_timer, 0);
    }
    s_scan_timer = xTimerCreate("scan_tmr",
                                 pdMS_TO_TICKS(duration_sec * 1000 + 500),
                                 pdFALSE, NULL, scan_timer_callback);
    if (s_scan_timer) {
        xTimerStart(s_scan_timer, 0);
    }

    ESP_LOGI(TAG, "Scan started, duration=%ds", duration_sec);
    return true;
}

void ble_pairing_stop_scan(void)
{
    if (!s_scanning) return;

    if (s_scan_timer) {
        xTimerStop(s_scan_timer, 0);
        xTimerDelete(s_scan_timer, 0);
        s_scan_timer = NULL;
    }

    esp_ble_gap_stop_ext_scan();
    s_scanning = false;
    ESP_LOGI(TAG, "Scan stopped");
}

bool ble_pairing_is_scanning(void)
{
    return s_scanning;
}

int ble_pairing_get_discovered(ble_discovered_device_t* devices, int max_count)
{
    int count = 0;
    xSemaphoreTake(s_scan_mutex, portMAX_DELAY);
    for (int i = 0; i < s_discovered_count && count < max_count; i++) {
        devices[count] = s_discovered[i];
        count++;
    }
    xSemaphoreGive(s_scan_mutex);
    return count;
}

bool ble_pairing_pair_with_device(int index)
{
    xSemaphoreTake(s_scan_mutex, portMAX_DELAY);
    if (index < 0 || index >= s_discovered_count) {
        xSemaphoreGive(s_scan_mutex);
        ESP_LOGE(TAG, "Invalid device index: %d", index);
        return false;
    }

    uint8_t now_mac[6];
    char name[BLE_DEV_NAME_MAX];
    memcpy(now_mac, s_discovered[index].now_mac, 6);
    strncpy(name, s_discovered[index].name, sizeof(name) - 1);
    xSemaphoreGive(s_scan_mutex);

    uint8_t channel = wifi_now_get_channel();
    bool ok = wifi_now_add_peer(now_mac, channel);
    if (ok) {
        ESP_LOGI(TAG, "Paired with %s, NOW_MAC=" MACSTR,
                 name, MAC2STR(now_mac));
        wifi_now_save_peers();
    }
    return ok;
}

void ble_pairing_set_auto_pair(bool enable)
{
    s_auto_pair = enable;
    ESP_LOGI(TAG, "Auto-pair: %s", enable ? "enabled" : "disabled");
}

bool ble_pairing_get_auto_pair(void)
{
    return s_auto_pair;
}

uint8_t* ble_pairing_get_own_now_mac(void)
{
    return s_own_now_mac;
}

void ble_pairing_get_name(char* name_out)
{
    if (!name_out) return;
    strncpy(name_out, s_dev_name, BLE_DEV_NAME_MAX - 1);
    name_out[BLE_DEV_NAME_MAX - 1] = '\0';
}
