idf_component_register(SRCS "ESP32-S3-LVGL.c" "book_ui.c" "book_font_16.c" "book_font_28.c"
                    INCLUDE_DIRS "."
                    REQUIRES esp_lcd driver esp_timer lvgl)

# 答案语料（UTF-8 JSON，运行时按下 BOOT 随机抽取）
target_add_binary_data(${COMPONENT_LIB} "answers.json" BINARY RENAME_TO answers_data ALIGN 4)
