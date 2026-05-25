#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "esp_wifi.h"
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
#include "freertos/task.h"
#include "freertos/queue.h"
#include "ble_pairing.h"
#include "wifi_now.h"
#include "role_control.h"

static const char* TAG = "BLE_PAIR";

#define BLE_MFG_ID              0x02E5
#define BLE_TAG_MARKER_0        'E'
#define BLE_TAG_MARKER_1        'N'

// BLE 扫描重试：当发现设备少于 2 个时自动补扫
// 用于规避 ESP32-S3 BLE controller 内部状态机交替丢失设备的 bug
// 策略：每次补扫前先复位 BLE controller（清空状态机），再用更长的时间扫描
#define MAX_SCAN_RETRIES        3       // 最多重试 3 次
#define RETRY_SCAN_DURATION     30      // 每次补扫 30 秒（足够 controller 恢复）

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
static TimerHandle_t s_burst_timer = NULL;
static bool          s_burst_mode = false;
static TickType_t    s_last_burst_ticks = 0;  // 上次burst启动时间, 用于频率限制
static TickType_t    s_scan_start_ticks = 0;  // 本次扫描开始时间, burst延迟判断
static SemaphoreHandle_t s_scan_stop_sem = NULL; // 同步信号量: 等待BLE控制器确认扫描停止
static int           s_scan_retries = 0;       // 当前补扫计数
static TimerHandle_t s_post_scan_timer = NULL;  // 扫描停止后的延迟处理定时器
static TickType_t    s_disc_time[BLE_MAX_DISCOVERED] = {0}; // 每个设备被发现的时间戳（诊断用）
static int           s_adv_report_total = 0;        // 诊断：本次扫描收到的 ADV_REPORT 总数

static uint8_t build_adv_raw(uint8_t* buf, uint8_t buf_size)
{
    uint8_t pos = 0;

    // Flags AD
    if (pos + 3 <= buf_size) {
        buf[pos++] = 2;
        buf[pos++] = 0x01;
        buf[pos++] = 0x06;
    }

    uint8_t name_len = (uint8_t)strlen(s_dev_name);

    // MFG AD: MFG_ID(2) + TAG(2) + MAC(6) + VERSION(1) + CH(1) + TYPE(1) + name = 13 + name_len
    // 注意: 不使用单独的Name AD, 名字放在MFG段中以节省空间(31字节限制)
    uint8_t mfg_fixed = 13;
    uint8_t mfg_total_len = mfg_fixed + name_len;
    uint8_t mfg_max = buf_size - pos - 2;
    if (mfg_total_len > mfg_max) mfg_total_len = mfg_max;

    if (pos + 2 + mfg_total_len <= buf_size && mfg_total_len >= mfg_fixed) {
        buf[pos++] = 1 + mfg_total_len;
        buf[pos++] = 0xFF;
        uint8_t* p = buf + pos;
        p[0] = (uint8_t)(BLE_MFG_ID & 0xFF);
        p[1] = (uint8_t)((BLE_MFG_ID >> 8) & 0xFF);
        p[2] = BLE_TAG_MARKER_0;
        p[3] = BLE_TAG_MARKER_1;
        memcpy(p + 4, s_own_now_mac, 6);
        p[10] = 1;  // 版本号: 用于区分新旧格式
        p[11] = wifi_now_get_channel();
        p[12] = PEER_TYPE_ESPNOW;
        int copy_len = mfg_total_len - mfg_fixed;
        if (copy_len > 0) {
            memcpy(p + 13, s_dev_name, copy_len);
        }
        pos += mfg_total_len;
    }

    return pos;
}

static bool parse_scan_mfg_data(const uint8_t* adv_data, uint8_t adv_data_len,
                                 uint8_t* now_mac_out, char* name_out, int name_size,
                                 uint8_t* channel_out, uint8_t* type_out)
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
                // 使用版本号字节(offset 12)区分新旧格式:
                //   新版: VERSION=1, channel at +13, type at +14, name at +15
                //   旧版: 无版本号, name at +10 (VERSION字节充当名字首字节)
                uint8_t fmt_version = adv_data[idx + 12];
                if (fmt_version == 1 && len >= 15) {
                    // 新版格式: 包含版本号, channel, type
                    if (channel_out) *channel_out = adv_data[idx + 13];
                    if (type_out) *type_out = adv_data[idx + 14];
                    if (name_out && name_size > 0) {
                        // len包含AD type字节: data=len-1, header=13, name=len-1-13=len-14
                        int name_len = (int)len - 14;
                        if (name_len > name_size - 1) name_len = name_size - 1;
                        if (name_len > 0) {
                            memcpy(name_out, adv_data + idx + 15, name_len);
                            name_out[name_len] = '\0';
                        } else {
                            name_out[0] = '\0';
                        }
                    }
                } else {
                    // 旧版格式: 无channel/type (MFG_ID+TAG+MAC=10字节, name在fixed header之后)
                    if (channel_out) *channel_out = 0;
                    if (type_out) *type_out = PEER_TYPE_ESPNOW;
                    if (name_out && name_size > 0) {
                        // data=len-1, header=10, name=len-1-10=len-11
                        int name_len = (int)len - 11;
                        if (name_len > name_size - 1) name_len = name_size - 1;
                        if (name_len > 0) {
                            memcpy(name_out, adv_data + idx + 12, name_len);
                            name_out[name_len] = '\0';
                        } else {
                            name_out[0] = '\0';
                        }
                    }
                }
                return true;
            }
        }
        idx += (len + 1);
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

static void parse_adv_name(const uint8_t* adv_data, uint8_t adv_data_len,
                            char* name_out, int name_size)
{
    if (!name_out || name_size <= 0) return;
    uint8_t idx = 0;
    while (idx < adv_data_len) {
        uint8_t len = adv_data[idx];
        if (len == 0 || idx + len >= adv_data_len) break;
        uint8_t type = adv_data[idx + 1];
        if (type == 0x09 || type == 0x08) {
            int copy_len = (int)len - 1;
            if (copy_len > name_size - 1) copy_len = name_size - 1;
            if (copy_len > 0) {
                memcpy(name_out, adv_data + idx + 2, copy_len);
                name_out[copy_len] = '\0';
            }
            return;
        }
        idx += (len + 1);
    }
}

// 扫描结束后批量处理所有发现的设备（add_peer + save_peers + send_pair_request），
// 不再在 GAP 回调中做任何 ESP-NOW 操作，彻底避免 ESP-NOW 传输干扰 BLE 扫描。

// 前向声明（定义在后，因相互引用需要提前声明）
static void deferred_process_task_func(void *arg);
static void post_scan_timer_callback(TimerHandle_t timer);

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
            s_scan_start_ticks = xTaskGetTickCount();
            ESP_LOGI(TAG, "Scan started");
        }
        break;
    case ESP_GAP_BLE_SCAN_STOP_COMPLETE_EVT:
        s_scanning = false;
        // 无论状态码是否成功，都通知等待扫描停止的同步信号量
        if (s_scan_stop_sem) {
            xSemaphoreGive(s_scan_stop_sem);
        }
        ESP_LOGI(TAG, "Scan stopped (status=%d), discovered=%d, total_adv=%d",
                 params->scan_stop_cmpl.status, s_discovered_count, s_adv_report_total);

        // 启动延迟处理定时器（100ms），避免与 stop_scan 的同步信号量竞争
        // 无论扫描如何停止（自动停止或手动停止），都在此统一处理后处理逻辑
        if (s_post_scan_timer) {
            xTimerReset(s_post_scan_timer, 0);
        } else {
            s_post_scan_timer = xTimerCreate("post_scan",
                                              pdMS_TO_TICKS(100),
                                              pdFALSE, NULL, post_scan_timer_callback);
            if (s_post_scan_timer) {
                xTimerStart(s_post_scan_timer, 0);
            }
        }
        break;
    case ESP_GAP_BLE_EXT_ADV_REPORT_EVT: {
        s_adv_report_total++;
        const esp_ble_gap_ext_adv_report_t* report = &params->ext_adv_report.params;
        // 诊断用：每 50 个包打印一次统计，避免在密集 BLE 环境中串口泛洪
        if (s_adv_report_total % 50 == 1) {
            ESP_LOGI(TAG, "ADV_REPORT #%d: latest addr=" MACSTR ", rssi=%d, data_len=%d",
                     s_adv_report_total,
                     MAC2STR(report->addr), report->rssi,
                     report->adv_data_len);
        }
        uint8_t now_mac[6];
        char name[BLE_DEV_NAME_MAX] = {0};
        uint8_t peer_channel = 0;
        uint8_t peer_type = PEER_TYPE_ESPNOW;
        if (parse_scan_mfg_data(report->adv_data, report->adv_data_len,
                                now_mac, name, sizeof(name),
                                &peer_channel, &peer_type)) {
            // MFG数据中名字为空时, 尝试从标准Name AD获取
            if (name[0] == '\0') {
                parse_adv_name(report->adv_data, report->adv_data_len,
                               name, sizeof(name));
            }
            bool already_discovered = is_duplicate_now_mac(now_mac);

            // 只记录发现的设备到列表，不进行任何 ESP-NOW 操作
            // ESP-NOW 的 add_peer/save/send 全部推迟到扫描结束后统一处理
            xSemaphoreTake(s_scan_mutex, portMAX_DELAY);
            if (!already_discovered && s_discovered_count < BLE_MAX_DISCOVERED) {
                ble_discovered_device_t* dev = &s_discovered[s_discovered_count];
                memcpy(dev->mac, report->addr, 6);
                memcpy(dev->now_mac, now_mac, 6);
                strncpy(dev->name, name, sizeof(dev->name) - 1);
                dev->rssi = report->rssi;
                dev->channel = peer_channel;
                dev->peer_type = peer_type;
                s_disc_time[s_discovered_count] = xTaskGetTickCount();
                s_discovered_count++;

                ESP_LOGI(TAG, "Discovered[%d]: name=%s, NOW_MAC=" MACSTR
                         ", ch=%d, type=%d, rssi=%d, total=%d",
                         s_discovered_count - 1, name, MAC2STR(now_mac), peer_channel, peer_type,
                         report->rssi, s_discovered_count);
            } else if (already_discovered) {
                ESP_LOGD(TAG, "Dup disc(now): name=%s, rssi=%d, cnt=%d",
                         name, report->rssi, s_discovered_count);
            }
            xSemaphoreGive(s_scan_mutex);
        }
        break;
    }
    default:
        break;
    }
}

// 前向声明
static void ble_pairing_process_discovered(void);

// 扫描停止后的延迟处理回调：每100ms检查是否需要进行补扫或处理
// 放在定时器回调而非 GAP 事件中，以避免在 BLE controller 上下文执行复杂操作
static void post_scan_timer_callback(TimerHandle_t timer)
{
    // 如果正在扫描（如补扫已启动），不做任何事
    if (s_scanning) return;

    // 打印诊断信息：每个设备被发现的时间（相对启动时间）
    for (int i = 0; i < s_discovered_count; i++) {
        TickType_t elapsed = s_disc_time[i] - s_scan_start_ticks;
        ESP_LOGI(TAG, "  DIAG: dev[%d]=%s discovered at +%dms",
                 i, s_discovered[i].name, (int)(elapsed * portTICK_PERIOD_MS));
    }

    // 扫描完全停止后的处理逻辑
    // 如果发现设备少于 2 个且还有补扫机会，先复位 BLE controller 再启动补扫
    if (s_discovered_count < 2 && s_scan_retries < MAX_SCAN_RETRIES) {
        s_scan_retries++;
        ESP_LOGI(TAG, "=== RETRY %d/%d: only %d device(s), "
                 "resetting BLE controller + %ds supplementary scan ===",
                 s_scan_retries, MAX_SCAN_RETRIES, s_discovered_count, RETRY_SCAN_DURATION);

        // 关键：先复位 BLE controller，清空内部状态机再重新开始扫描
        // 避免 controller 在错误状态下继续补扫（无效补扫）
        ble_pairing_reset_controller();
        vTaskDelay(pdMS_TO_TICKS(200));

        // 复位后使用被动扫描（避免 SCAN_REQ/SCAN_RSP 碰撞丢失）
        esp_ble_ext_scan_params_t ext_scan_params = {};
        ext_scan_params.own_addr_type = BLE_ADDR_TYPE_PUBLIC;
        ext_scan_params.filter_policy = BLE_SCAN_FILTER_ALLOW_ALL;
        ext_scan_params.scan_duplicate = BLE_SCAN_DUPLICATE_ENABLE;
        ext_scan_params.cfg_mask = ESP_BLE_GAP_EXT_SCAN_CFG_UNCODE_MASK;
        // 重试使用被动扫描（PASSIVE），减少空中碰撞
        ext_scan_params.uncoded_cfg.scan_type = BLE_SCAN_TYPE_PASSIVE;
        ext_scan_params.uncoded_cfg.scan_interval = 160;
        ext_scan_params.uncoded_cfg.scan_window = 80;
        ext_scan_params.coded_cfg.scan_type = BLE_SCAN_TYPE_PASSIVE;
        ext_scan_params.coded_cfg.scan_interval = 160;
        ext_scan_params.coded_cfg.scan_window = 80;

        esp_err_t ret = esp_ble_gap_set_ext_scan_params(&ext_scan_params);
        if (ret == ESP_OK) {
            s_scan_start_ticks = xTaskGetTickCount();
            // esp_ble_gap_start_ext_scan 的 duration 参数单位是 10ms
            uint32_t retry_10ms = RETRY_SCAN_DURATION * 100;
            ret = esp_ble_gap_start_ext_scan(retry_10ms, 0);
            if (ret == ESP_OK) {
                ESP_LOGI(TAG, "Retry scan %d/%d started (%ds, PASSIVE mode)",
                         s_scan_retries, MAX_SCAN_RETRIES, RETRY_SCAN_DURATION);
                return;
            }
        }
        ESP_LOGE(TAG, "Failed to start retry scan: %d", ret);
        // 失败后继续往下走：进行常规处理
    }

    // 补扫结束或无需补扫：创建处理任务
    // 如果还差一个设备，但重试用完了，仍然尝试配对（能配几个算几个）
    if (s_discovered_count < 2 && s_scan_retries >= MAX_SCAN_RETRIES) {
        ESP_LOGW(TAG, "*** WARNING: Only %d/%d devices discovered after %d retries ***",
                 s_discovered_count, 2, MAX_SCAN_RETRIES);
    }

    // 创建延迟处理任务。3秒等待移至任务函数内部执行，避免阻塞
    // FreeRTOS timer 任务（阻塞 timer 任务会阻止 WiFi 管理定时器触发，
    // 导致 web 服务器在 BLE 扫描后无响应）
    TaskHandle_t proc_task = NULL;
    if (xTaskCreate(deferred_process_task_func, "disc_proc", 4096,
                    NULL, tskIDLE_PRIORITY + 1, &proc_task) != pdPASS) {
        ESP_LOGW(TAG, "Failed to create discovered-processing task");
    }
}

static void scan_timer_callback(TimerHandle_t timer)
{
    ESP_LOGI(TAG, "Scan timeout, stopping...");
    ble_pairing_stop_scan();
}

static void burst_timer_callback(TimerHandle_t timer)
{
    ESP_LOGI(TAG, "Adv burst timeout, stopping...");
    ble_pairing_stop_adv_burst();
}

bool ble_pairing_start_adv_burst(const char* device_name, uint16_t duration_sec)
{
    if (s_burst_mode) {
        // 已经在突发广告中, 重置定时器
        if (s_burst_timer) {
            xTimerReset(s_burst_timer, 0);
        }
        return true;
    }

    // 如果有扫描在进行, 先停止
    bool was_scanning = s_scanning;
    if (was_scanning) {
        esp_ble_gap_stop_ext_scan();
        s_scanning = false;
    }

    // 保存扫描状态, 突发结束后恢复
    s_burst_mode = true;

    // 更新设备名
    if (device_name && device_name[0]) {
        strncpy(s_dev_name, device_name, sizeof(s_dev_name) - 1);
    }

    // 开始广告
    esp_ble_gap_ext_adv_params_t ext_adv_params = {};
    ext_adv_params.type = ESP_BLE_GAP_SET_EXT_ADV_PROP_LEGACY_IND;
    ext_adv_params.interval_min = 160;
    ext_adv_params.interval_max = 200;
    ext_adv_params.channel_map = ADV_CHNL_ALL;
    ext_adv_params.own_addr_type = BLE_ADDR_TYPE_PUBLIC;
    ext_adv_params.primary_phy = ESP_BLE_GAP_PHY_1M;
    ext_adv_params.secondary_phy = ESP_BLE_GAP_PHY_1M;
    ext_adv_params.scan_req_notif = false;

    esp_err_t ret = esp_ble_gap_ext_adv_set_params(0, &ext_adv_params);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Burst: set adv params failed: %d", ret);
        s_burst_mode = false;
        return false;
    }

    // 创建突发定时器
    if (s_burst_timer) {
        xTimerDelete(s_burst_timer, 0);
    }
    s_burst_timer = xTimerCreate("burst_tmr",
                                  pdMS_TO_TICKS(duration_sec * 1000),
                                  pdFALSE, NULL, burst_timer_callback);
    if (s_burst_timer) {
        xTimerStart(s_burst_timer, 0);
    }

    ESP_LOGI(TAG, "Adv burst started: name=%s, NOW_MAC=" MACSTR ", duration=%ds",
             s_dev_name, MAC2STR(s_own_now_mac), duration_sec);
    return true;
}

void ble_pairing_stop_adv_burst(void)
{
    if (!s_burst_mode) return;

    s_burst_mode = false;

    // 停止广告
    if (s_advertising) {
        uint8_t instance = 0;
        esp_ble_gap_ext_adv_stop(1, (const uint8_t*)&instance);
        s_advertising = false;
    }

    if (s_burst_timer) {
        xTimerStop(s_burst_timer, 0);
        xTimerDelete(s_burst_timer, 0);
        s_burst_timer = NULL;
    }

    ESP_LOGI(TAG, "Adv burst stopped");

    // 恢复扫描 (角色仍是 RECEIVE 模式时)
    if (!s_scanning) {
        // 使用短扫描持续时间, 不重置扫描定时器
        esp_ble_ext_scan_params_t ext_scan_params = {};
        ext_scan_params.own_addr_type = BLE_ADDR_TYPE_PUBLIC;
        ext_scan_params.filter_policy = BLE_SCAN_FILTER_ALLOW_ALL;
        ext_scan_params.scan_duplicate = BLE_SCAN_DUPLICATE_ENABLE;
        ext_scan_params.cfg_mask = ESP_BLE_GAP_EXT_SCAN_CFG_UNCODE_MASK;
        ext_scan_params.uncoded_cfg.scan_type = BLE_SCAN_TYPE_PASSIVE;
        ext_scan_params.uncoded_cfg.scan_interval = 80;
        ext_scan_params.uncoded_cfg.scan_window = 80;
        ext_scan_params.coded_cfg.scan_type = BLE_SCAN_TYPE_PASSIVE;
        ext_scan_params.coded_cfg.scan_interval = 80;
        ext_scan_params.coded_cfg.scan_window = 80;

        esp_err_t ret = esp_ble_gap_set_ext_scan_params(&ext_scan_params);
        if (ret == ESP_OK) {
            esp_ble_gap_start_ext_scan(0, 0);
        }
    }
}

bool ble_pairing_is_burst_mode(void)
{
    return s_burst_mode;
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

    // 创建扫描停止同步信号量 (用于等待 BLE Controller 确认扫描停止)
    if (!s_scan_stop_sem) {
        s_scan_stop_sem = xSemaphoreCreateBinary();
    }
}

void ble_pairing_deinit(void)
{
    ble_pairing_stop_advertise();
    ble_pairing_stop_scan();

    // 清理后处理定时器
    if (s_post_scan_timer) {
        xTimerStop(s_post_scan_timer, 0);
        xTimerDelete(s_post_scan_timer, 0);
        s_post_scan_timer = NULL;
    }

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
    ext_adv_params.interval_max = 200;
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

// 扫描结束后批量处理所有发现的设备：add_peer + save_peers + send_pair_request
// 在 BLE 扫描完全停止后才调用，避免 ESP-NOW 传输干扰 BLE 扫描
static void ble_pairing_process_discovered(void)
{
    // 检查 ESP-NOW 是否已初始化，防止在角色关闭/解绑过程中执行 ESP-NOW 操作
    if (!wifi_now_is_initialized()) {
        ESP_LOGI(TAG, "Post-scan: ESP-NOW not initialized, skipping");
        return;
    }

    if (s_discovered_count == 0) {
        ESP_LOGI(TAG, "Post-scan: no devices discovered");
        return;
    }

    ESP_LOGI(TAG, "Post-scan: processing %d discovered device(s)...", s_discovered_count);

    int paired_count = 0;
    for (int i = 0; i < s_discovered_count; i++) {
        ble_discovered_device_t* dev = &s_discovered[i];

        // 自动配对关闭或 Broadcast 角色时跳过
        if (!s_auto_pair || role_control_get_role() == ROLE_BROADCAST) {
            ESP_LOGI(TAG, "Post-scan: skip auto-pair for %s (auto=%d, role=%d)",
                     dev->name, s_auto_pair, role_control_get_role());
            continue;
        }

        bool already_paired = wifi_now_is_peer_cached(dev->now_mac);

        if (already_paired) {
            // 已配对设备重连：更新信道 + 发送配对回复
            ESP_LOGI(TAG, "Post-scan rediscovery: %s (" MACSTR "), ch=%d",
                     dev->name, MAC2STR(dev->now_mac), dev->channel);
            wifi_now_add_peer_with_name(dev->now_mac, dev->channel, dev->name, dev->peer_type);
            wifi_now_save_peers();
            wifi_now_send_pair_response(dev->now_mac);
            paired_count++;
        } else {
            // 全新设备：添加 peer + 保存 + 发送配对请求
            uint8_t channel = (dev->channel != 0) ? dev->channel : wifi_now_get_channel();
            bool added = wifi_now_add_peer_with_name(dev->now_mac, channel, dev->name, dev->peer_type);
            if (added) {
                ESP_LOGI(TAG, "Post-scan auto-pair: %s (" MACSTR "), ch=%d",
                         dev->name, MAC2STR(dev->now_mac), channel);
                wifi_now_save_peers();
                wifi_now_send_pair_request(dev->now_mac);
                paired_count++;
            } else {
                ESP_LOGW(TAG, "Post-scan add peer FAILED for %s", dev->name);
            }
        }
    }

    ESP_LOGI(TAG, "Post-scan result: %d/%d devices paired", paired_count, s_discovered_count);

    // BLE burst 暂时禁用：经测试发现启动 burst 会导致 WiFi 不稳定，
    // 使 web 服务器无响应。自动配对功能不受影响，SLave 可通过自身
    // 广播来让 Master 发现（或通过 WEB 手动触发配对）。
    // if (paired_count > 0 && !s_burst_mode && !s_advertising) {
    //     TickType_t now = xTaskGetTickCount();
    //     if (s_last_burst_ticks == 0 || now - s_last_burst_ticks > pdMS_TO_TICKS(15000)) {
    //         vTaskDelay(pdMS_TO_TICKS(2000));
    //         s_last_burst_ticks = xTaskGetTickCount();
    //         ble_pairing_start_adv_burst(NULL, 5);
    //     }
    // }
}

bool ble_pairing_start_scan(uint16_t duration_sec)
{
    // 如果已经有扫描在运行，先停止（避免与上次的补扫状态冲突）
    if (s_scanning) {
        ESP_LOGI(TAG, "Scan already running, stopping first...");
        ble_pairing_stop_scan();
        vTaskDelay(pdMS_TO_TICKS(200));
    }

    // 停止并清理可能存在的后处理定时器
    if (s_post_scan_timer) {
        xTimerStop(s_post_scan_timer, 0);
        xTimerDelete(s_post_scan_timer, 0);
        s_post_scan_timer = NULL;
    }

    // 新扫描开始：重置补扫计数，清空发现的设备列表
    s_scan_retries = 0;
    s_adv_report_total = 0;
    xSemaphoreTake(s_scan_mutex, portMAX_DELAY);
    s_discovered_count = 0;
    memset(s_discovered, 0, sizeof(s_discovered));
    memset(s_disc_time, 0, sizeof(s_disc_time));
    xSemaphoreGive(s_scan_mutex);

    esp_ble_ext_scan_params_t ext_scan_params = {};
    ext_scan_params.own_addr_type = BLE_ADDR_TYPE_PUBLIC;
    ext_scan_params.filter_policy = BLE_SCAN_FILTER_ALLOW_ALL;
    ext_scan_params.scan_duplicate = BLE_SCAN_DUPLICATE_ENABLE;
    ext_scan_params.cfg_mask = ESP_BLE_GAP_EXT_SCAN_CFG_UNCODE_MASK;
    // 使用主动扫描 (ACTIVE)：BLE controller 会发送 SCAN_REQ 给每个广告者，
    // 收到 SCAN_RSP 后再上报给 host。这改变了 controller 内部状态机行为，
    // 可能避免被动扫描中的交替丢失 bug。
    ext_scan_params.uncoded_cfg.scan_type = BLE_SCAN_TYPE_ACTIVE;
    ext_scan_params.uncoded_cfg.scan_interval = 160;
    ext_scan_params.uncoded_cfg.scan_window = 80;  // 50% duty cycle: BLE 扫描占用一半天线时间，留给 WiFi 足够窗口
    ext_scan_params.coded_cfg.scan_type = BLE_SCAN_TYPE_ACTIVE;
    ext_scan_params.coded_cfg.scan_interval = 160;
    ext_scan_params.coded_cfg.scan_window = 80;    // 50% duty cycle

    esp_err_t ret = esp_ble_gap_set_ext_scan_params(&ext_scan_params);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Set scan params failed: %d", ret);
        return false;
    }

    // esp_ble_gap_start_ext_scan 的 duration 参数单位是 10ms（而非毫秒），
    // 所以 15 秒扫描需要传入 15 * 100 = 1500（即 1500 * 10ms = 15000ms）
    uint32_t duration_10ms = duration_sec * 100;
    ret = esp_ble_gap_start_ext_scan(duration_10ms, 0);
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

    ESP_LOGI(TAG, "Scan started, duration=%ds (ACTIVE mode)", duration_sec);
    return true;
}

// 异步配对处理任务函数：在独立任务中处理发现的设备，不阻塞 HTTP 服务器
static void deferred_process_task_func(void *arg)
{
    // 延迟 3 秒让 BLE controller 完全停止，WiFi MAC 恢复稳定后才操作 ESP-NOW
    // 此延迟在独立任务中执行，不阻塞 FreeRTOS timer 任务（timer 任务阻塞会
    // 阻止 WiFi 管理定时器，导致 web 服务器在 BLE 扫描后无响应）
    vTaskDelay(pdMS_TO_TICKS(3000));
    ESP_LOGI(TAG, "Deferred process task started");
    ble_pairing_process_discovered();
    ESP_LOGI(TAG, "Deferred process task finished");
    vTaskDelete(NULL);
}

void ble_pairing_stop_scan(void)
{
    if (!s_scanning) return;

    if (s_scan_timer) {
        xTimerStop(s_scan_timer, 0);
        xTimerDelete(s_scan_timer, 0);
        s_scan_timer = NULL;
    }

    // 先消费残留的信号量（防止前一次超时留下的信号干扰）
    xSemaphoreTake(s_scan_stop_sem, 0);

    esp_ble_gap_stop_ext_scan();

    // 同步等待 BLE Controller 确认扫描停止，避免下次启动时状态冲突
    // SCAN_STOP_COMPLETE_EVT 会 give semaphore 并启动 post_scan_timer
    if (xSemaphoreTake(s_scan_stop_sem, pdMS_TO_TICKS(2000)) == pdTRUE) {
        ESP_LOGI(TAG, "Scan stop confirmed by controller");
    } else {
        ESP_LOGW(TAG, "Scan stop timeout - forcing stop, starting post-scan manually");
        s_scanning = false;
        // BLE controller 未响应 stop 时，手动触发后处理逻辑
        // 避免 auto-pair 因 SCAN_STOP_COMPLETE_EVT 丢失而永远不执行
        if (s_post_scan_timer) {
            xTimerReset(s_post_scan_timer, 0);
        } else {
            s_post_scan_timer = xTimerCreate("post_scan",
                                              pdMS_TO_TICKS(100),
                                              pdFALSE, NULL, post_scan_timer_callback);
            if (s_post_scan_timer) {
                xTimerStart(s_post_scan_timer, 0);
            }
        }
    }

    // 处理逻辑（补扫 / 创建处理任务）移至 post_scan_timer_callback。
    // post_scan_timer 由 SCAN_STOP_COMPLETE_EVT 启动 100ms 后触发。
    // 补扫受 s_scan_retries 上限约束，最多 MAX_SCAN_RETRIES 次。
    // ble_pairing_start_scan() 在新扫描前停止补扫并重置 retries。
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
    uint8_t peer_type;
    memcpy(now_mac, s_discovered[index].now_mac, 6);
    strncpy(name, s_discovered[index].name, sizeof(name) - 1);
    peer_type = s_discovered[index].peer_type;
    xSemaphoreGive(s_scan_mutex);

    uint8_t channel = wifi_now_get_channel();
    bool ok = wifi_now_add_peer_with_name(now_mac, channel, name, peer_type);
    if (ok) {
        ESP_LOGI(TAG, "Paired with %s, NOW_MAC=" MACSTR,
                 name, MAC2STR(now_mac));
        wifi_now_save_peers();
        // 多次发送配对请求，将本机 PMK 同步给对端，确保对端在BLE广播间隙能收到
        for (int _r = 0; _r < 5; _r++) {
            wifi_now_send_pair_request(now_mac);
            vTaskDelay(pdMS_TO_TICKS(20));
        }
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

bool ble_pairing_set_name(const char* name)
{
    if (!name || !name[0]) return false;

    strncpy(s_dev_name, name, BLE_DEV_NAME_MAX - 1);
    s_dev_name[BLE_DEV_NAME_MAX - 1] = '\0';

    nvs_handle_t handle;
    if (nvs_open("espnow_cfg", NVS_READWRITE, &handle) == ESP_OK) {
        nvs_set_str(handle, "ble_name", s_dev_name);
        nvs_commit(handle);
        nvs_close(handle);
    }

    ESP_LOGI(TAG, "Device name set to '%s'", s_dev_name);
    return true;
}

// 复位 BLE controller：disable + enable Bluedroid + BT controller
// vs 全 deinit/init 已被验证不可行（在 Wi-Fi 激活时完全破坏 BLE 堆栈）
// disable/enable 后，GAP callback 仍然有效，无需重新注册。
// 注意：必须在扫描/广告完全停止后再调用。
void ble_pairing_reset_controller(void)
{
    // 确保所有 BLE 操作已停止
    if (s_advertising) {
        ble_pairing_stop_advertise();
    }
    if (s_scanning) {
        ble_pairing_stop_scan();
    }

    // 清理后处理定时器
    if (s_post_scan_timer) {
        xTimerStop(s_post_scan_timer, 0);
        xTimerDelete(s_post_scan_timer, 0);
        s_post_scan_timer = NULL;
    }
    vTaskDelay(pdMS_TO_TICKS(100));

    // 清理信号量残留
    xSemaphoreTake(s_scan_stop_sem, 0);

    ESP_LOGI(TAG, "BLE controller reset (disable/enable)...");

    // 1. Disable Bluedroid
    esp_err_t ret = esp_bluedroid_disable();
    ESP_LOGI(TAG, "  [1/4] esp_bluedroid_disable  = %d", ret);
    vTaskDelay(pdMS_TO_TICKS(50));

    // 2. Disable BT controller
    ret = esp_bt_controller_disable();
    ESP_LOGI(TAG, "  [2/4] esp_bt_controller_disable = %d", ret);
    vTaskDelay(pdMS_TO_TICKS(50));

    // 3. Re-enable BT controller
    ret = esp_bt_controller_enable(ESP_BT_MODE_BLE);
    ESP_LOGI(TAG, "  [3/4] esp_bt_controller_enable  = %d", ret);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "BT re-enable failed!");
        return;
    }
    vTaskDelay(pdMS_TO_TICKS(50));

    // 4. Re-enable Bluedroid
    ret = esp_bluedroid_enable();
    ESP_LOGI(TAG, "  [4/4] esp_bluedroid_enable    = %d", ret);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Bluedroid re-enable failed!");
        return;
    }
    vTaskDelay(pdMS_TO_TICKS(50));

    // GAP callback 在 disable/enable 后仍然有效

    // 重置状态
    s_scanning = false;
    s_advertising = false;
    s_burst_mode = false;
    s_scan_retries = 0;

    ESP_LOGI(TAG, "BLE controller reset complete");
}
