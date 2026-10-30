idf_component_register(SRCS "ESP32-S3-LVGL.c" "clock_ui.c" "wifi_mgr.c"
                    INCLUDE_DIRS "."
                    REQUIRES esp_lcd driver esp_timer esp_wifi nvs_flash esp_netif esp_event esp_http_server lvgl)

# 轮播图片：RGB565 二进制（每张 240*240*2 字节），符号为 img1 / img2 / img3
target_add_binary_data(${COMPONENT_LIB} "images/img1.bin" BINARY RENAME_TO img1 ALIGN 4)
target_add_binary_data(${COMPONENT_LIB} "images/img2.bin" BINARY RENAME_TO img2 ALIGN 4)
target_add_binary_data(${COMPONENT_LIB} "images/img3.bin" BINARY RENAME_TO img3 ALIGN 4)
