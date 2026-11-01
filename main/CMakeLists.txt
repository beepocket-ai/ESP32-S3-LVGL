idf_component_register(SRCS "ESP32-S3-LVGL.c" "breath_ui.c" "ui_font_16.c" "ui_font_28.c"
                    INCLUDE_DIRS "."
                    REQUIRES esp_lcd driver esp_timer lvgl)
