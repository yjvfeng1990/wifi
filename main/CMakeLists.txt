idf_component_register(SRCS "main.cpp"
                         "usb_network.cpp"
                         "wifi_service.cpp"
                         "web_server.cpp"
                    INCLUDE_DIRS "."
                    REQUIRES nvs_flash esp_wifi esp_netif esp_event esp_http_server esp_timer driver tinyusb)