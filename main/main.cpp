#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "usb_network.h"
#include "wifi_service.h"
#include "web_server.h"
#include "ble_pairing.h"
#include "wifi_now.h"
#include "role_control.h"
#include "led_indicator.h"

static const char* TAG = "MAIN";

WebServer g_webServer;

extern "C" void app_main(void)
{
    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "  ESP32-S3 USB WiFi Manager Starting");
    ESP_LOGI(TAG, "========================================");

    // 压制 WiFi 固件的 802.11 Block ACK 高频日志（tid/ssn/winSize 消息），
    // 避免在 WiFi 连接频繁变更时串口泛洪导致 web server 无响应。
    esp_log_level_set("wifi", ESP_LOG_WARN);

    wifi_service_init();

    wifi_op_mode_t mode = wifi_service_get_mode();

    ESP_LOGI(TAG, "Step 1: USB Network init...");
    esp_err_t ret = usb_network_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "USB Network init failed: %d", ret);
    }

    ESP_LOGI(TAG, "Step 2: Web Server start...");
    web_server_start(&g_webServer);

    ESP_LOGI(TAG, "Step 3: ESP-NOW init...");
    wifi_now_init();

    ESP_LOGI(TAG, "Step 4: BLE Pairing init...");
    ble_pairing_init();

    ESP_LOGI(TAG, "Step 4b: LED Indicator init...");
    led_indicator_init();

    ESP_LOGI(TAG, "Step 4c: Role Control init...");
    role_control_init();

    ESP_LOGI(TAG, "Step 5: USB reconnect...");
    usb_network_reconnect();

    bool sta_should_connect = (mode == WIFI_OP_MODE_STA || mode == WIFI_OP_MODE_APSTA)
                               && wifi_service_has_config();

    if (sta_should_connect) {
        nvs_handle_t handle;
        if (nvs_open("wifi_cfg", NVS_READONLY, &handle) == ESP_OK) {
            char ssid[33]     = {0};
            char password[65] = {0};
            size_t len = sizeof(ssid);
            nvs_get_str(handle, "ssid", ssid, &len);
            len = sizeof(password);
            nvs_get_str(handle, "password", password, &len);
            nvs_close(handle);

            if (strlen(ssid) > 0) {
                ESP_LOGI(TAG, "Found saved WiFi config, auto-connecting to: %s", ssid);
                wifi_service_connect(ssid, password);
            }
        }
    }

    if (mode == WIFI_OP_MODE_AP) {
        ESP_LOGI(TAG, "AP-only mode active");
    } else if (mode == WIFI_OP_MODE_APSTA) {
        ESP_LOGI(TAG, "AP+STA dual mode active");
    }

    // SLAVE / MASTER 自动启动：
    // - SLAVE (ROLE_BROADCAST): 自动启动 BLE 广播，让 MASTER 通过 BLE 扫描发现
    // - MASTER (ROLE_RECEIVE):  自动启动 BLE 扫描，发现并配对附近的 SLAVE 设备
    // WiFi STA 需要先连接路由器获取 IP 和稳定信道后，才能安全启动 BLE。
    role_type_t start_role = role_control_get_role();
    if (start_role == ROLE_BROADCAST || start_role == ROLE_RECEIVE) {
        const char* role_name = (start_role == ROLE_BROADCAST) ? "SLAVE" : "MASTER";
        ESP_LOGI(TAG, "%s role detected, auto-starting in 8s...", role_name);
        vTaskDelay(pdMS_TO_TICKS(8000));
        role_control_start();
    }

    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "  System Ready");
    ESP_LOGI(TAG, "  USB Network: http://192.168.5.1");
    if (wifi_service_is_ap_active()) {
        ESP_LOGI(TAG, "  AP WiFi:     http://192.168.4.1");
    }
    ESP_LOGI(TAG, "========================================");

    // 永真循环：阻止 app_main() 返回，避免 ESP-IDF 框架持续打印
    // "main_task: Returned from app_main()" 串口消息，该泛洪会占用大量
    // CPU 时间导致 HTTP 服务器响应退化到分钟级。
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(60000));
    }
}