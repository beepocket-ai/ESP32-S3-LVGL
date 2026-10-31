idf_component_register(SRCS "ESP32-S3-LVGL.c" "clock_ui.c" "wifi_mgr.c"
                    INCLUDE_DIRS "."
                    REQUIRES esp_lcd driver esp_timer esp_wifi nvs_flash esp_netif esp_event esp_http_server lvgl)

# 轮播图片：RGB565 二进制（每张 240*240*2 字节），符号为 img1 ~ img8
target_add_binary_data(${COMPONENT_LIB} "images/img1.bin" BINARY RENAME_TO img1 ALIGN 4)
target_add_binary_data(${COMPONENT_LIB} "images/img2.bin" BINARY RENAME_TO img2 ALIGN 4)
target_add_binary_data(${COMPONENT_LIB} "images/img3.bin" BINARY RENAME_TO img3 ALIGN 4)
target_add_binary_data(${COMPONENT_LIB} "images/img4.bin" BINARY RENAME_TO img4 ALIGN 4)
target_add_binary_data(${COMPONENT_LIB} "images/img5.bin" BINARY RENAME_TO img5 ALIGN 4)
target_add_binary_data(${COMPONENT_LIB} "images/img6.bin" BINARY RENAME_TO img6 ALIGN 4)
target_add_binary_data(${COMPONENT_LIB} "images/img7.bin" BINARY RENAME_TO img7 ALIGN 4)
target_add_binary_data(${COMPONENT_LIB} "images/img8.bin" BINARY RENAME_TO img8 ALIGN 4)
