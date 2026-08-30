#pragma once
#include "profile_script.h"
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

/* ── 机型识别 ──
 * device_id 是 USB 打印机类标准请求 GET_DEVICE_ID 读回的 IEEE-1284 串，
 * 枚举时自动取，形如 "MFG:HP;CMD:URF,PJL,...;MDL:...;CLS:PRINTER;"。
 * 其中 CMD: 决定哪些探针可以发——这是兼容性判断的地基。
 * 读不到（机型不支持）时返回空串。model 是从中抠出的 MDL 字段。 */
const char *usb_printer_device_id(void);
const char *usb_printer_model(void);
/* 当前生效的档案名，以及命中的 CUPS usb-quirks 位（见 usb_quirks_db.h）。
 * 两者一起上报给服务端，是判断「这台机器该配什么档案」的输入。 */
const char *usb_printer_profile_name(void);
uint8_t     usb_printer_quirks(void);

/* 第 0 层全量 dump：把这台打印机能无风险读到的一切拼成 JSON。
 * 只用控制传输和已缓存的描述符，**不碰打印通道，不可能出纸**，可随时调。
 * 含：设备与配置描述符全量、厂商串、产品串、**序列号串**、所有接口与端点、
 *     IEEE-1284 设备 ID、端口状态、本机选中的档案与 CUPS quirks。
 * 返回 ESP_ERR_INVALID_SIZE 表示 cap 不够、内容被截断（建议 ≥4KB）。 */
esp_err_t usb_printer_describe(char *out, size_t cap);

/* 当前插着的打印机序列号。没插时返回空串。
 * 这是换机场景的主键——设备 MAC 标识的是桥，不是打印机。
 * 心跳必须带它（API 文档 3.6 / 规则 10）。 */
const char *usb_printer_serial(void);

/* 服务端下发新档案后重算生效值（打印机已枚举时立即生效，不必等插拔）。 */
void usb_printer_reselect_profile(void);

/* 本次作业的一次性怪癖覆盖（API 文档 3.4）。传 -1 表示该项不覆盖。
 * **绝不写 NVS**；作业结束（成功或失败）自动销毁。 */
/* 装上生效档案（服务端下发的，或内置表合成的）。 */
void usb_printer_set_script(const prof_script_t *sc);
const prof_script_t *usb_printer_script(void);

/* 作业信令里的一次性钩子覆盖（接口文档 3.4 / 规则 9）。
 * 只对下一份作业生效，作业一结束就销毁，绝不写 NVS。
 * json 是整条作业信令，函数只看其中的 hooks 段。 */
bool usb_printer_job_hooks(const char *json, size_t len);

/* 精简机型身份 JSON，格式见 docs/API-cloud-print.md 3.8。
 * 走 MQTT retain=1，**上限 512 字节**；全量档案走 HTTPS，不走这里。 */
esp_err_t usb_printer_ident_json(char *out, size_t cap);

/* device ID 的 CMD: 是否授权发 PJL。第 1 层探针的开关，别绕过它。 */
bool usb_printer_pjl_allowed(void);

/* 按需 PJL 探针：发一条命令并收回包，例如 "@PJL INFO ID"。
 * 持作业锁执行，打印中会直接返回 ESP_ERR_TIMEOUT 而不是插队。
 * ⚠ 只在 device_id 的 CMD: 含 PJL/PCL 时才调用——对不懂 PJL 的机器盲发，
 *   命令会被当正文打出来，白费一张纸。 */
esp_err_t usb_printer_probe(const char *pjl_cmd, char *out, size_t cap, uint32_t wait_ms);
esp_err_t usb_printer_job_begin(void);          /* 占用打印通道（互斥） */
esp_err_t usb_printer_job_write(const uint8_t *data, size_t len);
void      usb_printer_job_end(void);   /* 补 UEL 作业结束符 + 释放通道 */
void      usb_printer_abort(void);     /* 作业中断时把 UEL 转给打印机 */
