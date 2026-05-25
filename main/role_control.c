#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/timers.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "driver/gpio.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "role_control.h"
#include "ble_pairing.h"
#include "wifi_now.h"
#include "led_indicator.h"

static const char* TAG = "ROLE";
static const char* NVS_NS = "espnow_cfg";
static const char* NVS_KEY_ROLE = "role";

static role_type_t s_role = ROLE_OFF;
static role_state_t s_state = ROLE_STATE_IDLE;
static TimerHandle_t s_activity_timer = NULL;
static SemaphoreHandle_t s_role_mutex = NULL;
static TickType_t s_active_start_time = 0;

// GPIO4 防抖
static volatile bool s_gpio_triggered = false;
static TickType_t s_last_gpio_tick = 0;

// 收到的数据计数
static volatile int s_data_recv_count = 0;

// 广播模式周期扫描状态
static bool s_bcast_scanning = false;
static TickType_t s_bcast_phase_start = 0;

static void save_role_to_nvs(void)
{
    nvs_handle_t handle;
    if (nvs_open(NVS_NS, NVS_READWRITE, &handle) != ESP_OK) return;
    uint8_t val = (uint8_t)s_role;
    nvs_set_u8(handle, NVS_KEY_ROLE, val);
    nvs_commit(handle);
    nvs_close(handle);
}

static void load_role_from_nvs(void)
{
    nvs_handle_t handle;
    if (nvs_open(NVS_NS, NVS_READONLY, &handle) != ESP_OK) return;
    uint8_t val = 0;
    if (nvs_get_u8(handle, NVS_KEY_ROLE, &val) == ESP_OK) {
        if (val <= ROLE_RECEIVE) {
            s_role = (role_type_t)val;
        }
    }
    nvs_close(handle);
}

// 停止角色动作
void role_control_stop(void)
{
    xSemaphoreTake(s_role_mutex, portMAX_DELAY);
    if (s_state == ROLE_STATE_IDLE) {
        xSemaphoreGive(s_role_mutex);
        return;
    }
    s_state = ROLE_STATE_IDLE;

    // 停止BLE操作
    if (ble_pairing_is_burst_mode()) {
        ble_pairing_stop_adv_burst();
    }
    if (ble_pairing_is_scanning()) {
        ble_pairing_stop_scan();
    }
    if (ble_pairing_is_advertising()) {
        ble_pairing_stop_advertise();
    }

    // 停止定时器
    if (s_activity_timer) {
        xTimerStop(s_activity_timer, 0);
    }

    // LED关
    led_indicator_set_mode(LED_MODE_OFF);

    xSemaphoreGive(s_role_mutex);

    // BLE controller 复位：disable/enable 控制器
    // 注意：每次迭代间完全复位可清除 BLE controller 内部状态，
    // 但也可能引入额外时序问题。经过实际测试，disable/enable
    // 循环 2-3 次后会损坏 BLE 堆栈，导致后续 BLE 操作完全失效。
    // 这里注释掉复位调用，让 BLE controller 状态持续保持。
    // 扫描/广告的停止已经通过 ble_pairing_stop_* 完成，无需复位。
    // ble_pairing_reset_controller();

    ESP_LOGI(TAG, "Role action stopped");
}

// 重置显示计时器：用于 Start 按钮重新启动 BLE 后，remaining 从 60s 重新倒计时
// 不改变角色状态，仅更新活动起始时间和重置定时器
void role_control_reset_timer(void)
{
    xSemaphoreTake(s_role_mutex, portMAX_DELAY);
    s_active_start_time = xTaskGetTickCount();
    if (s_activity_timer) {
        xTimerReset(s_activity_timer, 0);
    }
    xSemaphoreGive(s_role_mutex);
    ESP_LOGI(TAG, "Display timer reset");
}

// 启动角色动作
bool role_control_start(void)
{
    if (s_role == ROLE_OFF) {
        ESP_LOGW(TAG, "Role is OFF, cannot start");
        return false;
    }

    xSemaphoreTake(s_role_mutex, portMAX_DELAY);
    if (s_state == ROLE_STATE_ACTIVE) {
        // 已经活跃：重置计时器
        if (s_activity_timer) {
            xTimerReset(s_activity_timer, 0);
        }
        s_active_start_time = xTaskGetTickCount();

        // 检查 BLE 操作是否已停止（如活动定时器到期后），需要重新启动
        // 注意：ble_pairing 内部有自己的互斥锁，不依赖 s_role_mutex，因此可以安全地在持有锁时调用
        if (s_role == ROLE_RECEIVE && !ble_pairing_is_scanning()) {
            ble_pairing_set_auto_pair(true);
            ble_pairing_start_scan(60);
            ESP_LOGI(TAG, "Receive (Master): BLE scan restarted");
        } else if (s_role == ROLE_BROADCAST && !ble_pairing_is_advertising()) {
            ble_pairing_set_auto_pair(true);
            ble_pairing_start_advertise(NULL);
            ESP_LOGI(TAG, "Broadcast (Slave): BLE advertise restarted");
        }

        xSemaphoreGive(s_role_mutex);
        ESP_LOGI(TAG, "Role timer reset, %ds remaining", ROLE_ACTIVE_SECONDS);
        return true;
    }

    s_state = ROLE_STATE_ACTIVE;
    s_active_start_time = xTaskGetTickCount();
    s_data_recv_count = 0;
    s_bcast_scanning = false;
    s_bcast_phase_start = xTaskGetTickCount();

    // 启动BLE操作 + LED
    if (s_role == ROLE_RECEIVE) {
        // Master: 扫描发现Slave（60秒扫描发现，之后后处理配对）
        // 扫描完成后 role_task 会保持角色 ACTIVE 状态，ESP-NOW 通信不受影响。
        ble_pairing_set_auto_pair(true);
        ESP_LOGI(TAG, "Receive (Master): starting BLE scan (60s)");
        ble_pairing_start_scan(60);
        led_indicator_set_mode(LED_MODE_BREATHE);
        ESP_LOGI(TAG, "Receive (Master): BLE scan started");
    } else if (s_role == ROLE_BROADCAST) {
        // Slave: 广播自身
        ble_pairing_set_auto_pair(true);
        ble_pairing_start_advertise(NULL);
        led_indicator_set_mode(LED_MODE_BREATHE);
        ESP_LOGI(TAG, "Broadcast (Slave): BLE advertise");
    }

    // 启动30s定时器
    if (s_activity_timer) {
        xTimerStart(s_activity_timer, 0);
    }

    xSemaphoreGive(s_role_mutex);

    ESP_LOGI(TAG, "Role action started, will stop in %ds", ROLE_ACTIVE_SECONDS);
    return true;
}

// BLE操作计时回调：60s到期后停止BLE，设置state=IDLE
// MASTER/SLAVE 统一行为：60s后BLE停止，剩余时间归0
// 后续可通过 START 按钮重新启动 BLE
static void activity_timer_callback(TimerHandle_t timer)
{
    // Stop BLE operations when timer expires
    if (ble_pairing_is_burst_mode()) {
        ble_pairing_stop_adv_burst();
    }
    if (ble_pairing_is_scanning()) {
        ble_pairing_stop_scan();
    }
    if (ble_pairing_is_advertising()) {
        ble_pairing_stop_advertise();
    }

    // 设置state=IDLE：remaining归0，UI正确显示BLE状态
    xSemaphoreTake(s_role_mutex, portMAX_DELAY);
    s_state = ROLE_STATE_IDLE;
    xSemaphoreGive(s_role_mutex);

    ESP_LOGI(TAG, "BLE phase complete (60s), state=IDLE");
}

// GPIO4中断处理
static void IRAM_ATTR gpio_isr_handler(void* arg)
{
    TickType_t now = xTaskGetTickCountFromISR();
    if (now - s_last_gpio_tick < pdMS_TO_TICKS(300)) {
        return; // 防抖
    }
    s_last_gpio_tick = now;
    s_gpio_triggered = true;
}

// 后台任务: 处理GPIO触发 + 更新状态 + 广播模式周期扫描
static void role_task(void* arg)
{
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(500));

        // 处理GPIO触发 (在任务上下文中执行, 可以调用API)
        if (s_gpio_triggered) {
            s_gpio_triggered = false;
            ESP_LOGI(TAG, "GPIO4 triggered, starting role action");
            role_control_start();
        }

        // 更新LED: 收到数据时闪烁
        if (s_data_recv_count > 0) {
            s_data_recv_count = 0;
            if (s_state == ROLE_STATE_ACTIVE) {
                led_indicator_flash_once();
            }
        }

        // 广播模式: 保持BLE广播，不扫描（避免SLAVE间互相配对+干扰ESP-NOW接收）
        if (s_state == ROLE_STATE_ACTIVE && s_role == ROLE_BROADCAST) {
            // SLAVE全程保持广播，不进入扫描阶段
            if (s_bcast_scanning) {
                // 如果之前处于扫描状态，切回广播
                if (ble_pairing_is_scanning()) {
                    ble_pairing_stop_scan();
                }
                if (!ble_pairing_is_advertising()) {
                    ble_pairing_start_advertise(NULL);
                }
                s_bcast_scanning = false;
                ESP_LOGI(TAG, "Broadcast: staying in advertise mode");
            }
        }
    }
}

void role_control_init(void)
{
    if (s_role_mutex) return;

    s_role_mutex = xSemaphoreCreateMutex();

    // 从NVS读取配置
    load_role_from_nvs();
    ESP_LOGI(TAG, "Role loaded from NVS: %d", s_role);

    // GPIO4配置: 输入, 上拉, 下降沿中断
    gpio_config_t io = {
        .pin_bit_mask = (1ULL << ROLE_GPIO_TRIGGER),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_NEGEDGE,
    };
    gpio_config(&io);
    gpio_install_isr_service(0);
    gpio_isr_handler_add(ROLE_GPIO_TRIGGER, gpio_isr_handler, NULL);
    ESP_LOGI(TAG, "GPIO%d configured as trigger (falling edge)", ROLE_GPIO_TRIGGER);

    // 创建30s定时器
    s_activity_timer = xTimerCreate("role_timer",
                                     pdMS_TO_TICKS(ROLE_ACTIVE_SECONDS * 1000),
                                     pdFALSE, NULL, activity_timer_callback);

    // 创建后台任务
    xTaskCreate(role_task, "role_task", 3072, NULL, 5, NULL);

    ESP_LOGI(TAG, "Role control initialized, current role=%d, state=IDLE", s_role);

    // 开机自动启动 BLE（MASTER 扫描 / SLAVE 广播），60s 后自动停止
    // BLE 已在 ble_pairing_init() 中初始化，可安全调用
    if (s_role == ROLE_RECEIVE || s_role == ROLE_BROADCAST) {
        ESP_LOGI(TAG, "Boot auto-start: role=%d", s_role);
        role_control_start();
    }
}

void role_control_set_role(role_type_t role)
{
    if (role > ROLE_RECEIVE) role = ROLE_OFF;

    xSemaphoreTake(s_role_mutex, portMAX_DELAY);

    // 如果正在活动中, 先停止
    bool was_active = (s_state == ROLE_STATE_ACTIVE);
    if (was_active) {
        xSemaphoreGive(s_role_mutex);
        role_control_stop();
        xSemaphoreTake(s_role_mutex, portMAX_DELAY);
    }

    s_role = role;
    save_role_to_nvs();
    xSemaphoreGive(s_role_mutex);

    ESP_LOGI(TAG, "Role set to %d", role);
}

role_type_t role_control_get_role(void)
{
    role_type_t r;
    xSemaphoreTake(s_role_mutex, portMAX_DELAY);
    r = s_role;
    xSemaphoreGive(s_role_mutex);
    return r;
}

role_state_t role_control_get_state(void)
{
    role_state_t st;
    xSemaphoreTake(s_role_mutex, portMAX_DELAY);
    st = s_state;
    xSemaphoreGive(s_role_mutex);
    return st;
}

int role_control_get_remaining(void)
{
    xSemaphoreTake(s_role_mutex, portMAX_DELAY);
    if (s_state != ROLE_STATE_ACTIVE) {
        xSemaphoreGive(s_role_mutex);
        return 0;
    }
    TickType_t elapsed = xTaskGetTickCount() - s_active_start_time;
    int remaining = ROLE_ACTIVE_SECONDS - (elapsed * 1000 / configTICK_RATE_HZ) / 1000;
    if (remaining < 0) remaining = 0;
    xSemaphoreGive(s_role_mutex);
    return remaining;
}

int role_control_get_peer_count(void)
{
    return wifi_now_get_peer_count();
}

void role_control_notify_data(void)
{
    s_data_recv_count++;
}
