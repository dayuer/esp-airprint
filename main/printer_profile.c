#include <stddef.h>
#include <string.h>
#include "esp_log.h"
#include "printer_profile.h"
#include "usb_quirks_db.h"
#include <string.h>
#include "nvs_flash.h"
#include "nvs.h"

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

/* 查 CUPS usb-quirks 表。精确 VID/PID 优先，其次厂商通配条目。 */
static uint8_t cups_quirks_lookup(uint16_t vid, uint16_t pid)
{
    uint8_t flags = 0;
    for (size_t i = 0; i < CUPS_USB_QUIRKS_N; i++) {
        const usb_quirk_t *q = &CUPS_USB_QUIRKS[i];
        if (q->vid != vid) continue;
        if (q->pid == pid || q->pid == QUIRK_PID_ANY) flags |= q->flags;
    }
    return flags;
}

/* 返回的是这份可写副本，不是模板本身——因为要在上面叠加 quirks。
 * 调用方拿到的指针在下一次 profile_lookup 之前一直有效。 */
static printer_profile_t s_active;

/* ── 服务端下发的档案：只管存取原始 JSON ── */
#define NVS_CLOUD "cloud"
#define NVS_KEY   "profile"

void profile_raw_save(const char *json, size_t len)
{
    if (!json || len == 0 || len > PROF_MAX_JSON) return;
    nvs_handle_t h;
    if (nvs_open(NVS_CLOUD, NVS_READWRITE, &h) != ESP_OK) return;
    /* 存成带结尾 0 的字符串，读回来可以直接当 C 串用 */
    if (nvs_set_blob(h, NVS_KEY, json, len + 1) == ESP_OK) nvs_commit(h);
    nvs_close(h);
}

size_t profile_raw_load(char *out, size_t cap)
{
    nvs_handle_t h;
    if (nvs_open(NVS_CLOUD, NVS_READONLY, &h) != ESP_OK) return 0;
    size_t n = cap;
    esp_err_t e = nvs_get_blob(h, NVS_KEY, out, &n);
    nvs_close(h);
    if (e != ESP_OK || n == 0) return 0;
    out[n - 1] = 0;                 /* 存的时候带了结尾 0，这里兜一下底 */
    return n - 1;
}

void profile_raw_clear(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_CLOUD, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_erase_key(h, NVS_KEY);
    nvs_commit(h);
    nvs_close(h);
}

/* UEL(Universal Exit Language)，HP/三星系的作业分隔符。
 * 服务端下发的档案里它是一串十六进制；这里是内置兜底用的同一串字节。 */
static const uint8_t UEL_BYTES[] = { 0x1b, '%', '-', '1', '2', '3', '4', '5', 'X' };

static void push_send(prof_hook_t *h, const uint8_t *d, size_t n)
{
    if (h->n >= PROF_MAX_STEPS || n > PROF_MAX_SEND) return;
    prof_step_t *st = &h->step[h->n++];
    st->op = PROF_OP_SEND_HEX;
    memcpy(st->data, d, n);
    st->len = (uint8_t)n;
}

static void push_simple(prof_hook_t *h, prof_op_t op, uint16_t ms)
{
    if (h->n >= PROF_MAX_STEPS) return;
    prof_step_t *st = &h->step[h->n++];
    st->op = op;
    st->ms = ms;
}

void profile_script_from_builtin(const printer_profile_t *p, prof_script_t *out)
{
    memset(out, 0, sizeof *out);
    out->valid  = true;
    out->rev    = 0;                      /* 0 = 不是服务端下发的 */
    out->unidir = p && p->unidir;
    out->pjl_ok = !(p && p->unidir);
    snprintf(out->src, sizeof out->src, "builtin");

    /* 没有档案时按最保守的来：发 UEL。方向永远是「宁可多发 9 个字节，
     * 也不要打不出来」——不发它只能打第一份，第二份要人去按取消键。 */
    if (!p || p->uel_job_end)
        push_send(&out->hook[PROF_HOOK_JOB_END], UEL_BYTES, sizeof UEL_BYTES);

    if (p && p->iface_cycle)
        push_simple(&out->hook[PROF_HOOK_JOB_BEGIN], PROF_OP_IFACE_RESET, 0);

    if (p && p->uel_wake) {
        push_send(&out->hook[PROF_HOOK_WAKE], UEL_BYTES, sizeof UEL_BYTES);
        if (p->wake_delay_ms) {
            uint32_t ms = p->wake_delay_ms;
            if (ms > PROF_MAX_DELAY_MS) ms = PROF_MAX_DELAY_MS;
            push_simple(&out->hook[PROF_HOOK_WAKE], PROF_OP_DELAY_MS, (uint16_t)ms);
        }
    }
}

const printer_profile_t *profile_lookup(uint16_t vid, uint16_t pid)
{
    size_t n = sizeof PROFILES / sizeof PROFILES[0];
    const printer_profile_t *tpl = &PROFILES[n - 1];   /* 默认通用兜底 */
    bool exact = false;

    for (size_t i = 0; i < n; i++) {
        if (PROFILES[i].vid == vid && PROFILES[i].pid == pid) {
            tpl = &PROFILES[i]; exact = true; break;
        }
    }
    memcpy(&s_active, tpl, sizeof s_active);

    uint8_t q = cups_quirks_lookup(vid, pid);
    s_active.cups_quirks = q;

    if (exact) {
        ESP_LOGI(TAG, "匹配档案「%s」(%04X:%04X)", tpl->name, vid, pid);
    } else {
        ESP_LOGW(TAG, "未知机型 %04X:%04X，使用通用档案", vid, pid);
        /* 只在没有手写档案时才让 CUPS 的结论改写行为。
         * 手写档案是实测出来的，优先级高于第三方表。 */
        if (q & QK_UNIDIR) {
            s_active.unidir      = true;
            s_active.pjl_ustatus = false;   /* 单向机器发 PJL 收不到回包 */
            s_active.uel_wake    = false;
            ESP_LOGW(TAG, "CUPS quirks: 该机型只支持单向 I/O，禁用 IN 端点与 PJL");
        }
        if (q & QK_SOFT_RESET) {
            s_active.soft_reset = true;
            ESP_LOGI(TAG, "CUPS quirks: 该机型打印后需要 SOFT_RESET");
        }
        if (q & QK_DELAY_CLOSE) {
            s_active.iface_cycle = true;
            ESP_LOGI(TAG, "CUPS quirks: 该机型释放接口前需要延时");
        }
    }

    if (q & QK_BLACKLIST)
        ESP_LOGE(TAG, "CUPS quirks: 该机型被 CUPS 列为 USB 后端不可用，"
                      "大概率打不出来——这条只警告，不阻止尝试");
    if (q & QK_USB_INIT)
        ESP_LOGW(TAG, "CUPS quirks: 该机型需要厂商私有初始化串，本项目未实现");
    if (q & QK_VENDOR_CLASS)
        ESP_LOGW(TAG, "CUPS quirks: 该机型用厂商私有 class，7/1/x 枚举可能认不出");

    return &s_active;
}
