/*
 * LVGL 时钟 / 仪表盘 UI
 *
 * 由 ESP32-S3-LVGL.c 在 esp_lcd 面板初始化完成后调用 clock_ui_start()。
 * 自己实现 LVGL 显示端口（flush -> esp_lcd_panel_draw_bitmap），不依赖 esp_lvgl_port。
 *
 * 功能：
 *   - 模拟表盘（时/分/秒三根指针）+ 数字时间 + 日期 + WiFi/NTP 状态
 *   - 首次上电（或连接失败）时进入配网模式：屏幕提示热点名与 192.168.4.1，
 *     手机连热点后网页填写 WiFi，保存重启后自动联网 NTP 对时（东八区）
 *   - BOOT 键：短按切换「表盘 / 图片轮播（1.png 2.png 3.png 每 5 秒）」，
 *     长按 5 秒清除 WiFi 配置并重启
 */

#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <string.h>
#include <time.h>
#include <math.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "lvgl.h"

#include "clock_ui.h"
#include "wifi_mgr.h"
#include "driver/gpio.h"

#define LCD_H_RES   240
#define LCD_V_RES   240
#define BUF_LINES   40                      // 局部渲染缓冲（40 行，双缓冲约 38KB）

#define IMG_COUNT           3               // 1.png / 2.png / 3.png
#define BOOT_BTN_GPIO       0               // 开发板 BOOT 键（GPIO0，按下为低）
#define BOOT_HOLD_RESET_MS  5000            // 长按 5 秒重置 WiFi 配置
#define BTN_SHORT_PRESS_MS  100             // 松开时按住时长 >= 100ms 才算一次按键

/* 28 号字体未启用时退回默认 14 号字体（菜单里可开启：Component config → LVGL → Font usage） */
#if CONFIG_LV_FONT_MONTSERRAT_28
#define CLOCK_TIME_FONT     (&lv_font_montserrat_28)
#else
#define CLOCK_TIME_FONT     (&lv_font_montserrat_14)
#endif

static const char *TAG = "clock_ui";

static esp_lcd_panel_io_handle_t s_io;
static esp_lcd_panel_handle_t s_panel;
static SemaphoreHandle_t s_flush_done;

static uint16_t s_buf1[LCD_H_RES * BUF_LINES] __attribute__((aligned(4)));
static uint16_t s_buf2[LCD_H_RES * BUF_LINES] __attribute__((aligned(4)));

static lv_obj_t *s_dial;
static lv_obj_t *s_needle_hour;
static lv_obj_t *s_needle_min;
static lv_obj_t *s_needle_sec;
static lv_obj_t *s_time_label;
static lv_obj_t *s_date_label;
static lv_obj_t *s_status_label;

static lv_obj_t *s_clock_layer;             // 表盘层（整体显隐）
static lv_obj_t *s_img;                     // 轮播图片控件
static lv_timer_t *s_slide_timer;           // 5 秒切换定时器
static bool s_slideshow_on;
static int s_img_index;
static lv_image_dsc_t s_img_dsc[IMG_COUNT];
static SemaphoreHandle_t s_lvgl_mutex;      // 保护跨线程的 LVGL 调用

// 由 CMake 的 target_add_binary_data 生成（240x240 RGB565，115200 字节）
extern const uint8_t img1[];
extern const uint8_t img2[];
extern const uint8_t img3[];

/* ---------------- LVGL 显示端口 ---------------- */
static bool on_color_done(esp_lcd_panel_io_handle_t io, esp_lcd_panel_io_event_data_t *edata, void *ctx)
{
    (void)io;
    (void)edata;
    (void)ctx;
    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR(s_flush_done, &woken);
    return woken == pdTRUE;
}

static void flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    int w = area->x2 - area->x1 + 1;
    int h = area->y2 - area->y1 + 1;
    uint16_t *px = (uint16_t *)px_map;

    // LVGL 是小端 RGB565，SPI 屏需要大端（逐像素交换字节）
    int n = w * h;
    for (int i = 0; i < n; i++) {
        uint16_t v = px[i];
        px[i] = (uint16_t)((v << 8) | (v >> 8));
    }

    esp_lcd_panel_draw_bitmap(s_panel, area->x1, area->y1, area->x2 + 1, area->y2 + 1, px);
    xSemaphoreTake(s_flush_done, portMAX_DELAY);   // 等 SPI 传完，缓冲区才能被 LVGL 复用
    lv_display_flush_ready(disp);
}

static uint32_t tick_cb(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000); // 毫秒
}

static void lvgl_task(void *arg)
{
    (void)arg;
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(5));
        if (xSemaphoreTake(s_lvgl_mutex, portMAX_DELAY) == pdTRUE) {
            lv_timer_handler();
            xSemaphoreGive(s_lvgl_mutex);
        }
    }
}

/* ---------------- 界面 ---------------- */
/* 创建一个无样式的基础矩形（用于刻度和指针，可用 transform 旋转） */
static lv_obj_t *make_rect(lv_obj_t *parent, int x, int y, int w, int h, uint32_t color)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_size(o, w, h);
    lv_obj_set_style_bg_color(o, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(o, LV_RADIUS_CIRCLE, 0);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    return o;
}

#define DIAL_SIZE   168
#define DIAL_CX     (DIAL_SIZE / 2)

static void build_ui(void)
{
    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x0B1014), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);

    // 表盘层：所有时钟控件都放这里，方便整体隐藏/显示
    s_clock_layer = lv_obj_create(scr);
    lv_obj_remove_style_all(s_clock_layer);
    lv_obj_set_size(s_clock_layer, LCD_H_RES, LCD_V_RES);
    lv_obj_set_pos(s_clock_layer, 0, 0);
    lv_obj_clear_flag(s_clock_layer, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    // 表盘容器 + 外圈
    s_dial = lv_obj_create(s_clock_layer);
    lv_obj_remove_style_all(s_dial);
    lv_obj_set_size(s_dial, DIAL_SIZE, DIAL_SIZE);
    lv_obj_align(s_dial, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_clear_flag(s_dial, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *ring = lv_obj_create(s_dial);
    lv_obj_remove_style_all(ring);
    lv_obj_set_size(ring, DIAL_SIZE, DIAL_SIZE);
    lv_obj_set_style_radius(ring, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(ring, 2, 0);
    lv_obj_set_style_border_color(ring, lv_color_hex(0x22303C), 0);
    lv_obj_clear_flag(ring, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    // 12 个整点刻度（先放在圆周对应位置，再绕自身中心转到径向）
    for (int i = 0; i < 12; i++) {
        float a = (float)i * 30.0f * 3.14159265f / 180.0f;
        int x = (int)(DIAL_CX + (DIAL_CX - 8) * sinf(a)) - 1;
        int y = (int)(DIAL_CX - (DIAL_CX - 8) * cosf(a)) - 5;
        lv_obj_t *tick = make_rect(s_dial, x, y, 2, 10, 0x66788A);
        lv_obj_set_style_transform_rotation(tick, i * 300, 0); // 单位 0.1 度
    }

    // 三根指针：以底边中点为旋转轴
    s_needle_hour = make_rect(s_dial, DIAL_CX - 2, DIAL_CX - 50, 4, 50, 0xE6EDF3);
    lv_obj_set_style_transform_pivot_x(s_needle_hour, 2, 0);
    lv_obj_set_style_transform_pivot_y(s_needle_hour, 50, 0);

    s_needle_min = make_rect(s_dial, DIAL_CX - 1, DIAL_CX - 66, 3, 66, 0x35D0FF);
    lv_obj_set_style_transform_pivot_x(s_needle_min, 1, 0);
    lv_obj_set_style_transform_pivot_y(s_needle_min, 66, 0);

    s_needle_sec = make_rect(s_dial, DIAL_CX - 1, DIAL_CX - 56, 2, 56, 0xFF4D6D);
    lv_obj_set_style_transform_pivot_x(s_needle_sec, 1, 0);
    lv_obj_set_style_transform_pivot_y(s_needle_sec, 56, 0);

    // 中心圆点
    make_rect(s_dial, DIAL_CX - 4, DIAL_CX - 4, 8, 8, 0xE6EDF3);

    // 数字时间
    s_time_label = lv_label_create(s_clock_layer);
    lv_obj_set_style_text_font(s_time_label, CLOCK_TIME_FONT, 0);
    lv_obj_set_style_text_color(s_time_label, lv_color_hex(0xE6EDF3), 0);
    lv_label_set_text(s_time_label, "00:00:00");
    lv_obj_align(s_time_label, LV_ALIGN_TOP_MID, 0, 164);

    // 日期
    s_date_label = lv_label_create(s_clock_layer);
    lv_obj_set_style_text_color(s_date_label, lv_color_hex(0x8A98A8), 0);
    lv_label_set_text(s_date_label, "BOOT");
    lv_obj_align(s_date_label, LV_ALIGN_TOP_MID, 0, 198);

    // WiFi / 状态
    s_status_label = lv_label_create(s_clock_layer);
    lv_obj_set_style_text_color(s_status_label, lv_color_hex(0x4CD964), 0);
    lv_label_set_text(s_status_label, "STARTING...");
    lv_obj_align(s_status_label, LV_ALIGN_TOP_MID, 0, 220);

    // 轮播图片（默认隐藏，BOOT 短按时显示）
    s_img = lv_image_create(scr);
    lv_obj_set_pos(s_img, 0, 0);
    lv_obj_add_flag(s_img, LV_OBJ_FLAG_HIDDEN);
}

static void clock_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    if (s_slideshow_on) {
        return;                             // 轮播时表盘层隐藏，不用刷新
    }
    wifi_mgr_state_t st = wifi_mgr_state();
    bool synced = wifi_mgr_time_synced();
    int hour, min, sec;
    char date[48] = "";

    if (synced) {
        time_t now = time(NULL);
        struct tm tm_now;
        localtime_r(&now, &tm_now);
        hour = tm_now.tm_hour;
        min = tm_now.tm_min;
        sec = tm_now.tm_sec;
        strftime(date, sizeof(date), "%Y-%m-%d %a", &tm_now);
    } else {
        int64_t us = esp_timer_get_time();
        sec = (int)((us / 1000000) % 60);
        min = (int)((us / 60000000) % 60);
        hour = (int)((us / 3600000000LL) % 12);
    }

    // 旋转指针（0.1 度单位）：秒 6°/s，分 6°/min，时 30°/h
    lv_obj_set_style_transform_rotation(s_needle_sec, (sec * 60) % 3600, 0);
    lv_obj_set_style_transform_rotation(s_needle_min, (min * 60 + sec) % 3600, 0);
    lv_obj_set_style_transform_rotation(s_needle_hour, ((hour % 12) * 300 + min * 5) % 3600, 0);

    lv_label_set_text_fmt(s_time_label, "%02d:%02d:%02d", hour, min, sec);

    char ap_ssid[32] = { 0 };
    char line[40] = { 0 };
    wifi_mgr_get_ap_ssid(ap_ssid, sizeof(ap_ssid));

    if (st == WIFI_MGR_AP_CONFIG) {
        // 配网模式：屏幕提示热点名和配置网址
        lv_label_set_text(s_date_label, "SETUP: 192.168.4.1");
        snprintf(line, sizeof(line), "AP: %s", ap_ssid);
        lv_obj_set_style_text_color(s_status_label, lv_color_hex(0xFFB020), 0);
        lv_label_set_text(s_status_label, line);
    } else if (st == WIFI_MGR_CONNECTING) {
        lv_label_set_text(s_date_label, "CONNECTING TO WIFI");
        lv_obj_set_style_text_color(s_status_label, lv_color_hex(0x8A98A8), 0);
        lv_label_set_text(s_status_label, "WAITING FOR IP...");
    } else {
        lv_label_set_text(s_date_label, synced ? date : "SYNCING TIME...");
        lv_obj_set_style_text_color(s_status_label, lv_color_hex(0x4CD964), 0);
        lv_label_set_text(s_status_label, synced ? "WIFI OK  NTP SYNCED" : "WIFI OK");
    }
}

/* ---------------- 图片轮播 ---------------- */
static void init_images(void)
{
    static const uint8_t *const data[IMG_COUNT] = { img1, img2, img3 };
    for (int i = 0; i < IMG_COUNT; i++) {
        memset(&s_img_dsc[i], 0, sizeof(s_img_dsc[i]));
        s_img_dsc[i].header.magic = LV_IMAGE_HEADER_MAGIC;
        s_img_dsc[i].header.cf = LV_COLOR_FORMAT_RGB565;
        s_img_dsc[i].header.w = LCD_H_RES;
        s_img_dsc[i].header.h = LCD_V_RES;
        s_img_dsc[i].header.stride = LCD_H_RES * 2;
        s_img_dsc[i].data_size = LCD_H_RES * LCD_V_RES * 2;
        s_img_dsc[i].data = data[i];
    }
}

static void slideshow_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    if (!s_slideshow_on) {
        return;
    }
    s_img_index = (s_img_index + 1) % IMG_COUNT;
    lv_image_set_src(s_img, &s_img_dsc[s_img_index]);
}

static void toggle_slideshow(void)
{
    if (xSemaphoreTake(s_lvgl_mutex, pdMS_TO_TICKS(200)) != pdTRUE) {
        return;
    }
    s_slideshow_on = !s_slideshow_on;
    if (s_slideshow_on) {
        ESP_LOGI(TAG, "boot key: show image slideshow");
        s_img_index = 0;
        lv_image_set_src(s_img, &s_img_dsc[s_img_index]);
        lv_obj_clear_flag(s_img, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_clock_layer, LV_OBJ_FLAG_HIDDEN);
        lv_timer_resume(s_slide_timer);
    } else {
        ESP_LOGI(TAG, "boot key: back to clock");
        lv_obj_add_flag(s_img, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(s_clock_layer, LV_OBJ_FLAG_HIDDEN);
        lv_timer_pause(s_slide_timer);
        clock_timer_cb(NULL);               // 立即刷新，避免显示旧时间
    }
    xSemaphoreGive(s_lvgl_mutex);
}

/* ---------------- BOOT 按键 ---------------- */
static void boot_btn_task(void *arg)
{
    (void)arg;
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << BOOT_BTN_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io);

    int held_ms = 0;
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(20));
        if (gpio_get_level(BOOT_BTN_GPIO) == 0) {
            held_ms += 20;
            if (held_ms % 1000 == 0 && held_ms < BOOT_HOLD_RESET_MS) {
                ESP_LOGW(TAG, "BOOT held %d/%d s (keep holding to reset wifi)",
                         held_ms / 1000, BOOT_HOLD_RESET_MS / 1000);
            }
            if (held_ms >= BOOT_HOLD_RESET_MS) {
                wifi_mgr_clear_config();     // 内部会重启
                held_ms = 0;
            }
        } else {
            if (held_ms >= BTN_SHORT_PRESS_MS && held_ms < BOOT_HOLD_RESET_MS) {
                toggle_slideshow();          // 短按：表盘 <-> 图片轮播
            }
            held_ms = 0;
        }
    }
}

/* ---------------- 对外入口 ---------------- */
void clock_ui_start(esp_lcd_panel_io_handle_t io, esp_lcd_panel_handle_t panel)
{
    s_io = io;
    s_panel = panel;

    s_flush_done = xSemaphoreCreateBinary();
    assert(s_flush_done);
    esp_lcd_panel_io_callbacks_t cbs = {
        .on_color_trans_done = on_color_done,
    };
    ESP_ERROR_CHECK(esp_lcd_panel_io_register_event_callbacks(s_io, &cbs, NULL));

    lv_init();
    lv_tick_set_cb(tick_cb);

    lv_display_t *disp = lv_display_create(LCD_H_RES, LCD_V_RES);
    lv_display_set_flush_cb(disp, flush_cb);
    lv_display_set_buffers(disp, s_buf1, s_buf2, sizeof(s_buf1), LV_DISPLAY_RENDER_MODE_PARTIAL);

    s_lvgl_mutex = xSemaphoreCreateMutex();
    assert(s_lvgl_mutex);

    init_images();
    build_ui();
    lv_timer_create(clock_timer_cb, 500, NULL);

    s_slide_timer = lv_timer_create(slideshow_timer_cb, 5000, NULL);
    lv_timer_pause(s_slide_timer);           // 进入轮播时才启动 5 秒切换

    xTaskCreate(lvgl_task, "lvgl", 5120, NULL, 5, NULL);

    wifi_mgr_start();
    xTaskCreate(boot_btn_task, "boot_btn", 3072, NULL, 4, NULL);
    ESP_LOGI(TAG, "LVGL clock UI started");
}
