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

    ESP_LOGI(TAG, "Step 1: USB Network init...");
    esp_err_t ret = usb_network_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "USB Network init failed: %d", ret);
    }

    ESP_LOGI(TAG, "Step 2: Web Server start...");
    web_server_start(&g_webServer);

    ESP_LOGI(TAG, "Step 3: USB reconnect...");
    usb_network_reconnect();

    if (wifi_service_get_mode() != WIFI_OP_MODE_AP && wifi_service_has_config()) {
        nvs_handle_t handle;
        if (nvs_open("wifi_cfg", NVS_READONLY, &handle) == ESP_OK) {
            char ssid[33] = {0};
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
    } else if (wifi_service_get_mode() == WIFI_OP_MODE_AP) {
        ESP_LOGI(TAG, "AP mode active - skip STA auto-connect");
    }

    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "  System Ready");
    ESP_LOGI(TAG, "  USB Network: http://192.168.5.1");
    ESP_LOGI(TAG, "========================================");
}