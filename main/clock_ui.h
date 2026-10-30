#pragma once

#include "esp_lcd_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 在已初始化的 esp_lcd 面板上启动 LVGL 时钟 UI（内部自建 LVGL 任务） */
void clock_ui_start(esp_lcd_panel_io_handle_t io, esp_lcd_panel_handle_t panel);

#ifdef __cplusplus
}
#endif
