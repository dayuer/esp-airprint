#pragma once
void lcd_ui_init(void);
void lcd_ui_wifi(const char *t);
void lcd_ui_prn(const char *t);
void lcd_ui_job(const char *t);
void lcd_ui_log(const char *line);
/* 心跳：只在 MQTT publish 真正成功时调，标题行会显示序号和距上次的秒数。
 * 超过 90 秒没有就显示「心跳停」并变红——云链路断了一眼可见。 */
void lcd_ui_beat(void);
