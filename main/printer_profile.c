#include <stddef.h>
#include "esp_log.h"
#include "printer_profile.h"

static const char *TAG = "profile";

static const printer_profile_t PROFILES[] = {
    {
        .name           = "HP Laser MFP 13x",
        .vid = 0x03F0, .pid = 0xF22A,
        .make_and_model = "HP Laser MFP 136a",
        .device_id      = "MFG:HP;CMD:URF;MDL:HP Laser MFP 136a;CLS:PRINTER;",
        /* 黑白无双面。SRGB24 是彩机的、DM1 是双面的，都不能发 */
        .urf            = "CP1,IS1-4-5-19,MT1-2-3-4-5-6,RS600,V1.4,W8",
        .media_default  = "iso_a4_210x297mm",
        .media_x = 21000, .media_y = 29700, .margin = 423,
        .resolution = 300, .ppm = 20,
        .color = false, .duplex = false,

        .uel_job_end   = true,      /* 缺它就只能打第一份 */
        .uel_wake      = true,
        .wake_delay_ms = 1500,
        .iface_cycle   = true,
        .pjl_ustatus   = true,      /* 会推 Ready / Power Save / Printing */
        .soft_reset    = false,     /* 实测被 STALL */
    },
    {   /* 通用兜底：只用最保守、对所有打印机类设备都安全的动作 */
        .name           = "通用打印机",
        .vid = 0, .pid = 0,
        .make_and_model = "USB Printer (ESP32 bridge)",
        .device_id      = "MFG:Generic;CMD:URF;MDL:USB Printer;CLS:PRINTER;",
        .urf            = "CP1,IS1-4-5-19,MT1-2-3-4-5-6,RS600,V1.4,W8",
        .media_default  = "iso_a4_210x297mm",
        .media_x = 21000, .media_y = 29700, .margin = 423,
        .resolution = 300, .ppm = 10,
        .color = false, .duplex = false,

        .uel_job_end   = true,      /* UEL 是 HP/PCL 系通用作业分隔符，安全 */
        .uel_wake      = true,
        .wake_delay_ms = 1500,
        .iface_cycle   = true,
        .pjl_ustatus   = false,     /* 不确定对方懂不懂 PJL，不主动发 */
        .soft_reset    = false,
    },
};

const printer_profile_t *profile_lookup(uint16_t vid, uint16_t pid)
{
    size_t n = sizeof PROFILES / sizeof PROFILES[0];
    for (size_t i = 0; i < n; i++) {
        if (PROFILES[i].vid == vid && PROFILES[i].pid == pid) {
            ESP_LOGI(TAG, "匹配档案「%s」(%04X:%04X)", PROFILES[i].name, vid, pid);
            return &PROFILES[i];
        }
    }
    ESP_LOGW(TAG, "未知机型 %04X:%04X，使用通用档案", vid, pid);
    return &PROFILES[n - 1];
}
