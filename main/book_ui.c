/*
 * 答案之书 UI
 *
 * 由 ESP32-S3-LVGL.c 在 esp_lcd 面板初始化完成后调用 book_ui_start()。
 * 自己实现 LVGL 显示端口（flush -> esp_lcd_panel_draw_bitmap），不依赖 esp_lvgl_port。
 *
 * 功能：
 *   - 默认显示「答案之书」封面
 *   - BOOT 短按：随机翻开一条答案，10 秒后自动回到封面
 *   - 答案显示期间再按 BOOT：换一条答案并重新计时
 *   - 答案语料内嵌自 main/answers.json（UTF-8，137 条）
 */

#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_timer.h"
#include "esp_random.h"
#include "esp_log.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "lvgl.h"

#include "book_ui.h"
#include "book_font.h"
#include "driver/gpio.h"

#define LCD_H_RES   240
#define LCD_V_RES   240
#define BUF_LINES   40                      // 局部渲染缓冲（40 行，双缓冲约 38KB）
#define LVGL_TICK_MS 10                     // LVGL 主循环周期（100Hz tick 下必须 >= 1 tick）

#define BOOT_BTN_GPIO       0               // 开发板 BOOT 键（GPIO0，按下为低）
#define BTN_SHORT_PRESS_MS  100             // 松开时按住时长 >= 100ms 才算一次按键
#define ANSWER_HIDE_MS      10000           // 答案显示 10 秒后回到封面
#define ANSWER_MAX          512             // 语料条数上限

#define COLOR_BG        0x14100D             // 深棕黑背景
#define COLOR_PAPER     0x1C1712             // 封面纸色
#define COLOR_GOLD      0xD9B36C             // 金色标题 / 描边
#define COLOR_TEXT      0xF2E8D5             // 答案正文
#define COLOR_HINT      0x8C8378             // 提示文字

static const char *TAG = "book_ui";

static esp_lcd_panel_io_handle_t s_io;
static esp_lcd_panel_handle_t s_panel;
static SemaphoreHandle_t s_flush_done;

static uint16_t s_buf1[LCD_H_RES * BUF_LINES] __attribute__((aligned(4)));
static uint16_t s_buf2[LCD_H_RES * BUF_LINES] __attribute__((aligned(4)));

static lv_obj_t *s_cover_layer;
static lv_obj_t *s_answer_layer;
static lv_obj_t *s_answer_label;
static lv_obj_t *s_page_label;
static lv_timer_t *s_hide_timer;
static SemaphoreHandle_t s_lvgl_mutex;

// 答案语料（由 CMake 的 target_add_binary_data 嵌入 answers.json）
extern const uint8_t answers_data_start[] asm("_binary_answers_data_start");
extern const uint32_t answers_data_length asm("answers_data_length");

static char *s_answer_buf;                  // 语料 RSS 内存副本（改写为 C 字符串）
static const char *s_answers[ANSWER_MAX];
static int s_answer_count;
static int s_last_index = -1;

/* ---------------- 答案语料 ---------------- */

static void load_answers(void)
{
    uint32_t len = answers_data_length;
    s_answer_buf = malloc(len + 1);
    assert(s_answer_buf);
    memcpy(s_answer_buf, answers_data_start, len);
    s_answer_buf[len] = '\0';

    // answers.json 是字符串数组，没有转义引号：直接按 "..." 切分
    int count = 0;
    char *p = s_answer_buf;
    while (count < ANSWER_MAX) {
        char *start = strchr(p, '"');
        if (start == NULL) {
            break;
        }
        start++;
        char *end = strchr(start, '"');
        if (end == NULL) {
            break;
        }
        *end = '\0';
        s_answers[count++] = start;
        p = end + 1;
    }
    s_answer_count = count;
    ESP_LOGI(TAG, "loaded %d answers (%u bytes json)", count, (unsigned)len);
}

static int pick_index(void)
{
    if (s_answer_count <= 0) {
        return -1;
    }
    if (s_answer_count == 1) {
        return 0;
    }
    // 避免连续两次抽到同一条
    for (int i = 0; i < 10; i++) {
        int idx = (int)(esp_random() % (uint32_t)s_answer_count);
        if (idx != s_last_index) {
            s_last_index = idx;
            return idx;
        }
    }
    s_last_index = (s_last_index + 1) % s_answer_count;
    return s_last_index;
}

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
        /* CONFIG_FREERTOS_HZ=100 时 pdMS_TO_TICKS(5) == 0，vTaskDelay(0) 只是让出
         * 同优先级任务并立刻被重新调度，优先级 5 的 lvgl 任务会一直占住 CPU0，
         * IDLE0 拿不到时间片 → task_wdt 报「IDLE0 未及时喂狗」。这里强制至少 1 tick。 */
        TickType_t period = pdMS_TO_TICKS(LVGL_TICK_MS);
        vTaskDelay(period > 0 ? period : 1);
        if (xSemaphoreTake(s_lvgl_mutex, portMAX_DELAY) == pdTRUE) {
            lv_timer_handler();
            xSemaphoreGive(s_lvgl_mutex);
        }
    }
}

/* ---------------- 界面 ---------------- */
static lv_obj_t *layer_create(lv_obj_t *parent)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_size(o, LCD_H_RES, LCD_V_RES);
    lv_obj_set_pos(o, 0, 0);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    return o;
}

static lv_obj_t *hline_create(lv_obj_t *parent, int w, int y, uint32_t color, lv_opa_t opa)
{
    lv_obj_t *line = lv_obj_create(parent);
    lv_obj_remove_style_all(line);
    lv_obj_set_size(line, w, 1);
    lv_obj_align(line, LV_ALIGN_TOP_MID, 0, y);
    lv_obj_set_style_bg_color(line, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(line, opa, 0);
    lv_obj_clear_flag(line, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    return line;
}

static void build_cover(void)
{
    s_cover_layer = layer_create(lv_screen_active());

    // 书本封皮
    lv_obj_t *book = lv_obj_create(s_cover_layer);
    lv_obj_remove_style_all(book);
    lv_obj_set_size(book, 196, 196);
    lv_obj_align(book, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(book, lv_color_hex(COLOR_PAPER), 0);
    lv_obj_set_style_bg_opa(book, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(book, 10, 0);
    lv_obj_set_style_border_width(book, 2, 0);
    lv_obj_set_style_border_color(book, lv_color_hex(COLOR_GOLD), 0);
    lv_obj_clear_flag(book, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    // 书名
    lv_obj_t *title = lv_label_create(book);
    lv_obj_set_style_text_font(title, &book_font_28, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(COLOR_GOLD), 0);
    lv_label_set_text(title, "答案之书");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 34);

    hline_create(book, 96, 82, COLOR_GOLD, LV_OPA_50);

    // 说明
    lv_obj_t *hint = lv_label_create(book);
    lv_obj_set_style_text_font(hint, &book_font_16, 0);
    lv_obj_set_style_text_color(hint, lv_color_hex(COLOR_HINT), 0);
    lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_line_space(hint, 4, 0);
    lv_label_set_text(hint, "心想一个问题\n按下 BOOT 翻开");
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -26);
}

static void build_answer(void)
{
    s_answer_layer = layer_create(lv_screen_active());

    lv_obj_t *head = lv_label_create(s_answer_layer);
    lv_obj_set_style_text_font(head, &book_font_16, 0);
    lv_obj_set_style_text_color(head, lv_color_hex(COLOR_GOLD), 0);
    lv_label_set_text(head, "答案");
    lv_obj_align(head, LV_ALIGN_TOP_MID, 0, 18);

    s_page_label = lv_label_create(s_answer_layer);
    lv_obj_set_style_text_font(s_page_label, &book_font_16, 0);
    lv_obj_set_style_text_color(s_page_label, lv_color_hex(COLOR_HINT), 0);
    lv_label_set_text(s_page_label, "P.000");
    lv_obj_align(s_page_label, LV_ALIGN_TOP_RIGHT, -14, 18);

    hline_create(s_answer_layer, 100, 44, COLOR_GOLD, LV_OPA_40);

    s_answer_label = lv_label_create(s_answer_layer);
    lv_obj_set_style_text_font(s_answer_label, &book_font_16, 0);
    lv_obj_set_style_text_color(s_answer_label, lv_color_hex(COLOR_TEXT), 0);
    lv_obj_set_style_text_align(s_answer_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_line_space(s_answer_label, 6, 0);
    lv_obj_set_width(s_answer_label, 216);
    lv_label_set_long_mode(s_answer_label, LV_LABEL_LONG_WRAP);
    lv_label_set_text(s_answer_label, "");
    lv_obj_align(s_answer_label, LV_ALIGN_TOP_MID, 0, 56);

    lv_obj_t *foot = lv_label_create(s_answer_layer);
    lv_obj_set_style_text_font(foot, &book_font_16, 0);
    lv_obj_set_style_text_color(foot, lv_color_hex(COLOR_HINT), 0);
    lv_label_set_text(foot, "再按 BOOT 换一条");
    lv_obj_align(foot, LV_ALIGN_BOTTOM_MID, 0, -16);

    lv_obj_add_flag(s_answer_layer, LV_OBJ_FLAG_HIDDEN);
}

static void build_ui(void)
{
    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_hex(COLOR_BG), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);

    build_cover();
    build_answer();
}

/* ---------------- 翻页逻辑 ---------------- */
static void hide_timer_cb(lv_timer_t *timer)
{
    lv_obj_add_flag(s_answer_layer, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_cover_layer, LV_OBJ_FLAG_HIDDEN);
    lv_timer_pause(timer);
    ESP_LOGI(TAG, "answer hidden, back to cover");
}

static void show_answer(void)
{
    if (xSemaphoreTake(s_lvgl_mutex, pdMS_TO_TICKS(200)) != pdTRUE) {
        return;
    }
    int idx = pick_index();
    if (idx >= 0) {
        lv_label_set_text(s_answer_label, s_answers[idx]);
        lv_label_set_text_fmt(s_page_label, "P.%03d", idx + 1);
        ESP_LOGI(TAG, "answer #%d: %s", idx, s_answers[idx]);
    }
    lv_obj_add_flag(s_cover_layer, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_answer_layer, LV_OBJ_FLAG_HIDDEN);
    lv_timer_reset(s_hide_timer);
    lv_timer_resume(s_hide_timer);
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
        } else {
            if (held_ms >= BTN_SHORT_PRESS_MS) {
                show_answer();
            }
            held_ms = 0;
        }
    }
}

/* ---------------- 对外入口 ---------------- */
void book_ui_start(esp_lcd_panel_io_handle_t io, esp_lcd_panel_handle_t panel)
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

    load_answers();
    build_ui();

    s_hide_timer = lv_timer_create(hide_timer_cb, ANSWER_HIDE_MS, NULL);
    lv_timer_pause(s_hide_timer);

    xTaskCreate(lvgl_task, "lvgl", 5120, NULL, 5, NULL);
    xTaskCreate(boot_btn_task, "boot_btn", 3072, NULL, 4, NULL);
    ESP_LOGI(TAG, "answer book UI started");
}
