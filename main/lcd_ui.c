/* 板载 ST7789 状态屏：Wi-Fi / 打印机 / 作业 / 滚动日志（LVGL + 中文字库） */
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "bsp/esp-bsp.h"
#include "esp_lvgl_port.h"
#include "lvgl.h"
#include "freertos/queue.h"

LV_FONT_DECLARE(font_cn_16);

static lv_obj_t *s_lbl_title, *s_lbl_wifi, *s_lbl_prn, *s_lbl_job, *s_lbl_log;
static char s_log_lines[4][48];

/* 屏幕更新一律走队列，由专门的任务渲染。
 * 原因：LVGL 渲染中文很吃栈，之前直接在 ipp_conn 任务（4.6K 栈）里调，
 * 发完 UEL 那一刻栈溢出把整机干重启了——coredump 实锤。 */
typedef struct { uint8_t kind; char txt[44]; } ui_msg_t;
enum { UI_WIFI, UI_PRN, UI_JOB, UI_LOG };
static QueueHandle_t s_q;

static void set_text(lv_obj_t *lbl, const char *prefix, const char *txt);
static void do_log(const char *line);
static int  s_log_n;
static bool s_ready;

static void mk_label(lv_obj_t **lbl, int y, lv_color_t color, const lv_font_t *font)
{
    *lbl = lv_label_create(lv_scr_act());
    lv_obj_set_pos(*lbl, 6, y);
    lv_obj_set_width(*lbl, 228);
    lv_label_set_long_mode(*lbl, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_color(*lbl, color, 0);
    lv_obj_set_style_text_font(*lbl, font, 0);
}

static void ui_task(void *a)
{
    ui_msg_t m;
    while (1) {
        if (xQueueReceive(s_q, &m, portMAX_DELAY) != pdTRUE) continue;
        switch (m.kind) {
        case UI_WIFI: set_text(s_lbl_wifi, "网络: ",   m.txt); break;
        case UI_PRN:  set_text(s_lbl_prn,  "打印机: ", m.txt); break;
        case UI_JOB:  set_text(s_lbl_job,  "作业: ",   m.txt); break;
        case UI_LOG:  do_log(m.txt); break;
        }
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
    bsp_display_lock(0);
    lv_obj_set_style_bg_color(lv_scr_act(), lv_color_black(), 0);
    const lv_font_t *cjk = &font_cn_16;
    mk_label(&s_lbl_title, 4,   lv_color_hex(0x00d0ff), cjk);
    mk_label(&s_lbl_wifi,  30,  lv_color_hex(0xffffff), cjk);
    mk_label(&s_lbl_prn,   54,  lv_color_hex(0xffffff), cjk);
    mk_label(&s_lbl_job,   78,  lv_color_hex(0xa0ffa0), cjk);
    mk_label(&s_lbl_log,   108, lv_color_hex(0x808080), cjk);
    lv_obj_set_height(s_lbl_log, 128);
    lv_label_set_text(s_lbl_title, "AirPrint 打印桥");
    lv_label_set_text(s_lbl_wifi,  "网络: 启动中");
    lv_label_set_text(s_lbl_prn,   "打印机: 等待");
    lv_label_set_text(s_lbl_job,   "作业: 无");
    lv_label_set_text(s_lbl_log,   "");
    bsp_display_unlock();
    s_ready = true;
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

void lcd_ui_wifi(const char *t) { post(UI_WIFI, t); }
void lcd_ui_prn(const char *t)  { post(UI_PRN,  t); }
void lcd_ui_job(const char *t)  { post(UI_JOB,  t); }

void lcd_ui_log(const char *line) { post(UI_LOG, line); }

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
