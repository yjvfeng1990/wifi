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

static const char* TAG = "MAIN";

WebServer g_webServer;

extern "C" void app_main(void)
{
    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "  ESP32-S3 USB WiFi Manager Starting");
    ESP_LOGI(TAG, "========================================");

    wifi_service_init();

    wifi_op_mode_t mode = wifi_service_get_mode();

    ESP_LOGI(TAG, "Step 1: USB Network init...");
    esp_err_t ret = usb_network_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "USB Network init failed: %d", ret);
    }

    ESP_LOGI(TAG, "Step 2: Web Server start...");
    web_server_start(&g_webServer);

    ESP_LOGI(TAG, "Step 3: USB reconnect...");
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

    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "  System Ready");
    ESP_LOGI(TAG, "  USB Network: http://192.168.5.1");
    if (wifi_service_is_ap_active()) {
        ESP_LOGI(TAG, "  AP WiFi:     http://192.168.4.1");
    }
    ESP_LOGI(TAG, "========================================");
}