#pragma once
#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* 打印机自报状态。code/display 来自 @PJL USTATUS DEVICE 的主动推送，
 * paper_out/error 来自 USB 打印机类标准请求 GET_PORT_STATUS（对所有
 * USB 打印机通用，不是厂商私货）。seq 每变一次 +1，供上报方检测变化。 */
typedef struct {
    bool     connected;
    bool     asleep;
    int      code;            /* 10001=Ready 10023=Printing 35078=Power Save */
    char     display[32];     /* 打印机面板原文 */
    bool     online;
    bool     paper_out;
    bool     error;
    uint32_t seq;
} usb_prn_status_t;
void      usb_printer_status(usb_prn_status_t *out);

void      usb_printer_start(void);              /* 起 host 栈 + 枚举任务 */
bool      usb_printer_connected(void);
esp_err_t usb_printer_job_begin(void);          /* 占用打印通道（互斥） */
esp_err_t usb_printer_job_write(const uint8_t *data, size_t len);
void      usb_printer_job_end(void);
void      usb_printer_abort(void);   /* 客户端取消时把 UEL 转给打印机 */            /* 补 ZLP + 释放通道 */
