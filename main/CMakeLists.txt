idf_component_register(SRCS "main.cpp"
                         "usb_network.cpp"
                         "wifi_service.cpp"
                         "wifi_now.c"
                         "ble_pairing.c"
                         "web_server.cpp"
                         "led_indicator.c"
                         "role_control.c"
                    INCLUDE_DIRS "."
                    REQUIRES nvs_flash esp_wifi esp_netif esp_event esp_http_server esp_timer driver tinyusb bt
                    PRIV_REQUIRES esp_driver_ledc spi_flash esp_partition)