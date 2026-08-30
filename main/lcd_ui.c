/* 板载 ST7789 状态屏：Wi-Fi / 打印机 / 作业 / 滚动日志（LVGL + 中文字库）
 *
 * ── 防残影三件套 ──
 * ST7789 是 TFT LCD 不是 OLED，它的「烧屏」是残影/离子迁移，比 OLED 轻且多半
 * 可恢复；这块屏真正的老化大头是**背光常亮**。所以三条措施的实际权重是：
 *
 *   ① 空闲降亮度  —— 对 LCD 寿命影响最大的一条，背光不常年满功率
 *   ② 整屏像素偏移 —— 每隔几分钟把内容整体挪几个像素，同一像素不会长期显示同一内容
 *   ③ 横向跑马灯  —— 长文本循环滚动；顺带解决了以前 CLIP 模式把 IP、机型名截断的问题
 *
 * 三条都只改显示，不影响任何业务逻辑。
 */
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "bsp/esp-bsp.h"
#include "esp_lvgl_port.h"
#include "esp_timer.h"
#include "lvgl.h"
#include "freertos/queue.h"

LV_FONT_DECLARE(font_cn_16);

/* ── 防残影参数，想调就调这里 ── */
#define SHIFT_PERIOD_S   120     /* 多久挪一次位置 */
#define SHIFT_STEP_PX      3     /* 每次挪几个像素 */
#define SHIFT_STATES       8     /* 偏移循环几个位置（走一个方框） */
#define IDLE_DIM_S       300     /* 多久没有新状态就降亮度 */
#define BRIGHT_ACTIVE    100     /* 活动时亮度 % */
#define BRIGHT_IDLE       12     /* 空闲时亮度 %。设 0 就是全黑息屏 */

static lv_obj_t *s_root;         /* 所有内容的容器，像素偏移就是挪它 */
static lv_obj_t *s_lbl_title, *s_lbl_wifi, *s_lbl_prn, *s_lbl_job, *s_lbl_log;
static char s_log_lines[4][48];

/* 屏幕更新一律走队列，由专门的任务渲染。
 * 原因：LVGL 渲染中文很吃栈，之前直接在网络任务（当时是 ipp_conn，4.6K 栈）里调，
 * 发完 UEL 那一刻栈溢出把整机干重启了——coredump 实锤。 */
typedef struct { uint8_t kind; char txt[44]; } ui_msg_t;
enum { UI_WIFI, UI_PRN, UI_JOB, UI_LOG, UI_BEAT };
static QueueHandle_t s_q;

static void set_text(lv_obj_t *lbl, const char *prefix, const char *txt);
static void do_log(const char *line);
static int  s_log_n;
static bool s_ready;
static int64_t s_last_activity_us;
static bool s_dimmed;

/* ── 心跳 ──
 * 只有真正 publish 成功才记一拍（见 cloud_client.c 的 report()）。
 * 所以标题行的秒数一旦不再归零，就说明云链路断了——不用连电脑就看得出来。
 * 顺带：这行每秒都在变，标题不再是长期静止的内容，防残影也受益。 */
static uint32_t s_beat_n;
static int64_t  s_beat_us;
#define BEAT_STALE_S  90        /* 多久没心跳算「停」 */

static void mk_label(lv_obj_t **lbl, int y, lv_color_t color, const lv_font_t *font)
{
    *lbl = lv_label_create(s_root);
    lv_obj_set_pos(*lbl, 6, y);
    lv_obj_set_width(*lbl, 222);      /* 留出 SHIFT 的横向余量，见 SHIFT_STEP_PX */
    /* 循环跑马灯：文本超宽就一直滚。短文本 LVGL 不会滚，那部分靠像素偏移兜底。
     * 以前是 CLIP，长的 IP 和机型名会被直接截掉看不见。 */
    lv_label_set_long_mode(*lbl, LV_LABEL_LONG_MODE_SCROLL_CIRCULAR);
    lv_obj_set_style_text_color(*lbl, color, 0);
    lv_obj_set_style_text_font(*lbl, font, 0);
    lv_obj_set_style_anim_duration(*lbl, 8000, 0);   /* 滚一圈的时长，别太快晃眼 */
}

/* 整屏像素偏移：沿一个小方框循环走位，任何一个像素都不会长期显示同一内容。
 * 走方框而不是随机，是为了让相邻两次的位移可预期，视觉上不跳。 */
static void apply_shift(void)
{
    static int state;
    static const int8_t DX[SHIFT_STATES] = { 0, 1, 2, 2, 2, 1, 0, 0 };
    static const int8_t DY[SHIFT_STATES] = { 0, 0, 0, 1, 2, 2, 2, 1 };
    state = (state + 1) % SHIFT_STATES;
    bsp_display_lock(0);
    lv_obj_set_pos(s_root, DX[state] * SHIFT_STEP_PX, DY[state] * SHIFT_STEP_PX);
    bsp_display_unlock();
}

/* 标题行：云打印桥 · 心跳序号 · 距上次心跳多少秒 */
static void refresh_title(void)
{
    char buf[48];
    if (!s_beat_us) {
        snprintf(buf, sizeof buf, "云打印桥  心跳 --");
    } else {
        int age = (int)((esp_timer_get_time() - s_beat_us) / 1000000);
        if (age > BEAT_STALE_S)
            snprintf(buf, sizeof buf, "云打印桥  心跳停 %ds", age);
        else
            snprintf(buf, sizeof buf, "云打印桥  心跳 %lu  %ds",
                     (unsigned long)s_beat_n, age);
    }
    bsp_display_lock(0);
    lv_label_set_text(s_lbl_title, buf);
    lv_obj_set_style_text_color(s_lbl_title,
        (s_beat_us && (esp_timer_get_time() - s_beat_us) / 1000000 > BEAT_STALE_S)
            ? lv_color_hex(0xff6060)          /* 心跳停了变红 */
            : lv_color_hex(0x00d0ff), 0);
    bsp_display_unlock();
}

static void set_brightness(bool active)
{
    if (s_dimmed == !active) return;          /* 状态没变就别折腾背光 */
    s_dimmed = !active;
    bsp_display_brightness_set(active ? BRIGHT_ACTIVE : BRIGHT_IDLE);
}

static void ui_task(void *a)
{
    ui_msg_t m;
    int64_t last_shift_us = esp_timer_get_time();
    while (1) {
        /* 1 秒超时兼做心跳：没消息时跑防残影的定时逻辑 */
        if (xQueueReceive(s_q, &m, pdMS_TO_TICKS(1000)) == pdTRUE) {
            /* 心跳每 30 秒一次，不能算「活动」——否则屏幕永远进不了低亮度 */
            if (m.kind != UI_BEAT) {
                s_last_activity_us = esp_timer_get_time();
                set_brightness(true);         /* 有新状态就点亮 */
            }
            switch (m.kind) {
            case UI_WIFI: set_text(s_lbl_wifi, "云端: ",   m.txt); break;
            case UI_PRN:  set_text(s_lbl_prn,  "打印机: ", m.txt); break;
            case UI_JOB:  set_text(s_lbl_job,  "作业: ",   m.txt); break;
            case UI_LOG:  do_log(m.txt); break;
            case UI_BEAT: s_beat_n++; s_beat_us = esp_timer_get_time();
                          refresh_title(); break;
            }
            continue;
        }

        refresh_title();          /* 每秒走一下秒数，心跳停了会自己变红 */

        int64_t now = esp_timer_get_time();
        if (now - last_shift_us >= (int64_t)SHIFT_PERIOD_S * 1000000) {
            last_shift_us = now;
            apply_shift();
        }
        if (now - s_last_activity_us >= (int64_t)IDLE_DIM_S * 1000000)
            set_brightness(false);
    }
}

void lcd_ui_init(void)
{
    const bsp_display_cfg_t dcfg = {
        .lvgl_port_cfg = ESP_LVGL_PORT_INIT_CONFIG(),
        .buffer_size   = BSP_LCD_H_RES * 16,   /* 双缓冲 240x30 太奢侈，状态屏够用即可 */
        .double_buffer = false,
        .flags = { .buff_dma = true, .buff_spiram = false },
    };
    bsp_display_start_with_config(&dcfg);
    bsp_display_backlight_on();
    bsp_display_brightness_set(BRIGHT_ACTIVE);
    bsp_display_lock(0);
    lv_obj_set_style_bg_color(lv_scr_act(), lv_color_black(), 0);

    /* 所有内容挂在这个透明容器里，偏移时整体挪它一个对象就够了。
     * 版面比屏幕矮一截（最低到 y=208），空出来的就是偏移余量。 */
    s_root = lv_obj_create(lv_scr_act());
    lv_obj_remove_style_all(s_root);
    lv_obj_set_pos(s_root, 0, 0);
    lv_obj_set_size(s_root, BSP_LCD_H_RES, BSP_LCD_V_RES);
    lv_obj_set_style_bg_opa(s_root, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(s_root, LV_OBJ_FLAG_SCROLLABLE);

    const lv_font_t *cjk = &font_cn_16;
    mk_label(&s_lbl_title, 2,  lv_color_hex(0x00d0ff), cjk);
    mk_label(&s_lbl_wifi,  26, lv_color_hex(0xffffff), cjk);
    mk_label(&s_lbl_prn,   48, lv_color_hex(0xffffff), cjk);
    mk_label(&s_lbl_job,   70, lv_color_hex(0xa0ffa0), cjk);
    mk_label(&s_lbl_log,   96, lv_color_hex(0x808080), cjk);
    /* 日志区是多行，用 WRAP 不用跑马灯——横向滚动会让多行日志没法读 */
    lv_label_set_long_mode(s_lbl_log, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_height(s_lbl_log, 112);
    lv_label_set_text(s_lbl_title, "云打印桥  心跳 --");
    lv_label_set_text(s_lbl_wifi,  "云端: 连接中");
    lv_label_set_text(s_lbl_prn,   "打印机: 等待");
    lv_label_set_text(s_lbl_job,   "作业: 无");
    lv_label_set_text(s_lbl_log,   "");
    bsp_display_unlock();
    s_ready = true;
    s_last_activity_us = esp_timer_get_time();
    s_q = xQueueCreate(8, sizeof(ui_msg_t));
    xTaskCreate(ui_task, "lcd_ui", 6144, NULL, 2, NULL);   /* 渲染独占大栈 */
}

static void set_text(lv_obj_t *lbl, const char *prefix, const char *txt)
{
    if (!s_ready) return;
    char buf[64];
    snprintf(buf, sizeof buf, "%s%s", prefix, txt);
    bsp_display_lock(0);
    lv_label_set_text(lbl, buf);
    bsp_display_unlock();
}

static void post(uint8_t kind, const char *t)
{
    if (!s_q) return;
    ui_msg_t m = { .kind = kind };
    strlcpy(m.txt, t, sizeof m.txt);
    xQueueSend(s_q, &m, 0);          /* 满了就丢，绝不阻塞调用方 */
}

void lcd_ui_wifi(const char *t) { post(UI_WIFI, t); }   /* 现在显示云端连接状态 */
void lcd_ui_prn(const char *t)  { post(UI_PRN,  t); }
void lcd_ui_job(const char *t)  { post(UI_JOB,  t); }

void lcd_ui_log(const char *line) { post(UI_LOG, line); }
void lcd_ui_beat(void)            { post(UI_BEAT, ""); }

static void do_log(const char *line)
{
    if (!s_ready) return;
    if (s_log_n == 4) {
        memmove(s_log_lines[0], s_log_lines[1], sizeof s_log_lines[0] * 3);
        s_log_n = 3;
    }
    strlcpy(s_log_lines[s_log_n++], line, sizeof s_log_lines[0]);
    char all[220] = "";
    for (int i = 0; i < s_log_n; i++) {
        strlcat(all, s_log_lines[i], sizeof all);
        strlcat(all, "\n", sizeof all);
    }
    bsp_display_lock(0);
    lv_label_set_text(s_lbl_log, all);
    bsp_display_unlock();
}
