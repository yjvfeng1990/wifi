#ifndef LED_INDICATOR_H
#define LED_INDICATOR_H

#include <stdint.h>
#include <stdbool.h>
#include "driver/gpio.h"

#ifdef __cplusplus
extern "C" {
#endif

#define LEDC_GPIO           GPIO_NUM_2
#define LEDC_TIMER          LEDC_TIMER_0
#define LEDC_CHANNEL        LEDC_CHANNEL_0
#define LEDC_FREQ           5000
#define LEDC_RESOLUTION     LEDC_TIMER_8_BIT

typedef enum {
    LED_MODE_OFF = 0,
    LED_MODE_BREATHE,       // 呼吸 30%-100%
    LED_MODE_FAST_PULSE,    // 快速脉冲 (发送时)
    LED_MODE_DATA_FLASH,    // 收到数据时闪一下
} led_mode_t;

void led_indicator_init(void);
void led_indicator_set_mode(led_mode_t mode);
led_mode_t led_indicator_get_mode(void);
void led_indicator_flash_once(void);  // 收到数据时调用

#ifdef __cplusplus
}
#endif

#endif
