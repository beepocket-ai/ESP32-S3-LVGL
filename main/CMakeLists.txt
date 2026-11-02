idf_component_register(SRCS "ESP32-S3-LVGL.c" "tarot_ui.c" "tarot_data.c" "tarot_font_16.c"
                    INCLUDE_DIRS "."
                    REQUIRES esp_lcd driver esp_timer lvgl)
