#include <stdio.h>
#include <string.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "driver/ledc.h"
#include "led_indicator.h"

static const char* TAG = "LED";

static led_mode_t s_current_mode = LED_MODE_OFF;
static SemaphoreHandle_t s_led_mutex = NULL;

// 收到数据瞬间高亮
static volatile bool s_flash_pending = false;
static TickType_t s_flash_start = 0;

static void led_set_duty(uint32_t duty)
{
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL, duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL);
}

static void led_task(void* arg)
{
    TickType_t last_wake = xTaskGetTickCount();
    const TickType_t interval = pdMS_TO_TICKS(20); // 50Hz 更新

    uint32_t tick = 0;

    while (1) {
        vTaskDelayUntil(&last_wake, interval);
        tick++;

        xSemaphoreTake(s_led_mutex, portMAX_DELAY);
        led_mode_t mode = s_current_mode;
        bool flash = s_flash_pending;
        xSemaphoreGive(s_led_mutex);

        uint32_t duty = 0;

        switch (mode) {
        case LED_MODE_OFF:
            duty = 0;
            break;

        case LED_MODE_BREATHE: {
            // 30%-100% 正弦呼吸, 周期2s = 100 tick (50Hz * 2s)
            float phase = (float)(tick % 100) / 100.0f * 2.0f * 3.14159f;
            float sin_val = sinf(phase);
            // sin: -1~1 → 0~1 → 30%~100%
            float normalized = (sin_val + 1.0f) * 0.5f; // 0~1
            duty = (uint32_t)((30 + normalized * 70.0f) / 100.0f * 255.0f);
            if (duty > 255) duty = 255;
            break;
        }

        case LED_MODE_FAST_PULSE: {
            // 快速脉冲 0-100% 周期0.5s = 25 tick
            float phase = (float)(tick % 25) / 25.0f * 2.0f * 3.14159f;
            float sin_val = sinf(phase);
            float normalized = (sin_val + 1.0f) * 0.5f;
            duty = (uint32_t)(normalized * 255.0f);
            if (duty > 255) duty = 255;
            break;
        }

        case LED_MODE_DATA_FLASH:
        default:
            duty = 0;
            break;
        }

        // 数据接收闪亮: 短暂覆盖为100%亮度
        if (flash) {
            TickType_t elapsed = xTaskGetTickCount() - s_flash_start;
            if (elapsed < pdMS_TO_TICKS(100)) {
                duty = 255;
            } else {
                xSemaphoreTake(s_led_mutex, portMAX_DELAY);
                s_flash_pending = false;
                xSemaphoreGive(s_led_mutex);
            }
        }

        led_set_duty(duty);
    }
}

void led_indicator_init(void)
{
    if (s_led_mutex) return;

    s_led_mutex = xSemaphoreCreateMutex();

    ledc_timer_config_t timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_RESOLUTION,
        .timer_num = LEDC_TIMER,
        .freq_hz = LEDC_FREQ,
        .clk_cfg = LEDC_AUTO_CLK
    };
    ESP_ERROR_CHECK(ledc_timer_config(&timer));

    ledc_channel_config_t channel = {
        .gpio_num = LEDC_GPIO,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_CHANNEL,
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = LEDC_TIMER,
        .duty = 0,
        .hpoint = 0
    };
    ESP_ERROR_CHECK(ledc_channel_config(&channel));

    led_set_duty(0);

    xTaskCreate(led_task, "led_task", 2048, NULL, 5, NULL);

    ESP_LOGI(TAG, "LED indicator initialized on GPIO%d", LEDC_GPIO);
}

void led_indicator_set_mode(led_mode_t mode)
{
    xSemaphoreTake(s_led_mutex, portMAX_DELAY);
    s_current_mode = mode;
    s_flash_pending = false;
    xSemaphoreGive(s_led_mutex);
    ESP_LOGD(TAG, "LED mode set to %d", mode);
}

led_mode_t led_indicator_get_mode(void)
{
    led_mode_t mode;
    xSemaphoreTake(s_led_mutex, portMAX_DELAY);
    mode = s_current_mode;
    xSemaphoreGive(s_led_mutex);
    return mode;
}

void led_indicator_flash_once(void)
{
    xSemaphoreTake(s_led_mutex, portMAX_DELAY);
    s_flash_pending = true;
    s_flash_start = xTaskGetTickCount();
    xSemaphoreGive(s_led_mutex);
}
