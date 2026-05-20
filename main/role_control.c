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

    ESP_LOGI(TAG, "Role action stopped");
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
        // 已经活跃，重置计时器
        if (s_activity_timer) {
            xTimerReset(s_activity_timer, 0);
        }
        s_active_start_time = xTaskGetTickCount();
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
        // Master: 扫描发现Slave
        ble_pairing_set_auto_pair(true);
        ble_pairing_start_scan(ROLE_ACTIVE_SECONDS + 5); // 多5s确保覆盖
        led_indicator_set_mode(LED_MODE_BREATHE);
        ESP_LOGI(TAG, "Receive mode: BLE scan started");
    } else if (s_role == ROLE_BROADCAST) {
        // Slave: 广播自身让对方发现, 开启auto-pair以响应MASTER的BLE回复
        ble_pairing_set_auto_pair(true);
        ble_pairing_start_advertise(NULL);
        led_indicator_set_mode(LED_MODE_BREATHE);
        ESP_LOGI(TAG, "Broadcast mode: BLE advertising started");
    }

    // 启动30s定时器
    if (s_activity_timer) {
        xTimerStart(s_activity_timer, 0);
    }

    xSemaphoreGive(s_role_mutex);

    ESP_LOGI(TAG, "Role action started, will stop in %ds", ROLE_ACTIVE_SECONDS);
    return true;
}

// 30s定时器回调
static void activity_timer_callback(TimerHandle_t timer)
{
    ESP_LOGI(TAG, "Activity timer expired, stopping...");
    role_control_stop();
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

        // 更新LED: 收到数据时闪一下
        if (s_data_recv_count > 0) {
            s_data_recv_count = 0;
            if (s_state == ROLE_STATE_ACTIVE) {
                led_indicator_flash_once();
            }
        }

        // 广播模式: 周期扫描以发现MASTER的BLE回应
        if (s_state == ROLE_STATE_ACTIVE && s_role == ROLE_BROADCAST) {
            TickType_t now = xTaskGetTickCount();
            TickType_t elapsed = now - s_bcast_phase_start;

            if (!s_bcast_scanning) {
                // 广告阶段: 12秒后切换为扫描
                if (elapsed > pdMS_TO_TICKS(12000)) {
                    if (ble_pairing_is_advertising()) {
                        ble_pairing_stop_advertise();
                    }
                    if (!ble_pairing_is_scanning()) {
                        ble_pairing_start_scan(6);  // 扫描6秒
                    }
                    s_bcast_scanning = true;
                    s_bcast_phase_start = now;
                    ESP_LOGI(TAG, "Broadcast: switching to scan phase");
                }
            } else {
                // 扫描阶段: 6秒后切换回广告
                if (elapsed > pdMS_TO_TICKS(6000) || !ble_pairing_is_scanning()) {
                    if (ble_pairing_is_scanning()) {
                        ble_pairing_stop_scan();
                    }
                    if (!ble_pairing_is_advertising()) {
                        ble_pairing_start_advertise(NULL);
                    }
                    s_bcast_scanning = false;
                    s_bcast_phase_start = now;
                    ESP_LOGI(TAG, "Broadcast: switching back to advertise phase");
                }
            }
        }

        // 状态活跃时如果BLE已停止(异常情况), 也停止我们的定时器
        // 注意: 突发广播模式下 advertising=true, 不做停止
        if (s_state == ROLE_STATE_ACTIVE && s_role == ROLE_RECEIVE) {
            if (!ble_pairing_is_scanning() && !ble_pairing_is_advertising()
                && !ble_pairing_is_burst_mode()) {
                TickType_t elapsed = xTaskGetTickCount() - s_active_start_time;
                if (elapsed > pdMS_TO_TICKS(ROLE_ACTIVE_SECONDS * 1000 + 5000)) {
                    ESP_LOGW(TAG, "BLE already stopped, stopping role");
                    role_control_stop();
                }
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
