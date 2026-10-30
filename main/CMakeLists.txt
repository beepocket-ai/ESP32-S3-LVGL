idf_component_register(SRCS "ESP32-S3-LVGL.c" "clock_ui.c" "wifi_mgr.c"
                    INCLUDE_DIRS "."
                    REQUIRES esp_lcd driver esp_timer esp_wifi nvs_flash esp_netif esp_event esp_http_server lvgl)
