#pragma once

#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"

/* 启动答案之书界面（在 esp_lcd 面板初始化完成后调用，接管 LVGL 显示与 BOOT 按键） */
void book_ui_start(esp_lcd_panel_io_handle_t io, esp_lcd_panel_handle_t panel);
