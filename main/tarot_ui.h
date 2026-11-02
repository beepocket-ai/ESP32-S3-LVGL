#pragma once

#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"

/* 电子塔罗牌：在已完成初始化的 esp_lcd 面板上启动 LVGL 界面 */
void tarot_ui_start(esp_lcd_panel_io_handle_t io, esp_lcd_panel_handle_t panel);
