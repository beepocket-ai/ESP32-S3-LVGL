/*
 * 电子塔罗牌（大阿尔克纳 22 张）
 *
 * 由 ESP32-S3-LVGL.c 在 esp_lcd 面板初始化完成后调用 tarot_ui_start()。
 * 自己实现 LVGL 显示端口（flush -> esp_lcd_panel_draw_bitmap），不依赖 esp_lvgl_port。
 *
 * 功能：
 *   - 待机：显示牌背 + 「按下 BOOT 抽一张牌」
 *   - BOOT 短按：随机抽 0..21 号大阿尔克纳，50% 正位 / 50% 逆位
 *       · 逆位时牌面 180° 翻转
 *       · 牌名 + 释义同步切换（正位金色 / 逆位紫色）
 *       · 抽牌时淡入 + 上滑 + 金色光晕收束
 *   - 再按 BOOT：重新抽一张
 *
 * 卡面数据：30x40 原生 RGB565（tools/gen_tarot.py 生成），运行时放大 4x 成 120x160。
 */

#include <stdio.h>
#include <assert.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "driver/gpio.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "lvgl.h"

#include "tarot_ui.h"
#include "tarot_data.h"
#include "tarot_font.h"

#define LCD_H_RES   240
#define LCD_V_RES   240
#define BUF_LINES   40                      // 局部渲染缓冲（40 行，双缓冲约 38KB）
#define LVGL_TICK_MS 10                     // LVGL 主循环周期（100Hz tick 下必须 >= 1 tick）

#define BOOT_BTN_GPIO       0               // 开发板 BOOT 键（GPIO0，按下为低）
#define BTN_SHORT_PRESS_MS  100             // 按住 >= 100ms 才算一次按键
#define AUTO_DRAW_MS        5000            // 每 5 秒自动抽一张（等同于自动按 BOOT）

/* 布局：牌名 0..17，卡牌 20..192，释义 196..230 */
#define CARD_W      132
#define CARD_H      172
#define CARD_Y      20
#define SCALE       4
#define IMG_W       (TAROT_ART_W * SCALE)   // 120
#define IMG_H       (TAROT_ART_H * SCALE)   // 160

/* 配色 */
#define COLOR_BG_TOP    0x120E26            // 夜空（上）
#define COLOR_BG_BOT    0x04030A            // 夜空（下）
#define COLOR_GOLD      0xC9A24B
#define COLOR_GOLD_LT   0xE7C878
#define COLOR_TEXT      0xE6E0EE
#define COLOR_DIM       0x8E88A6
#define COLOR_UPRIGHT   0xEBD08A            // 正位牌名
#define COLOR_REVERSED  0xB9A6E0            // 逆位牌名

static const char *TAG = "tarot_ui";

static esp_lcd_panel_io_handle_t s_io;
static esp_lcd_panel_handle_t s_panel;
static SemaphoreHandle_t s_flush_done;
static SemaphoreHandle_t s_lvgl_mutex;

static uint16_t s_buf1[LCD_H_RES * BUF_LINES] __attribute__((aligned(4)));
static uint16_t s_buf2[LCD_H_RES * BUF_LINES] __attribute__((aligned(4)));

static uint16_t *s_img_buf;                 // 120x160 RGB565（放大后的卡面）
static lv_image_dsc_t s_img_dsc;

static lv_obj_t *s_card;                    // 卡牌外框
static lv_obj_t *s_img;                     // 牌面（image）
static lv_obj_t *s_name;                    // 牌名 + 正逆位
static lv_obj_t *s_meaning;                 // 一句释义

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

    // LVGL 小端 RGB565，SPI 屏要大端（逐像素交换字节）
    int n = w * h;
    for (int i = 0; i < n; i++) {
        uint16_t v = px[i];
        px[i] = (uint16_t)((v << 8) | (v >> 8));
    }

    esp_lcd_panel_draw_bitmap(s_panel, area->x1, area->y1, area->x2 + 1, area->y2 + 1, px);
    xSemaphoreTake(s_flush_done, portMAX_DELAY);   // 等 SPI 传完，缓冲区才能复用
    lv_display_flush_ready(disp);
}

static uint32_t tick_cb(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static void lvgl_task(void *arg)
{
    (void)arg;
    while (1) {
        /* CONFIG_FREERTOS_HZ=100 时 pdMS_TO_TICKS(5) == 0，vTaskDelay(0) 会让
         * 优先级更高的 lvgl 任务一直占住 CPU0，饿死 IDLE0 触发任务看门狗。
         * 这里强制至少 1 个 tick。 */
        TickType_t period = pdMS_TO_TICKS(LVGL_TICK_MS);
        vTaskDelay(period > 0 ? period : 1);
        if (xSemaphoreTake(s_lvgl_mutex, portMAX_DELAY) == pdTRUE) {
            lv_timer_handler();
            xSemaphoreGive(s_lvgl_mutex);
        }
    }
}

/* ---------------- 卡面放大 ---------------- */
/* 把 30x40 的原生像素按 4x 最近邻展开成 120x160；reversed=true 时同时做 180° 翻转 */
static void decode_art(const uint16_t *src, bool reversed)
{
    for (int y = 0; y < TAROT_ART_H; y++) {
        for (int x = 0; x < TAROT_ART_W; x++) {
            int sx = reversed ? (TAROT_ART_W - 1 - x) : x;
            int sy = reversed ? (TAROT_ART_H - 1 - y) : y;
            uint16_t c = src[sy * TAROT_ART_W + sx];

            uint16_t *row = s_img_buf + (size_t)(y * SCALE) * IMG_W + x * SCALE;
            for (int k = 0; k < SCALE; k++) {
                row[0] = c;
                row[1] = c;
                row[2] = c;
                row[3] = c;
                row += IMG_W;
            }
        }
    }
}

/* ---------------- 抽牌动画 ---------------- */
static void anim_opa_cb(void *obj, int32_t v)
{
    lv_obj_set_style_opa((lv_obj_t *)obj, (lv_opa_t)v, 0);
}

static void anim_y_cb(void *obj, int32_t v)
{
    lv_obj_set_y((lv_obj_t *)obj, v);
}

static void anim_shadow_cb(void *obj, int32_t v)
{
    lv_obj_set_style_shadow_opa((lv_obj_t *)obj, (lv_opa_t)v, 0);
}

static void reveal_anim(void)
{
    lv_anim_t a;

    lv_anim_init(&a);                       // 淡入
    lv_anim_set_var(&a, s_card);
    lv_anim_set_values(&a, 0, 255);
    lv_anim_set_time(&a, 380);
    lv_anim_set_exec_cb(&a, anim_opa_cb);
    lv_anim_start(&a);

    lv_anim_init(&a);                       // 上滑落位
    lv_anim_set_var(&a, s_card);
    lv_anim_set_values(&a, CARD_Y + 16, CARD_Y);
    lv_anim_set_time(&a, 460);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_set_exec_cb(&a, anim_y_cb);
    lv_anim_start(&a);

    lv_anim_init(&a);                       // 金色光晕收束
    lv_anim_set_var(&a, s_card);
    lv_anim_set_values(&a, 170, 48);
    lv_anim_set_time(&a, 700);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_set_exec_cb(&a, anim_shadow_cb);
    lv_anim_start(&a);
}

/* ---------------- 抽牌 ---------------- */
static void show_back(void)
{
    decode_art(tarot_card_back, false);
    lv_obj_invalidate(s_img);
    lv_label_set_text(s_name, "大阿尔克纳");
    lv_obj_set_style_text_color(s_name, lv_color_hex(COLOR_GOLD_LT), 0);
    lv_label_set_text(s_meaning, "按下 BOOT 抽一张牌\n心中默念你的问题");
    lv_obj_set_style_text_color(s_meaning, lv_color_hex(COLOR_DIM), 0);
}

static void draw_card(void)
{
    int idx = (int)(esp_random() % TAROT_COUNT);
    bool reversed = (esp_random() & 1) != 0;
    const tarot_card_t *c = &tarot_cards[idx];

    decode_art(c->art, reversed);
    lv_obj_invalidate(s_img);

    lv_label_set_text_fmt(s_name, "%s · %s", c->name, reversed ? "逆位" : "正位");
    lv_obj_set_style_text_color(s_name, lv_color_hex(reversed ? COLOR_REVERSED : COLOR_UPRIGHT), 0);
    lv_label_set_text(s_meaning, reversed ? c->reversed : c->upright);
    lv_obj_set_style_text_color(s_meaning, lv_color_hex(COLOR_TEXT), 0);

    reveal_anim();
    ESP_LOGI(TAG, "draw #%d %s (%s)", idx, c->name, reversed ? "reversed" : "upright");
}

/* ---------------- 界面 ---------------- */
static void build_ui(void)
{
    lv_obj_t *scr = lv_screen_active();
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(scr, lv_color_hex(COLOR_BG_TOP), 0);
    lv_obj_set_style_bg_grad_color(scr, lv_color_hex(COLOR_BG_BOT), 0);
    lv_obj_set_style_bg_grad_dir(scr, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    // 背景星点（装饰，位于卡牌下层）
    static const struct { uint8_t x, y, opa; uint32_t c; } stars[] = {
        { 12, 26, 60, 0xE7D8A8 }, { 30, 14, 40, 0xBFD8F0 }, { 44, 46, 80, 0xFFF3C4 },
        { 24, 70, 30, 0xE7D8A8 }, { 36, 96, 55, 0xBFD8F0 }, { 14, 138, 45, 0xE7D8A8 },
        { 34, 168, 70, 0xFFF3C4 }, { 20, 200, 35, 0xBFD8F0 }, { 30, 224, 50, 0xE7D8A8 },
        { 210, 22, 50, 0xBFD8F0 }, { 226, 48, 75, 0xFFF3C4 }, { 196, 74, 30, 0xE7D8A8 },
        { 208, 108, 55, 0xBFD8F0 }, { 228, 142, 40, 0xE7D8A8 }, { 200, 186, 65, 0xFFF3C4 },
        { 216, 214, 35, 0xBFD8F0 }, { 60, 226, 40, 0xE7D8A8 }, { 180, 232, 45, 0xBFD8F0 },
    };
    for (size_t i = 0; i < sizeof(stars) / sizeof(stars[0]); i++) {
        lv_obj_t *dot = lv_obj_create(scr);
        lv_obj_remove_style_all(dot);
        lv_obj_set_size(dot, 2, 2);
        lv_obj_set_pos(dot, stars[i].x, stars[i].y);
        lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(dot, lv_color_hex(stars[i].c), 0);
        lv_obj_set_style_bg_opa(dot, stars[i].opa, 0);
        lv_obj_remove_flag(dot, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    }

    // 卡牌外框：深紫渐变 + 金色描边 + 金色光晕
    s_card = lv_obj_create(scr);
    lv_obj_remove_style_all(s_card);
    lv_obj_set_size(s_card, CARD_W, CARD_H);
    lv_obj_set_pos(s_card, (LCD_H_RES - CARD_W) / 2, CARD_Y);
    lv_obj_set_style_radius(s_card, 8, 0);
    lv_obj_set_style_bg_color(s_card, lv_color_hex(0x1C1730), 0);
    lv_obj_set_style_bg_grad_color(s_card, lv_color_hex(0x0A0813), 0);
    lv_obj_set_style_bg_grad_dir(s_card, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_opa(s_card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_card, 2, 0);
    lv_obj_set_style_border_color(s_card, lv_color_hex(COLOR_GOLD), 0);
    lv_obj_set_style_border_opa(s_card, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(s_card, 4, 0);
    lv_obj_set_style_shadow_width(s_card, 18, 0);
    lv_obj_set_style_shadow_opa(s_card, LV_OPA_20, 0);
    lv_obj_set_style_shadow_color(s_card, lv_color_hex(COLOR_GOLD), 0);
    lv_obj_remove_flag(s_card, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

    // 牌面
    s_img = lv_image_create(s_card);
    lv_image_set_src(s_img, &s_img_dsc);
    lv_obj_center(s_img);

    // 牌名（正位金 / 逆位紫）
    s_name = lv_label_create(scr);
    lv_obj_set_style_text_font(s_name, &tarot_font_16, 0);
    lv_obj_set_style_text_color(s_name, lv_color_hex(COLOR_GOLD_LT), 0);
    lv_obj_set_style_text_letter_space(s_name, 1, 0);
    lv_obj_align(s_name, LV_ALIGN_TOP_MID, 0, 1);

    // 一句释义（两行居中）
    s_meaning = lv_label_create(scr);
    lv_obj_set_style_text_font(s_meaning, &tarot_font_16, 0);
    lv_obj_set_style_text_color(s_meaning, lv_color_hex(COLOR_TEXT), 0);
    lv_obj_set_style_text_align(s_meaning, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_line_space(s_meaning, 2, 0);
    lv_obj_set_width(s_meaning, 216);
    lv_label_set_long_mode(s_meaning, LV_LABEL_LONG_WRAP);
    lv_obj_align(s_meaning, LV_ALIGN_TOP_MID, 0, 196);
}

/* ---------------- BOOT 按键 ---------------- */
static void on_boot_press(void)
{
    if (xSemaphoreTake(s_lvgl_mutex, pdMS_TO_TICKS(200)) != pdTRUE) {
        return;
    }
    draw_card();
    xSemaphoreGive(s_lvgl_mutex);
}

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
    int auto_ms = 0;
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(20));
        if (gpio_get_level(BOOT_BTN_GPIO) == 0) {
            held_ms += 20;
        } else {
            if (held_ms >= BTN_SHORT_PRESS_MS) {
                on_boot_press();
            }
            held_ms = 0;
        }
        /* 每 5 秒自动触发一次抽牌 */
        auto_ms += 20;
        if (auto_ms >= AUTO_DRAW_MS) {
            auto_ms = 0;
            on_boot_press();
        }
    }
}

/* ---------------- 对外入口 ---------------- */
void tarot_ui_start(esp_lcd_panel_io_handle_t io, esp_lcd_panel_handle_t panel)
{
    s_io = io;
    s_panel = panel;

    s_flush_done = xSemaphoreCreateBinary();
    assert(s_flush_done);
    esp_lcd_panel_io_callbacks_t cbs = {
        .on_color_trans_done = on_color_done,
    };
    ESP_ERROR_CHECK(esp_lcd_panel_io_register_event_callbacks(s_io, &cbs, NULL));

    // 放大后的卡面缓冲：优先 PSRAM（N16R8 有 8MB），失败再退回内部 RAM
    s_img_buf = heap_caps_malloc(IMG_W * IMG_H * sizeof(uint16_t),
                                 MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_img_buf == NULL) {
        s_img_buf = heap_caps_malloc(IMG_W * IMG_H * sizeof(uint16_t),
                                     MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    assert(s_img_buf);

    memset(&s_img_dsc, 0, sizeof(s_img_dsc));
    s_img_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    s_img_dsc.header.cf = LV_COLOR_FORMAT_RGB565;
    s_img_dsc.header.w = IMG_W;
    s_img_dsc.header.h = IMG_H;
    s_img_dsc.header.stride = IMG_W * sizeof(uint16_t);
    s_img_dsc.data_size = IMG_W * IMG_H * sizeof(uint16_t);
    s_img_dsc.data = (const uint8_t *)s_img_buf;

    lv_init();
    lv_tick_set_cb(tick_cb);

    lv_display_t *disp = lv_display_create(LCD_H_RES, LCD_V_RES);
    lv_display_set_flush_cb(disp, flush_cb);
    lv_display_set_buffers(disp, s_buf1, s_buf2, sizeof(s_buf1), LV_DISPLAY_RENDER_MODE_PARTIAL);

    s_lvgl_mutex = xSemaphoreCreateMutex();
    assert(s_lvgl_mutex);

    build_ui();
    show_back();

    xTaskCreate(lvgl_task, "lvgl", 5120, NULL, 5, NULL);
    xTaskCreate(boot_btn_task, "boot_btn", 3072, NULL, 4, NULL);
    ESP_LOGI(TAG, "tarot UI started (%d cards)", TAROT_COUNT);
}
