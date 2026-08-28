#pragma once
#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>

void      usb_printer_start(void);              /* 起 host 栈 + 枚举任务 */
bool      usb_printer_connected(void);
esp_err_t usb_printer_job_begin(void);          /* 占用打印通道（互斥） */
esp_err_t usb_printer_job_write(const uint8_t *data, size_t len);
void      usb_printer_job_end(void);            /* 补 ZLP + 释放通道 */
