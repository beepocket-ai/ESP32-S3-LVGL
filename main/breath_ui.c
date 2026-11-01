/*
 * 呼吸引导器（4-7-8 呼吸法）
 *
 * 由 ESP32-S3-LVGL.c 在 esp_lcd 面板初始化完成后调用 breath_ui_start()。
 * 自己实现 LVGL 显示端口（flush -> esp_lcd_panel_draw_bitmap），不依赖 esp_lvgl_port。
 *
 * 功能：
 *   - 待机：屏幕中央一枚暗色小圆环（像月亮），标题「呼吸引导」
 *   - BOOT 短按：开始一组 4-7-8 呼吸（吸 4s - 屏 7s - 呼 8s），默认 4 轮
 *       · 吸气：圆环平滑放大（青绿），中心倒计时
 *       · 屏息：圆环保持最大（淡紫）
 *       · 呼气：圆环平滑缩小（雾蓝）
 *   - 进行中再按 BOOT：随时结束，回到待机
 *   - 4 轮做完显示「完成」，3 秒后自动回到待机
 */

#include <stdio.h>
#include <assert.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "lvgl.h"

#include "breath_ui.h"
#include "ui_font.h"
#include "driver/gpio.h"

#define LCD_H_RES   240
#define LCD_V_RES   240
#define BUF_LINES   40                      // 局部渲染缓冲（40 行，双缓冲约 38KB）
#define LVGL_TICK_MS 10                     // LVGL 主循环周期（100Hz tick 下必须 >= 1 tick）

#define BOOT_BTN_GPIO       0               // 开发板 BOOT 键（GPIO0，按下为低）
#define BTN_SHORT_PRESS_MS  100             // 松开时按住时长 >= 100ms 才算一次按键

/* 4-7-8 呼吸法参数 */
#define INHALE_MS   4000                    // 吸气
#define HOLD_MS     7000                    // 屏息
#define EXHALE_MS   8000                    // 呼气
#define ROUNDS      4                       // 一组轮数（4-7-8 建议 4 轮起步）
#define DONE_MS     3000                    // 「完成」画面停留时间

#define RING_MIN    64                      // 圆环最小直径
#define RING_MAX    176                     // 圆环最大直径
#define GLOW_EXTRA  26                      // 光晕比圆环大出的直径

#define ANIM_TICK_MS 30                     // 动画帧周期（约 33fps）

/* 治愈系夜色配色 */
#define COLOR_BG        0x0E1420            // 深夜蓝
#define COLOR_TEXT      0xE8ECF3            // 主文字
#define COLOR_DIM       0x8A93A6            // 次要文字
#define COLOR_IDLE      0x3D5A80            // 待机圆环（暗蓝）
#define COLOR_INHALE    0x7BD8C0            // 吸气（青绿）
#define COLOR_HOLD      0xB39DDB            // 屏息（淡紫）
#define COLOR_EXHALE    0x7AA7D9            // 呼气（雾蓝）

static const char *TAG = "breath_ui";

static esp_lcd_panel_io_handle_t s_io;
static esp_lcd_panel_handle_t s_panel;
static SemaphoreHandle_t s_flush_done;
static SemaphoreHandle_t s_lvgl_mutex;

static uint16_t s_buf1[LCD_H_RES * BUF_LINES] __attribute__((aligned(4)));
static uint16_t s_buf2[LCD_H_RES * BUF_LINES] __attribute__((aligned(4)));

/* ---------------- 呼吸状态机 ---------------- */
typedef enum {
    ST_IDLE,                                // 待机封面
    ST_INHALE,                              // 吸气
    ST_HOLD,                                // 屏息
    ST_EXHALE,                              // 呼气
    ST_DONE,                                // 一组完成
} breath_state_t;

static breath_state_t s_state = ST_IDLE;
static uint32_t s_phase_start;              // 当前阶段起始 tick（lv_tick）
static uint32_t s_done_start;               // 「完成」画面起始 tick
static int s_round = 1;                     // 当前轮次 1..ROUNDS
static int s_last_count = -1;               // 已显示的倒计时数字（避免每帧刷新文字）

static lv_obj_t *s_glow;                    // 光晕（大、半透明）
static lv_obj_t *s_ring;                    // 呼吸圆环
static lv_obj_t *s_title;                   // 「呼吸引导」
static lv_obj_t *s_subtitle;                // 「4-7-8 呼吸法」
static lv_obj_t *s_phase_label;             // 吸气 / 屏息 / 呼气
static lv_obj_t *s_round_label;             // 第 x/4 轮
static lv_obj_t *s_count_label;             // 中央倒计时数字
static lv_obj_t *s_done_label;              // 完成
static lv_obj_t *s_hint;                    // 底部提示

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
        /* CONFIG_FREERTOS_HZ=100 时 pdMS_TO_TICKS(5) == 0，vTaskDelay(0) 不会阻塞，
         * 会把 IDLE0 饿死触发任务看门狗。这里强制至少 1 个 tick。 */
        TickType_t period = pdMS_TO_TICKS(LVGL_TICK_MS);
        vTaskDelay(period > 0 ? period : 1);
        if (xSemaphoreTake(s_lvgl_mutex, portMAX_DELAY) == pdTRUE) {
            lv_timer_handler();
            xSemaphoreGive(s_lvgl_mutex);
        }
    }
}

/* ---------------- 呼吸逻辑 ---------------- */
static uint32_t phase_ms(breath_state_t s)
{
    switch (s) {
    case ST_INHALE: return INHALE_MS;
    case ST_HOLD:   return HOLD_MS;
    case ST_EXHALE: return EXHALE_MS;
    default:        return 0;
    }
}

static uint32_t phase_color(breath_state_t s)
{
    switch (s) {
    case ST_INHALE: return COLOR_INHALE;
    case ST_HOLD:   return COLOR_HOLD;
    case ST_EXHALE: return COLOR_EXHALE;
    default:        return COLOR_IDLE;
    }
}

static const char *phase_text(breath_state_t s)
{
    switch (s) {
    case ST_INHALE: return "吸气";
    case ST_HOLD:   return "屏息";
    case ST_EXHALE: return "呼气";
    default:        return "";
    }
}

/* 调整圆环 + 光晕的大小和颜色（对齐方式保持居中，LVGL 会自动重排） */
static void set_ring(int d, uint32_t color)
{
    lv_color_t c = lv_color_hex(color);
    lv_obj_set_size(s_ring, d, d);
    lv_obj_set_style_border_color(s_ring, c, 0);
    lv_obj_set_style_bg_color(s_ring, c, 0);
    lv_obj_set_size(s_glow, d + GLOW_EXTRA, d + GLOW_EXTRA);
    lv_obj_set_style_bg_color(s_glow, c, 0);
}

static void set_phase(breath_state_t s)
{
    s_state = s;
    s_phase_start = lv_tick_get();
    s_last_count = -1;
    lv_label_set_text(s_phase_label, phase_text(s));
    lv_label_set_text_fmt(s_round_label, "第 %d/%d 轮", s_round, ROUNDS);
}

static void show_idle(void)
{
    s_state = ST_IDLE;
    lv_obj_clear_flag(s_title, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_subtitle, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_phase_label, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_round_label, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_count_label, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_done_label, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(s_hint, "按下 BOOT 开始");
    set_ring(RING_MIN, COLOR_IDLE);
}

static void start_session(void)
{
    s_round = 1;
    lv_obj_add_flag(s_title, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_subtitle, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_done_label, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_phase_label, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_round_label, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_count_label, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(s_hint, "再按 BOOT 结束");
    set_phase(ST_INHALE);
    ESP_LOGI(TAG, "breathing session started");
}

static void show_done(uint32_t now)
{
    s_state = ST_DONE;
    s_done_start = now;
    lv_obj_add_flag(s_phase_label, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_round_label, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_count_label, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_done_label, LV_OBJ_FLAG_HIDDEN);
    set_ring(RING_MIN, COLOR_IDLE);
    ESP_LOGI(TAG, "breathing session done");
}

/* 动画帧：按阶段进度更新圆环直径与倒计时（在 lvgl 任务上下文里运行） */
static void anim_cb(lv_timer_t *timer)
{
    (void)timer;
    uint32_t now = lv_tick_get();

    if (s_state == ST_IDLE) {
        return;
    }
    if (s_state == ST_DONE) {
        if (now - s_done_start >= DONE_MS) {
            show_idle();
        }
        return;
    }

    uint32_t ms = phase_ms(s_state);
    uint32_t el = now - s_phase_start;
    if (el >= ms) {                         // 阶段切换
        if (s_state == ST_INHALE) {
            set_phase(ST_HOLD);
        } else if (s_state == ST_HOLD) {
            set_phase(ST_EXHALE);
        } else {                            // 一轮呼气结束
            if (s_round >= ROUNDS) {
                show_done(now);
                return;
            }
            s_round++;
            set_phase(ST_INHALE);
        }
        el = 0;
        ms = phase_ms(s_state);
    }

    float p = (float)el / (float)ms;        // 0..1
    int d;
    if (s_state == ST_INHALE) {
        float e = 1.0f - (1.0f - p) * (1.0f - p) * (1.0f - p);   // ease-out cubic：先快后慢
        d = RING_MIN + (int)((RING_MAX - RING_MIN) * e);
    } else if (s_state == ST_HOLD) {
        d = RING_MAX;                       // 屏息保持最大
    } else {
        float e = p * p * (3.0f - 2.0f * p);                     // smoothstep：缓慢收尾
        d = RING_MAX - (int)((RING_MAX - RING_MIN) * e);
    }
    set_ring(d, phase_color(s_state));

    int count = (int)((ms - el + 999) / 1000);   // 阶段剩余秒数（向上取整）
    if (count != s_last_count) {
        s_last_count = count;
        lv_label_set_text_fmt(s_count_label, "%d", count);
    }
}

/* ---------------- 界面 ---------------- */
static lv_obj_t *label_create(lv_obj_t *parent, const lv_font_t *font, uint32_t color,
                              lv_align_t align, int y)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    lv_obj_align(l, align, 0, y);
    return l;
}

static void build_ui(void)
{
    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_hex(COLOR_BG), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);

    // 光晕（在圆环下层，先创建）
    s_glow = lv_obj_create(scr);
    lv_obj_remove_style_all(s_glow);
    lv_obj_set_style_radius(s_glow, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(s_glow, LV_OPA_20, 0);
    lv_obj_align(s_glow, LV_ALIGN_CENTER, 0, 10);
    lv_obj_clear_flag(s_glow, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    // 呼吸圆环（描边 + 40% 填充）
    s_ring = lv_obj_create(scr);
    lv_obj_remove_style_all(s_ring);
    lv_obj_set_style_radius(s_ring, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(s_ring, 3, 0);
    lv_obj_set_style_border_opa(s_ring, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_opa(s_ring, LV_OPA_40, 0);
    lv_obj_align(s_ring, LV_ALIGN_CENTER, 0, 10);
    lv_obj_clear_flag(s_ring, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    // 待机封面
    s_title = label_create(scr, &ui_font_28, COLOR_TEXT, LV_ALIGN_TOP_MID, 30);
    lv_label_set_text(s_title, "呼吸引导");
    s_subtitle = label_create(scr, &ui_font_16, COLOR_DIM, LV_ALIGN_TOP_MID, 68);
    lv_label_set_text(s_subtitle, "4-7-8 呼吸法");

    // 进行中的信息
    s_phase_label = label_create(scr, &ui_font_16, COLOR_TEXT, LV_ALIGN_TOP_MID, 26);
    s_round_label = label_create(scr, &ui_font_16, COLOR_DIM, LV_ALIGN_TOP_MID, 50);
    s_count_label = label_create(scr, &ui_font_28, COLOR_TEXT, LV_ALIGN_CENTER, 10);
    lv_obj_set_style_text_align(s_count_label, LV_TEXT_ALIGN_CENTER, 0);

    // 完成
    s_done_label = label_create(scr, &ui_font_28, COLOR_INHALE, LV_ALIGN_CENTER, 10);
    lv_label_set_text(s_done_label, "完成");

    // 底部提示
    s_hint = label_create(scr, &ui_font_16, COLOR_DIM, LV_ALIGN_BOTTOM_MID, -16);

    show_idle();
}

/* ---------------- BOOT 按键 ---------------- */
static void on_boot_press(void)
{
    if (xSemaphoreTake(s_lvgl_mutex, pdMS_TO_TICKS(200)) != pdTRUE) {
        return;
    }
    if (s_state == ST_IDLE) {
        start_session();
    } else {
        show_idle();                        // 进行中 / 完成画面再按 = 直接结束
    }
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
    }
}

/* ---------------- 对外入口 ---------------- */
void breath_ui_start(esp_lcd_panel_io_handle_t io, esp_lcd_panel_handle_t panel)
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

    build_ui();
    lv_timer_create(anim_cb, ANIM_TICK_MS, NULL);

    xTaskCreate(lvgl_task, "lvgl", 5120, NULL, 5, NULL);
    xTaskCreate(boot_btn_task, "boot_btn", 3072, NULL, 4, NULL);
    ESP_LOGI(TAG, "breathing guide UI started");
}
