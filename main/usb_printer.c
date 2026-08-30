/* USB host 打印机类驱动：枚举 -> claim 7/x/x 接口 -> 提供 bulk OUT 写通道 */
#include <string.h>
#include <strings.h>          /* strncasecmp */
#include <stdlib.h>           /* malloc/free */
#include <stdarg.h>           /* dump 拼 JSON 用 */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include "usb/usb_host.h"
#include "usb_printer.h"
#include "esp_timer.h"
#include "lcd_ui.h"
#include "joblog.h"
#include "printer_profile.h"
#include "profile_script.h"
#include "cloud_client.h"   /* cloud_device_id() */
#include "driver/gpio.h"
#include "esp_rom_crc.h"

static const char *TAG = "usb_prn";

/* UEL(Universal Exit Language)：HP/三星系的作业分隔符，9 字节。
 * 这是本项目唯一确认有效的「作业结束 / 唤醒」指令——缺它只能打第一份。 */
static const uint8_t UEL[] = { 0x1b, '%', '-', '1', '2', '3', '4', '5', 'X' };
#define UEL_LEN (sizeof UEL)

/* pjl_send_locked：裸发，调用者必须已持有 s_job_mutex。
 * pjl_send_safe  ：自己抢锁；抢不到就放弃——探针永远不该插队到作业前面去。
 * 这两个必须分开：以前只有一个不抢锁的 pjl_send，探针和作业会同时用那个
 * 全局唯一的 s_xfer，对在飞的 transfer 二次 submit 直接 panic，重启后
 * MQTT 重投作业又撞一次，就成了重启循环。 */
static void pjl_send_locked(const char *cmd);
static bool pjl_send_safe(const char *cmd, uint32_t wait_ms);
static uint8_t printer_port_status_raw(void);
static bool printer_device_id(char *out, size_t cap);
static bool devid_field(const char *devid, const char *key, char *out, size_t cap);
static void str_desc_ascii(const usb_str_desc_t *d, char *out, size_t cap);

#define PIN_LED_GREEN   GPIO_NUM_15
#define CHUNK_SIZE      8192
/* 一次 bulk IN 读多少。txt[] 按它开栈，改大要同步看 status_reader_task 的栈。 */
#define IN_READ_MAX     512
/* 每条探针发命令前先空转这么久，把上一条的迟到回包排掉 */
#define PROBE_DRAIN_MS  400

static usb_host_client_handle_t s_client;
static usb_device_handle_t      s_dev;
static uint8_t                  s_itf, s_ep, s_ep_in;
static uint8_t                  s_itf_proto;   /* 1=单向 2=双向 3=1284.4 */
static uint16_t                 s_mps, s_mps_in;
static volatile bool            s_connected;
static volatile bool            s_asleep;   /* 由 USTATUS 推送更新 */
static usb_prn_status_t         s_st;       /* 打印机自报状态，见 .h */
static int      s_base_code;                /* 同一包里的基础状态，非 Printing */
static char     s_base_disp[32];
static int64_t  s_job_done_us;              /* 作业发完的时刻，用于兜底降级 */

void usb_printer_status(usb_prn_status_t *out)
{
    *out = s_st;
    out->connected = s_connected;
    out->asleep    = s_asleep;
}

/* 一个 IN 包里可能连着多条 USTATUS（例如 Ready 紧跟 Power Save），
 * 只有最后一条代表当前状态。 */
static void parse_ustatus(const char *txt)
{
    const char *p = txt, *rec = NULL;
    while ((p = strstr(p, "CODE=")) != NULL) { rec = p; p += 5; }
    if (!rec) return;

    int  code   = atoi(rec + 5);
    bool online = strstr(rec, "ONLINE=TRUE") != NULL;
    char disp[sizeof s_st.display];
    disp[0] = 0;
    const char *d = strstr(rec, "DISPLAY=\"");
    if (d) {
        d += 9;
        const char *e = strchr(d, '\"');
        size_t n = e ? (size_t)(e - d) : strlen(d);
        if (n >= sizeof disp) n = sizeof disp - 1;
        memcpy(disp, d, n);
        disp[n] = 0;
    }
    if (code != 10023) {                    /* 10023=Printing 是瞬时态，不作基础 */
        s_base_code = code;
        strcpy(s_base_disp, disp);
    }
    if (code != s_st.code || online != s_st.online || strcmp(disp, s_st.display)) {
        s_st.code   = code;
        s_st.online = online;
        strcpy(s_st.display, disp);
        s_st.seq++;
    }
}
static const printer_profile_t *s_prof;   /* 当前机型档案 */
static uint8_t                  s_alt;    /* claim 时用的 alternate setting */
static char s_devid_str[320];             /* GET_DEVICE_ID 原文（IEEE-1284） */
static char s_model[64];                  /* 从 device ID 里抠出的 MDL 字段 */
static bool s_pjl_ok;                     /* CMD: 授权发 PJL 吗（第 1 层的开关） */
static char s_serial[48];                 /* 打印机序列号——换机场景的主键 */

/* ── 生效档案 ──
 *
 * 不管怪癖来自服务端下发还是编译进来的内置表，这里都只认 prof_script_t：
 * 内置表由 profile_script_from_builtin() 合成成同一种动作序列。
 * 执行路径只有一条，不用维护「有服务端档案」和「没有」两套分支。 */
static prof_script_t s_script;

/* ── 本次作业的一次性钩子覆盖（接口文档 3.4 / 规则 9）──
 * 服务端做适配测试时会试「不发 UEL 会怎样」。**只对本次作业生效，
 * 绝不写 NVS**；作业一结束就销毁，失败也要销毁——残留会把设备留在
 * 坏状态，用户测一次就再也打不了印。 */
static prof_script_t s_oneshot;
static bool          s_have_oneshot;

void usb_printer_set_script(const prof_script_t *sc)
{
    s_script = *sc;
    ESP_LOGI(TAG, "生效档案 src=%s rev=%d serial=%s begin=%u end=%u wake=%u",
             s_script.src, s_script.rev, s_script.serial,
             s_script.hook[PROF_HOOK_JOB_BEGIN].n,
             s_script.hook[PROF_HOOK_JOB_END].n,
             s_script.hook[PROF_HOOK_WAKE].n);
}

const prof_script_t *usb_printer_script(void){ return &s_script; }

/* 把内置表合成成脚本装上。枚举时先走这条；服务端的档案到了再覆盖。
 * 这样即使连不上云端，怪癖照样按编译进来的表生效。 */
static void apply_builtin_script(void)
{
    prof_script_t sc;
    profile_script_from_builtin(s_prof, &sc);
    usb_printer_set_script(&sc);
}

bool usb_printer_job_hooks(const char *json, size_t len)
{
    char err[128];
    if (!profile_script_parse_hooks(json, len, &s_script, &s_oneshot,
                                    err, sizeof err)) {
        ESP_LOGW(TAG, "一次性钩子解析失败，本次按档案走：%s", err);
        s_have_oneshot = false;
        return false;
    }
    s_have_oneshot = true;
    ESP_LOGW(TAG, "本次作业使用一次性钩子 begin=%u end=%u wake=%u",
             s_oneshot.hook[PROF_HOOK_JOB_BEGIN].n,
             s_oneshot.hook[PROF_HOOK_JOB_END].n,
             s_oneshot.hook[PROF_HOOK_WAKE].n);
    return true;
}

static void oneshot_clear(void)
{
    if (s_have_oneshot) ESP_LOGI(TAG, "一次性钩子已销毁，回到档案设定");
    s_have_oneshot = false;
}

/* 执行一个钩子。
 *
 * skip_iface_reset：本次连接的第一份作业跳过接口复位——设备刚枚举完，
 * 没有上一份作业需要收尾。这保持了改造前的行为。 */
static void run_hook(prof_hook_id_t hid, bool skip_iface_reset)
{
    const prof_script_t *sc = s_have_oneshot ? &s_oneshot : &s_script;
    const prof_hook_t *h = &sc->hook[hid];
    if (!h->n) return;

    /* 钩子里发的字节不计入作业字节数，便于与源文件核对 */
    size_t saved = s_job_bytes;
    for (uint8_t i = 0; i < h->n; i++) {
        const prof_step_t *st = &h->step[i];
        switch (st->op) {
        case PROF_OP_SEND_HEX:
            if (usb_printer_job_write(st->data, st->len) != ESP_OK)
                ESP_LOGW(TAG, "%s[%u] send_hex 失败", prof_hook_name(hid), i);
            break;
        case PROF_OP_DELAY_MS:
            vTaskDelay(pdMS_TO_TICKS(st->ms));
            break;
        case PROF_OP_IFACE_RESET:
            if (skip_iface_reset) break;
            joblog_phase(JOB_RESET_IF, 0, 0);
            printer_port_status();
            interface_cycle();
            break;
        case PROF_OP_READ_STATUS:
            printer_port_status();
            break;
        }
    }
    s_job_bytes = saved;
}

const char *usb_printer_serial(void){ return s_serial; }

/* 服务端下发新档案后重算生效值。打印机已经枚举过时用得上——
 * 否则要等下一次插拔才生效。 */
void usb_printer_reselect_profile(void)
{
    if (!s_connected || !s_dev) return;
    const usb_device_desc_t *dd = NULL;
    if (usb_host_get_device_descriptor(s_dev, &dd) != ESP_OK || !dd) return;
    s_prof = profile_lookup(dd->idVendor, dd->idProduct);
    apply_builtin_script();
    if (s_prof->unidir && s_ep_in) {
        ESP_LOGW(TAG, "新档案标记为单向 I/O，停用 IN 端点 0x%02X", s_ep_in);
        s_ep_in = 0;
    }
}
static QueueHandle_t            s_evt_q;
static SemaphoreHandle_t        s_xfer_done, s_job_mutex;
static usb_transfer_t          *s_xfer;
static volatile usb_transfer_status_t s_status;
static size_t                   s_job_bytes;
static uint32_t                 s_job_crc;
static uint32_t                 s_job_count;

static void xfer_cb(usb_transfer_t *t){ s_status = t->status; xSemaphoreGive(s_xfer_done); }

static void client_cb(const usb_host_client_event_msg_t *m, void *arg)
{
    uint8_t v;
    if (m->event == USB_HOST_CLIENT_EVENT_NEW_DEV) { v = m->new_dev.address; xQueueSend(s_evt_q, &v, 0); }
    else if (m->event == USB_HOST_CLIENT_EVENT_DEV_GONE) { v = 0; xQueueSend(s_evt_q, &v, 0); }
}

static bool find_endpoints(const usb_config_desc_t *cfg, uint8_t *itf, uint8_t *alt,
                           uint8_t *ep_out, uint16_t *mps_out,
                           uint8_t *ep_in, uint16_t *mps_in)
{
    int off = 0; bool in_prn = false; uint8_t ci = 0, ca = 0;
    bool got_out = false;
    *ep_in = 0; *mps_in = 0;
    const usb_standard_desc_t *d = (const usb_standard_desc_t *)cfg;
    while ((d = usb_parse_next_descriptor(d, cfg->wTotalLength, &off)) != NULL) {
        if (d->bDescriptorType == USB_B_DESCRIPTOR_TYPE_INTERFACE) {
            const usb_intf_desc_t *i = (const usb_intf_desc_t *)d;
            if (got_out) break;                 /* 已在打印接口取到端点，别跑到下个接口 */
            ci = i->bInterfaceNumber; ca = i->bAlternateSetting;
            in_prn = (i->bInterfaceClass == 0x07);
            if (in_prn) s_itf_proto = i->bInterfaceProtocol;
        } else if (d->bDescriptorType == USB_B_DESCRIPTOR_TYPE_ENDPOINT && in_prn) {
            const usb_ep_desc_t *e = (const usb_ep_desc_t *)d;
            if ((e->bmAttributes & USB_BM_ATTRIBUTES_XFERTYPE_MASK) != USB_BM_ATTRIBUTES_XFER_BULK)
                continue;
            if (e->bEndpointAddress & 0x80) { *ep_in = e->bEndpointAddress; *mps_in = e->wMaxPacketSize; }
            else { *itf = ci; *alt = ca; *ep_out = e->bEndpointAddress; *mps_out = e->wMaxPacketSize; got_out = true; }
        }
    }
    return got_out;
}

static void lib_task(void *a)
{
    while (1) {
        uint32_t f;
        usb_host_lib_handle_events(portMAX_DELAY, &f);
        if (f & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) usb_host_device_free_all();
    }
}
static void cli_task(void *a){ while (1) usb_host_client_handle_events(s_client, portMAX_DELAY); }

static void enum_task(void *a)
{
    uint8_t addr;
    while (xQueueReceive(s_evt_q, &addr, portMAX_DELAY) == pdTRUE) {
        if (addr == 0) {                       /* 拔出 */
            s_connected = false;
            gpio_set_level(PIN_LED_GREEN, 0);
            if (s_dev) { usb_host_interface_release(s_client, s_dev, s_itf);
                         usb_host_device_close(s_client, s_dev); s_dev = NULL; }
            s_serial[0] = 0;      /* 规则 10：拔掉后 serial 必须立刻变空串 */
            oneshot_clear();
            ESP_LOGW(TAG, "打印机已拔出");
            lcd_ui_prn("已拔出");
            continue;
        }
        if (s_connected) continue;
        usb_device_handle_t dev;
        if (usb_host_device_open(s_client, addr, &dev) != ESP_OK) continue;
        const usb_config_desc_t *cfg; uint8_t itf, alt, ep, ep_in; uint16_t mps, mps_in;
        if (usb_host_get_active_config_descriptor(dev, &cfg) != ESP_OK ||
            !find_endpoints(cfg, &itf, &alt, &ep, &mps, &ep_in, &mps_in) ||
            usb_host_interface_claim(s_client, dev, itf, alt) != ESP_OK) {
            ESP_LOGW(TAG, "设备 %u 不是可用打印机", addr);
            usb_host_device_close(s_client, dev);
            continue;
        }
        s_dev = dev; s_itf = itf; s_ep = ep; s_mps = mps; s_alt = alt;
        s_ep_in = ep_in; s_mps_in = mps_in;
        s_connected = true;
        gpio_set_level(PIN_LED_GREEN, 1);
        const usb_device_desc_t *dd;
        usb_host_get_device_descriptor(dev, &dd);
        ESP_LOGI(TAG, "打印机就绪 VID=0x%04X PID=0x%04X 出=0x%02X/%u 入=0x%02X/%u",
                 dd->idVendor, dd->idProduct, ep, mps, ep_in, mps_in);

        /* ── 第 0 层探测：只走控制传输，不碰打印通道，不可能出纸 ── */
        s_devid_str[0] = 0; s_model[0] = 0; s_serial[0] = 0;
        {   /* 序列号是换机场景的主键：设备 MAC 标识的是桥，不是打印机 */
            usb_device_info_t di = { 0 };
            if (usb_host_device_info(dev, &di) == ESP_OK)
                str_desc_ascii(di.str_desc_serial_num, s_serial, sizeof s_serial);
        }
        if (printer_device_id(s_devid_str, sizeof s_devid_str)) {
            ESP_LOGI(TAG, "设备 ID: %s", s_devid_str);
            if (!devid_field(s_devid_str, "MDL", s_model, sizeof s_model))
                devid_field(s_devid_str, "MODEL", s_model, sizeof s_model);
            char cmd[160];
            if (devid_field(s_devid_str, "CMD", cmd, sizeof cmd) ||
                devid_field(s_devid_str, "COMMAND SET", cmd, sizeof cmd))
                ESP_LOGI(TAG, "命令集: %s", cmd);
        } else {
            ESP_LOGW(TAG, "GET_DEVICE_ID 失败——该机型可能不支持，或接口/alt 不对");
        }

        /* 选机型档案。以前这里漏了，导致 s_prof 恒为 NULL，
         * uel_wake 和 iface_cycle 两条实测怪癖实际上从未生效过。 */
        s_prof = profile_lookup(dd->idVendor, dd->idProduct);
        apply_builtin_script();

        /* 单向机器：不能读 bulk IN。把 s_ep_in 清零，status_reader_task
         * 就会自己空转（它开头就检查 !s_ep_in），不必再加一处判断。
         * 对这类机器提交 IN 传输会一直超时，严重时拖垮整个 host 栈。 */
        if (s_prof->unidir && s_ep_in) {
            ESP_LOGW(TAG, "档案标记为单向 I/O，停用 IN 端点 0x%02X", s_ep_in);
            s_ep_in = 0;
        }

        lcd_ui_prn(s_model[0] ? s_model : "打印机已连接");

        /* 开启异步状态上报：之后打印机会主动推 Ready/Power Save/Printing，
         * 这是唯一可靠的休眠判据。
         * 只在设备自报懂 PJL 时才发——盲发会被当正文打出来。
         * device ID 读不到时按老行为发（这台机器已实测支持）。 */
        /* 该不该发 PJL，两个来源，优先级不能搞反：
         *
         *  ① 手写档案的 pjl_ustatus —— 实测结论，**赢**
         *  ② device ID 的 CMD: 里有没有 PJL/PCL —— 启发式，只给未知机型兜底
         *
         * 为什么必须这个顺序：136a 的 CMD: 是
         *   SPL,URF,FWV,PIC,RDS,AMPV,PWGRaster,EXT
         * 一个 PJL 字样都没有，但它**确实吃 PJL**——USTATUS 实测可用，
         * 面板状态和休眠检测全靠它。只信 CMD: 会把这台机器的状态回传弄死。
         * 通用兜底档案的 pjl_ustatus 是 false，所以未知机型仍然只认 CMD:。 */
        bool cmd_pjl = false;
        if (s_devid_str[0]) {
            char cmd[160] = { 0 };
            if (devid_field(s_devid_str, "CMD", cmd, sizeof cmd) ||
                devid_field(s_devid_str, "COMMAND SET", cmd, sizeof cmd)) {
                for (char *q = cmd; *q; q++)
                    if (*q >= 'a' && *q <= 'z') *q -= 32;      /* 就地转大写再匹配 */
                cmd_pjl = (strstr(cmd, "PJL") || strstr(cmd, "PCL"));
            }
        }
        bool pjl_ok = !s_prof->unidir && (s_prof->pjl_ustatus || cmd_pjl);
        s_pjl_ok = pjl_ok;
        if (pjl_ok)
            pjl_send_safe("@PJL USTATUS DEVICE=ON", 2000);
        else
            ESP_LOGW(TAG, "不发 PJL：档案未标 pjl_ustatus 且 CMD: 里没有 PJL/PCL"
                          "——对陌生机器盲发会被当正文打出来");

        /* PJL 探针（INFO ID / CONFIG / SUPPLIES 等）现在可以随时调
         * usb_printer_probe() 触发：它持作业锁、复用现役 IN 读取任务，
         * 不再与作业争用 s_xfer，那条重启循环已经消掉。 */
    }
}

/* 打印机是双向接口(7/1/2)：不排空 bulk IN，它的回传缓冲满了就整台停摆。
 * 顺带把回传内容打出来——打印机的报错就写在里面。 */
static SemaphoreHandle_t s_in_done;
static volatile bool s_in_paused, s_in_idle;
static volatile usb_transfer_status_t s_in_status;
static void in_cb(usb_transfer_t *t){ s_in_status = t->status; xSemaphoreGive(s_in_done); }

/* 探针回包捕获。
 * 关键：不另起一个读 bulk IN 的任务——两个任务同时在同一个 IN 端点上
 * submit 是另一条 panic 路径。复用现有的 status_reader_task，探针期间
 * 让它把收到的字节顺带抄一份到这里。
 *
 * 缓冲按调用方给的容量动态分配：@PJL INFO VARIABLES 能有好几 KB，
 * 而心跳类探针几十字节就够，固定大小要么不够要么白占 RAM。
 * s_probe_mux 保护 buf 指针——捕获发生在 status_reader_task，
 * 分配/释放发生在 probe 调用方，两个任务。 */
static volatile bool     s_probe_on;
static SemaphoreHandle_t s_probe_mux;
static char             *s_probe_buf;
static size_t            s_probe_cap, s_probe_len;

/* 收原始字节，不收已清洗的字符串。
 * \r \n \t 必须原样保留——PJL 回包是按行的 key=value，
 * 吃掉换行整个回包就没法解析了（这是以前的真实缺陷）。 */
static void probe_capture(const uint8_t *d, int n)
{
    if (!s_probe_on || n <= 0 || !s_probe_mux) return;
    if (xSemaphoreTake(s_probe_mux, 0) != pdTRUE) return;
    if (s_probe_on && s_probe_buf) {
        for (int i = 0; i < n && s_probe_len + 1 < s_probe_cap; i++) {
            uint8_t c = d[i];
            bool keep = (c >= 0x20 && c < 0x7f) || c == '\r' || c == '\n' || c == '\t';
            s_probe_buf[s_probe_len++] = keep ? (char)c : '.';
        }
        s_probe_buf[s_probe_len] = 0;
    }
    xSemaphoreGive(s_probe_mux);
}

static void status_reader_task(void *a)
{
    usb_transfer_t *x = NULL;
    while (1) {
        if (s_in_paused) { s_in_idle = true; vTaskDelay(pdMS_TO_TICKS(20)); continue; }
        s_in_idle = false;
        if (!s_connected || !s_ep_in) { vTaskDelay(pdMS_TO_TICKS(500)); continue; }
        if (!x && usb_host_transfer_alloc(512, 0, &x) != ESP_OK) {
            vTaskDelay(pdMS_TO_TICKS(1000)); continue;
        }
        x->device_handle = s_dev;
        x->bEndpointAddress = s_ep_in;
        x->callback = in_cb;
        x->num_bytes = (s_mps_in && (IN_READ_MAX % s_mps_in) == 0) ? IN_READ_MAX : s_mps_in;
        x->timeout_ms = 500;
        if (usb_host_transfer_submit(x) != ESP_OK) { vTaskDelay(pdMS_TO_TICKS(500)); continue; }
        if (xSemaphoreTake(s_in_done, pdMS_TO_TICKS(1500)) != pdTRUE) {
            vTaskDelay(pdMS_TO_TICKS(200)); continue;
        }
        if (s_in_status != USB_TRANSFER_STATUS_COMPLETED &&
            s_in_status != USB_TRANSFER_STATUS_TIMED_OUT) {
            ESP_LOGW(TAG, "IN 传输异常 status=%d", s_in_status);
        }
        if (s_in_status == USB_TRANSFER_STATUS_COMPLETED && x->actual_num_bytes > 0) {
            int n = x->actual_num_bytes;

            /* 探针拿原始字节（保留换行），状态解析拿清洗过的字符串。
             * 以前两者共用一个截到 128 字节的缓冲，而这里一次实际读 512——
             * INFO CONFIG / VARIABLES 的回包被静默切掉，且不报错。 */
            probe_capture(x->data_buffer, n);

            char txt[IN_READ_MAX + 1];
            int k = 0;
            for (int i = 0; i < n && k < IN_READ_MAX; i++) {
                uint8_t c = x->data_buffer[i];
                txt[k++] = (c >= 0x20 && c < 0x7f) ? c : '.';
            }
            txt[k] = 0;
            /* 日志只显示前 128 字节，避免一包 512 字节刷屏 */
            ESP_LOGW(TAG, "打印机回传 %d 字节: %.128s%s", n, txt,
                     k > 128 ? " …(日志截断，解析用的是全量)" : "");
            /* 打印机开了 USTATUS 后会主动推状态——这才是真正的休眠指示器。
             * 那个 GET_PORT_STATUS 字节在休眠时照样报 0x18，毫无用处。 */
            parse_ustatus(txt);
            if (strstr(txt, "Power Save"))   { s_asleep = true;  lcd_ui_prn("休眠中"); }
            else if (strstr(txt, "Ready"))   { s_asleep = false; lcd_ui_prn("就绪"); }
            else if (strstr(txt, "Printing")) { s_asleep = false; lcd_ui_prn("打印中"); }
        }
        /* 打印机只在状态变化时推 USTATUS。我们把活发完后它常常不再补
         * 一条 Ready，面板状态就永远卡在 Printing。作业结束 20 秒后仍是
         * Printing，就退回它自己在同一包里给出的基础状态——不是我们编的。 */
        if (s_st.code == 10023 && s_base_code && s_job_done_us &&
            esp_timer_get_time() - s_job_done_us > 20LL * 1000 * 1000) {
            s_st.code = s_base_code;
            strcpy(s_st.display, s_base_disp);
            s_st.seq++;
            s_job_done_us = 0;
            ESP_LOGI(TAG, "作业已结束，状态退回 %s", s_base_disp);
        }
        vTaskDelay(pdMS_TO_TICKS(150));
    }
}

/* 打印机类控制请求 */
static SemaphoreHandle_t s_ctrl_done;
static void ctrl_cb(usb_transfer_t *t){ xSemaphoreGive(s_ctrl_done); }

static uint8_t printer_port_status_raw(void)
{
    uint8_t result = 0;
    if (!s_connected) return 0;
    usb_transfer_t *x;
    if (usb_host_transfer_alloc(64, 0, &x) != ESP_OK) return 0;
    usb_setup_packet_t *sp = (usb_setup_packet_t *)x->data_buffer;
    sp->bmRequestType = 0xA1;          /* 类请求 | 接口 | 设备->主机 */
    sp->bRequest      = 1;             /* GET_PORT_STATUS */
    sp->wValue        = 0;
    sp->wIndex        = s_itf;
    sp->wLength       = 1;
    x->device_handle    = s_dev;
    x->bEndpointAddress = 0;
    x->callback         = ctrl_cb;
    x->num_bytes        = sizeof(usb_setup_packet_t) + 1;
    x->timeout_ms       = 2000;
    if (usb_host_transfer_submit_control(s_client, x) == ESP_OK &&
        xSemaphoreTake(s_ctrl_done, pdMS_TO_TICKS(2500)) == pdTRUE &&
        x->actual_num_bytes > (int)sizeof(usb_setup_packet_t)) {
        result = x->data_buffer[sizeof(usb_setup_packet_t)];
    }
    usb_host_transfer_free(x);
    return result;
}

/* GET_DEVICE_ID —— USB 打印机类标准请求（类规范 §4.2.1）。
 * 返回 IEEE-1284 设备 ID 串，形如：
 *   MFG:HP;CMD:SPL,URF,PWGRaster,...;MDL:HP Laser MFP 131 133 135-138;CLS:PRINTER;
 * 其中 CMD: 是整条兼容性链路的地基——它决定后面哪些探针可以发。
 *
 * 两个必须注意的点：
 *  1) 头两个字节是**大端**长度，且【含这两个字节自身】；
 *  2) wIndex 是 (接口号<<8)|alt，不是接口号本身——写错会 STALL。
 * 返回的串不保证 NUL 结尾，得自己补。 */
static bool printer_device_id(char *out, size_t cap)
{
    if (!s_connected || cap < 2) return false;
    const uint16_t want = 512;
    usb_transfer_t *x;
    if (usb_host_transfer_alloc(sizeof(usb_setup_packet_t) + want, 0, &x) != ESP_OK)
        return false;

    usb_setup_packet_t *sp = (usb_setup_packet_t *)x->data_buffer;
    sp->bmRequestType = 0xA1;                       /* 类 | 接口 | 设备->主机 */
    sp->bRequest      = 0;                          /* GET_DEVICE_ID */
    sp->wValue        = 0;                          /* 配置索引 */
    sp->wIndex        = (uint16_t)(s_itf << 8) | s_alt;
    sp->wLength       = want;
    x->device_handle    = s_dev;
    x->bEndpointAddress = 0;
    x->callback         = ctrl_cb;
    x->num_bytes        = sizeof(usb_setup_packet_t) + want;
    x->timeout_ms       = 3000;

    bool ok = false;
    if (usb_host_transfer_submit_control(s_client, x) == ESP_OK &&
        xSemaphoreTake(s_ctrl_done, pdMS_TO_TICKS(3500)) == pdTRUE &&
        x->status == USB_TRANSFER_STATUS_COMPLETED) {
        int got = x->actual_num_bytes - (int)sizeof(usb_setup_packet_t);
        const uint8_t *p = x->data_buffer + sizeof(usb_setup_packet_t);
        if (got >= 2) {
            int declared = (p[0] << 8) | p[1];      /* 大端，含这 2 字节 */
            int len = declared - 2;
            if (len < 0 || len > got - 2) len = got - 2;   /* 有的机器长度不实，以实收为准 */
            if (len > (int)cap - 1) len = cap - 1;
            /* 串里可能混进不可打印字节，就地清洗 */
            for (int i = 0; i < len; i++) {
                uint8_t c = p[2 + i];
                out[i] = (c >= 0x20 && c < 0x7f) ? (char)c : ' ';
            }
            out[len] = 0;
            ok = (len > 0);
        }
    }
    usb_host_transfer_free(x);
    return ok;
}

/* 从 device ID 里取一个字段，如 key="MDL"。找不到返回 false。
 * 兼容 MDL/MODEL 两种写法由调用方决定，这里只做单键查找。 */
static bool devid_field(const char *devid, const char *key, char *out, size_t cap)
{
    size_t klen = strlen(key);
    for (const char *p = devid; *p; ) {
        while (*p == ' ' || *p == ';') p++;
        if (!*p) break;
        const char *colon = strchr(p, ':');
        const char *semi  = strchr(p, ';');
        if (!colon || (semi && colon > semi)) { p = semi ? semi + 1 : p + strlen(p); continue; }
        size_t nlen = (size_t)(colon - p);
        if (nlen == klen && strncasecmp(p, key, klen) == 0) {
            const char *v = colon + 1;
            size_t vlen = semi ? (size_t)(semi - v) : strlen(v);
            if (vlen > cap - 1) vlen = cap - 1;
            memcpy(out, v, vlen);
            out[vlen] = 0;
            return vlen > 0;
        }
        p = semi ? semi + 1 : p + strlen(p);
    }
    return false;
}

static void printer_port_status(void)
{
    uint8_t st = printer_port_status_raw();
    if (st) {
        bool po = !!(st & 0x20), er = !(st & 0x08);
        if (po != s_st.paper_out || er != s_st.error) {
            s_st.paper_out = po; s_st.error = er; s_st.seq++;
        }
    }
    if (st) ESP_LOGI(TAG, "端口状态 0x%02X  纸尽=%d 选中=%d 无错=%d",
                     st, !!(st & 0x20), !!(st & 0x10), !!(st & 0x08));
    else    ESP_LOGW(TAG, "端口状态读取失败");
}

/* 标准请求 CLEAR_FEATURE(ENDPOINT_HALT)：USB 2.0 §9.4.5 规定它会把
 * **设备侧**该端点的 data toggle 复位到 DATA0。ESP-IDF 的 claim 只动主机侧，
 * 两边必须同时归零才对齐。 */
static bool clear_ep_halt(uint8_t ep)
{
    if (!ep) return true;
    usb_transfer_t *x;
    if (usb_host_transfer_alloc(64, 0, &x) != ESP_OK) return false;
    usb_setup_packet_t *sp = (usb_setup_packet_t *)x->data_buffer;
    sp->bmRequestType = 0x02;      /* 标准 | 主机->设备 | 收件人=端点 */
    sp->bRequest      = 1;         /* CLEAR_FEATURE */
    sp->wValue        = 0;         /* ENDPOINT_HALT */
    sp->wIndex        = ep;
    sp->wLength       = 0;
    x->device_handle    = s_dev;
    x->bEndpointAddress = 0;
    x->callback         = ctrl_cb;
    x->num_bytes        = sizeof(usb_setup_packet_t);
    x->timeout_ms       = 3000;
    bool ok = false;
    if (usb_host_transfer_submit_control(s_client, x) == ESP_OK &&
        xSemaphoreTake(s_ctrl_done, pdMS_TO_TICKS(3500)) == pdTRUE)
        ok = (x->status == USB_TRANSFER_STATUS_COMPLETED);
    usb_host_transfer_free(x);
    return ok;
}

/* 发一条 PJL 命令。两个必须遵守的点：
 *  1) 长度用 snprintf 的返回值，绝不硬编码——写错会多发越界垃圾字节；
 *  2) 结尾必须再补一个 UEL，否则打印机停在 PJL 模式等后续数据，
 *     面板显示 printing 不动，还会把命令文本当正文打出来。 */
static void pjl_send_locked(const char *cmd)
{
    if (!s_connected) return;
    char buf[128];
    int n = snprintf(buf, sizeof buf, "\x1b%%-12345X%s\r\n\x1b%%-12345X", cmd);
    size_t saved = s_job_bytes;
    usb_printer_job_write((const uint8_t *)buf, n);
    s_job_bytes = saved;
}

/* 抢到作业锁再发。抢不到说明正在打印，直接放弃——探针的优先级低于作业，
 * 而且抢同一个 s_xfer 就是那条 panic 路径。 */
static bool pjl_send_safe(const char *cmd, uint32_t wait_ms)
{
    if (!s_connected || !s_job_mutex) return false;
    if (xSemaphoreTake(s_job_mutex, pdMS_TO_TICKS(wait_ms)) != pdTRUE) {
        ESP_LOGW(TAG, "打印机忙，跳过 PJL：%s", cmd);
        return false;
    }
    pjl_send_locked(cmd);
    xSemaphoreGive(s_job_mutex);
    return true;
}

/* 按需探针：发一条 PJL 命令并把打印机的回包收上来。
 *
 * 安全边界（这三条是它不再把整机搞重启的原因）：
 *  - 全程持作业锁，绝不与打印作业并发使用 s_xfer
 *  - 不新开读 IN 端点的任务，回包由现役的 status_reader_task 顺带抄给我们
 *  - 抢不到锁就返回 ESP_ERR_TIMEOUT，不等、不重试
 *
 * 注意调用方的责任：**别对不认识的打印机盲发**。PJL 只有在 GET_DEVICE_ID
 * 的 CMD: 字段里出现 PJL/PCL 时才该发；猜错的话打印机会把命令当正文打出来，
 * 白费一张纸。 */
esp_err_t usb_printer_probe(const char *pjl_cmd, char *out, size_t cap, uint32_t wait_ms)
{
    if (!s_connected || !s_job_mutex || !out || cap == 0) return ESP_ERR_INVALID_STATE;
    if (xSemaphoreTake(s_job_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
        ESP_LOGW(TAG, "打印机忙，探针放弃：%s", pjl_cmd);
        return ESP_ERR_TIMEOUT;
    }

    /* 捕获缓冲按调用方给的容量分配：想拿 INFO VARIABLES 就传个大的 out */
    xSemaphoreTake(s_probe_mux, portMAX_DELAY);
    s_probe_buf = malloc(cap);
    s_probe_cap = cap;
    s_probe_len = 0;
    if (s_probe_buf) { s_probe_buf[0] = 0; s_probe_on = true; }
    xSemaphoreGive(s_probe_mux);
    if (!s_probe_buf) { xSemaphoreGive(s_job_mutex); return ESP_ERR_NO_MEM; }

    /* ── ① 先排干上一条探针的迟到回包 ──
     * 打印机从 Power Save 醒来时，回包能比命令晚 3 秒以上到。实测
     * INFO STATUS 的应答落进了下一条 INFO PAGECOUNT 的缓冲里，
     * 结果两条数据贴着同一个标签——比丢数据更糟，因为看起来是对的。
     * 捕获已经打开，这里空转一会儿专收上一条的尾巴，然后清零。 */
    vTaskDelay(pdMS_TO_TICKS(PROBE_DRAIN_MS));
    xSemaphoreTake(s_probe_mux, portMAX_DELAY);
    size_t drained = s_probe_len;
    s_probe_len = 0;
    s_probe_buf[0] = 0;
    xSemaphoreGive(s_probe_mux);
    if (drained)
        ESP_LOGW(TAG, "排掉 %u 字节上一条探针的迟到回包", (unsigned)drained);

    pjl_send_locked(pjl_cmd);

    /* ── ② 等命令回显出现，再判稳定 ──
     * PJL 的应答一律以命令自身开头（如 "@PJL INFO STATUS"），拿它做配对，
     * 就不必靠"沉默多久算收完"去猜——那个启发式对刚睡醒的机器根本不成立。 */
    const uint32_t step = 100;
    uint32_t waited = 0;
    size_t stable = 0, same = 0;
    bool echoed = false;
    while (waited < wait_ms) {
        vTaskDelay(pdMS_TO_TICKS(step));
        waited += step;
        if (!echoed) {
            xSemaphoreTake(s_probe_mux, portMAX_DELAY);
            echoed = (s_probe_buf && strstr(s_probe_buf, pjl_cmd) != NULL);
            xSemaphoreGive(s_probe_mux);
            if (!echoed) continue;           /* 回显还没来，继续等，先不判稳 */
            stable = 0; same = 0;            /* 从回显出现那一刻才开始判稳 */
        }
        if (s_probe_len && s_probe_len == stable) {
            if (++same >= 3) break;          /* 连续 300ms 没新字节，认为收完 */
        } else {
            stable = s_probe_len; same = 0;
        }
    }
    if (!echoed && s_probe_len)
        ESP_LOGW(TAG, "「%s」收到 %u 字节但没匹配到命令回显——可能仍是错位的",
                 pjl_cmd, (unsigned)s_probe_len);

    xSemaphoreTake(s_probe_mux, portMAX_DELAY);
    s_probe_on = false;
    size_t n = s_probe_len < cap - 1 ? s_probe_len : cap - 1;
    memcpy(out, s_probe_buf, n);
    out[n] = 0;
    free(s_probe_buf);
    s_probe_buf = NULL; s_probe_cap = 0; s_probe_len = 0;
    xSemaphoreGive(s_probe_mux);

    xSemaphoreGive(s_job_mutex);
    ESP_LOGI(TAG, "探针「%s」回包 %u 字节: %.200s%s", pjl_cmd, (unsigned)n, out,
             n > 200 ? " …" : "");
    return n ? ESP_OK : ESP_ERR_NOT_FOUND;
}

/* ── 第 0 层全量 dump ──
 * 全程只用控制传输和已缓存的描述符，不碰打印通道，**不可能出纸**。
 * 输出 JSON，服务端直接入库。 */

/* 往缓冲追加，越界就停——所有拼接都走它，避免到处判长度 */
static void jw(char *b, size_t cap, size_t *k, const char *fmt, ...)
{
    if (*k + 1 >= cap) return;
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(b + *k, cap - *k, fmt, ap);
    va_end(ap);
    if (n > 0) *k += ((size_t)n < cap - *k) ? (size_t)n : (cap - *k - 1);
}

/* JSON 字符串值，带转义 */
static void jstr(char *b, size_t cap, size_t *k, const char *v)
{
    jw(b, cap, k, "\"");
    for (const char *p = v; p && *p; p++) {
        if (*p == '"' || *p == '\\')      jw(b, cap, k, "\\%c", *p);
        else if ((unsigned char)*p < 0x20) jw(b, cap, k, "\\u%04x", *p);
        else                               jw(b, cap, k, "%c", *p);
    }
    jw(b, cap, k, "\"");
}

/* USB 字符串描述符是 UTF-16LE，这里只取 ASCII 面（型号/序列号实际都是 ASCII） */
static void str_desc_ascii(const usb_str_desc_t *d, char *out, size_t cap)
{
    out[0] = 0;
    if (!d || d->bLength < 2) return;
    int chars = (d->bLength - 2) / 2;
    size_t k = 0;
    for (int i = 0; i < chars && k + 1 < cap; i++) {
        uint16_t w = d->wData[i];
        out[k++] = (w >= 0x20 && w < 0x7f) ? (char)w : '?';
    }
    out[k] = 0;
}

static const char *ep_type_name(uint8_t bmAttributes)
{
    switch (bmAttributes & USB_BM_ATTRIBUTES_XFERTYPE_MASK) {
    case USB_BM_ATTRIBUTES_XFER_CONTROL: return "control";
    case USB_BM_ATTRIBUTES_XFER_ISOC:    return "isoc";
    case USB_BM_ATTRIBUTES_XFER_BULK:    return "bulk";
    default:                             return "interrupt";
    }
}

esp_err_t usb_printer_describe(char *out, size_t cap)
{
    if (!s_connected || !s_dev || !out || cap < 64) return ESP_ERR_INVALID_STATE;
    const usb_device_desc_t *dd = NULL;
    const usb_config_desc_t *cfg = NULL;
    usb_device_info_t info = { 0 };
    usb_host_get_device_descriptor(s_dev, &dd);
    usb_host_get_active_config_descriptor(s_dev, &cfg);
    usb_host_device_info(s_dev, &info);
    if (!dd) return ESP_FAIL;

    char mfr[64], prod[96], sn[64];
    str_desc_ascii(info.str_desc_manufacturer, mfr,  sizeof mfr);
    str_desc_ascii(info.str_desc_product,      prod, sizeof prod);
    str_desc_ascii(info.str_desc_serial_num,   sn,   sizeof sn);

    size_t k = 0;
    jw(out, cap, &k, "{\"usb\":{");
    jw(out, cap, &k, "\"vid\":\"0x%04X\",\"pid\":\"0x%04X\"", dd->idVendor, dd->idProduct);
    jw(out, cap, &k, ",\"bcd_device\":\"0x%04X\",\"bcd_usb\":\"0x%04X\"", dd->bcdDevice, dd->bcdUSB);
    jw(out, cap, &k, ",\"dev_class\":%u,\"dev_subclass\":%u,\"dev_protocol\":%u",
       dd->bDeviceClass, dd->bDeviceSubClass, dd->bDeviceProtocol);
    jw(out, cap, &k, ",\"ep0_mps\":%u,\"num_configs\":%u", dd->bMaxPacketSize0, dd->bNumConfigurations);
    jw(out, cap, &k, ",\"speed\":\"%s\"",
       info.speed == USB_SPEED_LOW ? "low" : info.speed == USB_SPEED_FULL ? "full" : "high");
    jw(out, cap, &k, ",\"manufacturer\":"); jstr(out, cap, &k, mfr);
    jw(out, cap, &k, ",\"product\":");      jstr(out, cap, &k, prod);
    /* 序列号是每台打印机唯一的，做兼容性库时它才是主键——设备 MAC 标识的是桥不是打印机 */
    jw(out, cap, &k, ",\"serial\":");       jstr(out, cap, &k, sn);

    if (cfg) {
        jw(out, cap, &k, ",\"self_powered\":%s,\"max_power_ma\":%u",
           (cfg->bmAttributes & 0x40) ? "true" : "false", cfg->bMaxPower * 2);
        jw(out, cap, &k, ",\"num_interfaces\":%u", cfg->bNumInterfaces);

        /* 全部接口都列出来——一体机常常还挂着扫描/存储/DOT4/厂商私有接口，
         * find_endpoints() 取到第一个 bulk OUT 就 break 了，看不到这些。 */
        jw(out, cap, &k, ",\"interfaces\":[");
        int off = 0, nitf = 0;
        bool ep_open = false;
        const usb_standard_desc_t *d = (const usb_standard_desc_t *)cfg;
        while ((d = usb_parse_next_descriptor(d, cfg->wTotalLength, &off)) != NULL) {
            if (d->bDescriptorType == USB_B_DESCRIPTOR_TYPE_INTERFACE) {
                const usb_intf_desc_t *i = (const usb_intf_desc_t *)d;
                if (ep_open) { jw(out, cap, &k, "]}"); ep_open = false; }
                jw(out, cap, &k, "%s{\"num\":%u,\"alt\":%u,\"class\":%u,"
                                 "\"subclass\":%u,\"protocol\":%u,\"eps\":[",
                   nitf ? "," : "", i->bInterfaceNumber, i->bAlternateSetting,
                   i->bInterfaceClass, i->bInterfaceSubClass, i->bInterfaceProtocol);
                nitf++; ep_open = true;
            } else if (d->bDescriptorType == USB_B_DESCRIPTOR_TYPE_ENDPOINT && ep_open) {
                const usb_ep_desc_t *e = (const usb_ep_desc_t *)d;
                jw(out, cap, &k, "%s{\"addr\":\"0x%02X\",\"dir\":\"%s\","
                                 "\"type\":\"%s\",\"mps\":%u}",
                   out[k - 1] == '[' ? "" : ",", e->bEndpointAddress,
                   (e->bEndpointAddress & 0x80) ? "in" : "out",
                   ep_type_name(e->bmAttributes), e->wMaxPacketSize);
            }
        }
        if (ep_open) jw(out, cap, &k, "]}");
        jw(out, cap, &k, "]");
    }
    jw(out, cap, &k, "}");

    /* 打印机类：IEEE-1284 设备 ID + 端口状态 */
    jw(out, cap, &k, ",\"printer_class\":{\"device_id\":");
    jstr(out, cap, &k, s_devid_str);
    jw(out, cap, &k, ",\"model\":"); jstr(out, cap, &k, s_model);
    jw(out, cap, &k, ",\"itf\":%u,\"alt\":%u,\"ep_out\":\"0x%02X\"", s_itf, s_alt, s_ep);
    if (s_ep_in) jw(out, cap, &k, ",\"ep_in\":\"0x%02X\"", s_ep_in);
    else         jw(out, cap, &k, ",\"ep_in\":null");
    jw(out, cap, &k, ",\"port_status\":%u", printer_port_status_raw());
    jw(out, cap, &k, ",\"pjl_allowed\":%s}", s_pjl_ok ? "true" : "false");

    /* 本机给它选了哪份档案 */
    jw(out, cap, &k, ",\"profile\":{\"name\":");
    jstr(out, cap, &k, s_prof ? s_prof->name : "");
    jw(out, cap, &k, ",\"cups_quirks\":%u", s_prof ? s_prof->cups_quirks : 0);
    if (s_prof)
        jw(out, cap, &k, ",\"uel_job_end\":%s,\"uel_wake\":%s,\"wake_delay_ms\":%u,"
                         "\"iface_cycle\":%s,\"unidir\":%s",
           s_prof->uel_job_end ? "true" : "false", s_prof->uel_wake ? "true" : "false",
           (unsigned)s_prof->wake_delay_ms,
           s_prof->iface_cycle ? "true" : "false", s_prof->unidir ? "true" : "false");
    jw(out, cap, &k, "}}");

    return (k + 1 < cap) ? ESP_OK : ESP_ERR_INVALID_SIZE;   /* 满了说明被截断 */
}

/* 精简机型身份，格式按 API 文档 3.8。**必须 <= 512 字节**——
 * 它是 retain=1 的 MQTT 消息，每个订阅者一连上就要吃一份。 */
esp_err_t usb_printer_ident_json(char *out, size_t cap)
{
    if (!s_connected || !s_dev || !out) return ESP_ERR_INVALID_STATE;
    const usb_device_desc_t *dd = NULL;
    usb_device_info_t info = { 0 };
    usb_host_get_device_descriptor(s_dev, &dd);
    usb_host_device_info(s_dev, &info);
    if (!dd) return ESP_FAIL;

    char mfg[32], cmd[128];
    const char *sn = s_serial;
    /* MFG 优先取 IEEE-1284 里的，那是打印机自报的；描述符里的字符串可能是空的 */
    if (!devid_field(s_devid_str, "MFG", mfg, sizeof mfg) &&
        !devid_field(s_devid_str, "MANUFACTURER", mfg, sizeof mfg))
        str_desc_ascii(info.str_desc_manufacturer, mfg, sizeof mfg);
    if (!devid_field(s_devid_str, "CMD", cmd, sizeof cmd))
        devid_field(s_devid_str, "COMMAND SET", cmd, sizeof cmd);

    size_t k = 0;
    jw(out, cap, &k, "{\"dev\":\"%s\"", cloud_device_id());
    jw(out, cap, &k, ",\"vid\":\"%04X\",\"pid\":\"%04X\"", dd->idVendor, dd->idProduct);
    jw(out, cap, &k, ",\"make\":");   jstr(out, cap, &k, mfg);
    jw(out, cap, &k, ",\"model\":");  jstr(out, cap, &k, s_model);
    jw(out, cap, &k, ",\"serial\":"); jstr(out, cap, &k, sn);
    jw(out, cap, &k, ",\"proto\":%u", s_itf_proto);
    jw(out, cap, &k, ",\"cmd\":");    jstr(out, cap, &k, cmd);
    jw(out, cap, &k, "}");
    return (k + 1 < cap) ? ESP_OK : ESP_ERR_INVALID_SIZE;
}

bool        usb_printer_pjl_allowed(void){ return s_pjl_ok; }
const char *usb_printer_device_id(void){ return s_devid_str; }
const char *usb_printer_model(void)    { return s_model[0] ? s_model : "未知型号"; }
uint8_t     usb_printer_quirks(void)   { return s_prof ? s_prof->cups_quirks : 0; }
const char *usb_printer_profile_name(void){ return s_prof ? s_prof->name : "未匹配"; }

static void interface_cycle(void)
{
    if (!s_connected || !s_dev) return;

    s_in_paused = true;
    for (int i = 0; i < 250 && !s_in_idle; i++) vTaskDelay(pdMS_TO_TICKS(20));
    vTaskDelay(pdMS_TO_TICKS(100));

    if (s_ep_in) {
        usb_host_endpoint_halt(s_dev, s_ep_in);
        usb_host_endpoint_flush(s_dev, s_ep_in);
        usb_host_endpoint_clear(s_dev, s_ep_in);
    }
    usb_host_endpoint_halt(s_dev, s_ep);
    usb_host_endpoint_flush(s_dev, s_ep);
    usb_host_endpoint_clear(s_dev, s_ep);
    vTaskDelay(pdMS_TO_TICKS(50));

    esp_err_t r1 = usb_host_interface_release(s_client, s_dev, s_itf);
    vTaskDelay(pdMS_TO_TICKS(200));
    esp_err_t r2 = ESP_OK;
    if (r1 == ESP_OK) r2 = usb_host_interface_claim(s_client, s_dev, s_itf, 0);
    ESP_LOGI(TAG, "接口重置 release=%s claim=%s", esp_err_to_name(r1), esp_err_to_name(r2));
    if (r2 != ESP_OK) { s_connected = false; gpio_set_level(PIN_LED_GREEN, 0); }

    /* 主机侧刚被 claim 打回 DATA0，这里把设备侧也打回 DATA0，两边对齐 */
    if (s_connected) {
        bool a = clear_ep_halt(s_ep);
        bool b = clear_ep_halt(s_ep_in);
        ESP_LOGI(TAG, "toggle 归零 出=%s 入=%s", a ? "OK" : "失败", b ? "OK" : "失败");
    }

    vTaskDelay(pdMS_TO_TICKS(300));
    s_in_paused = false;
}

void usb_printer_start(void)
{
    s_evt_q     = xQueueCreate(8, sizeof(uint8_t));
    s_xfer_done = xSemaphoreCreateBinary();
    s_job_mutex = xSemaphoreCreateMutex();

    const usb_host_config_t hc = { .intr_flags = ESP_INTR_FLAG_LEVEL1 };
    ESP_ERROR_CHECK(usb_host_install(&hc));
    const usb_host_client_config_t cc = {
        .is_synchronous = false, .max_num_event_msg = 5,
        .async = { .client_event_callback = client_cb },
    };
    ESP_ERROR_CHECK(usb_host_client_register(&cc, &s_client));
    ESP_ERROR_CHECK(usb_host_transfer_alloc(CHUNK_SIZE, 0, &s_xfer));

    xTaskCreate(lib_task,  "usb_lib",  4096, NULL, 5, NULL);
    xTaskCreate(cli_task,  "usb_cli",  4096, NULL, 5, NULL);
    xTaskCreate(enum_task, "usb_enum", 4096, NULL, 4, NULL);
    s_in_done = xSemaphoreCreateBinary();
    s_ctrl_done = xSemaphoreCreateBinary();
    s_probe_mux = xSemaphoreCreateMutex();
    /* 栈从 4096 提到 6144：收包缓冲 txt[IN_READ_MAX+1] 是栈上的，
     * 加上 ESP_LOGW 格式化本身的开销，4096 太紧。 */
    xTaskCreate(status_reader_task, "usb_stat", 6144, NULL, 3, NULL);
}

bool usb_printer_connected(void){ return s_connected; }

esp_err_t usb_printer_job_begin(void)
{
    if (!s_connected) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_job_mutex, portMAX_DELAY);
    if (!s_connected) { xSemaphoreGive(s_job_mutex); return ESP_ERR_INVALID_STATE; }

    /* 第一份作业跳过接口复位：设备刚枚举完，没有上一份需要收尾。 */
    bool first = (s_job_count++ == 0);
    run_hook(PROF_HOOK_JOB_BEGIN, first);

    /* 唤醒：打印机收到数据就会醒（实测发指令后它真的动了）。
     * 注意：GET_PORT_STATUS 测不出休眠——休眠时它照样报 0x18(选中+无错)，
     * 所以别再写「等状态位变化」的循环，那是空等。档案里的 wake 钩子干这事：
     * 发个无害的 UEL 敲门 + 固定延时。 */
    if (s_connected && s_script.hook[PROF_HOOK_WAKE].n) {
        joblog_phase(JOB_WAKING, 0, 0);
        run_hook(PROF_HOOK_WAKE, first);
        lcd_ui_prn("就绪");
    }

    s_job_bytes = 0;
    s_job_crc = 0;
    return ESP_OK;
}

esp_err_t usb_printer_job_write(const uint8_t *data, size_t len)
{
    static size_t last_mark;
    while (len) {
        size_t n = len > CHUNK_SIZE ? CHUNK_SIZE : len;
        if (!s_connected) return ESP_ERR_INVALID_STATE;
        s_job_crc = esp_rom_crc32_le(s_job_crc, data, n);
        memcpy(s_xfer->data_buffer, data, n);
        s_xfer->device_handle = s_dev;
        s_xfer->bEndpointAddress = s_ep;
        s_xfer->callback = xfer_cb;
        s_xfer->num_bytes = n;
        s_xfer->timeout_ms = 30000;
        esp_err_t r = usb_host_transfer_submit(s_xfer);
        if (r != ESP_OK) return r;
        if (xSemaphoreTake(s_xfer_done, pdMS_TO_TICKS(20000)) != pdTRUE) {
            /* 打印机 20 秒不收数据：halt+flush 端点自愈，避免永久卡死 */
            ESP_LOGE(TAG, "bulk 写 20s 无进展，复位端点");
            usb_host_endpoint_halt(s_dev, s_ep);
            usb_host_endpoint_flush(s_dev, s_ep);
            usb_host_endpoint_clear(s_dev, s_ep);
            xSemaphoreTake(s_xfer_done, pdMS_TO_TICKS(2000));
            return ESP_ERR_TIMEOUT;
        }
        if (s_status != USB_TRANSFER_STATUS_COMPLETED) return ESP_FAIL;
        data += s_xfer->actual_num_bytes;
        len  -= s_xfer->actual_num_bytes;
        s_job_bytes += s_xfer->actual_num_bytes;
        if ((s_job_bytes >> 16) != (last_mark >> 16)) {      /* 每 64KB 落一次盘 */
            last_mark = s_job_bytes;
            joblog_phase(JOB_SENDING, s_job_bytes, 0);
        }
    }
    return ESP_OK;
}

/* 客户端取消作业时调用：给打印机发 UEL，让它别再等后续数据。
 * 不抢作业锁——正在传的作业自己会走 job_end；这里只处理「已经卡住」的情况。 */
void usb_printer_abort(void)
{
    if (!s_connected) return;
    if (xSemaphoreTake(s_job_mutex, pdMS_TO_TICKS(100)) != pdTRUE) return;
    size_t saved = s_job_bytes;
    usb_printer_job_write((const uint8_t *)"\x1b%-12345X", 9);
    s_job_bytes = saved;
    joblog_phase(JOB_CANCELED, s_job_bytes, s_job_bytes);
    ESP_LOGI(TAG, "已向打印机转发取消(UEL)");
    xSemaphoreGive(s_job_mutex);
}

void usb_printer_job_end(void)
{
    /* 作业结束信号：USB 短包只表示「传输结束」，不表示「作业结束」。
     * 打印机会一直等后续数据（现象：必须手动按取消键才能打下一份）。
     * HP/三星系的作业分隔符是 UEL —— 驱动都会在作业末尾发它。 */
    if (s_connected && s_job_bytes > 0) {
        joblog_phase(JOB_UEL, s_job_bytes, s_job_bytes);
        run_hook(PROF_HOOK_JOB_END, false);
        s_job_done_us = esp_timer_get_time();
    }
    /* 作业结束后绝不碰端点：此刻最后一个短包可能还没物理发完，
     * halt/flush 会把尾巴掐掉（症状：打印机报 Decoding Fail，位置在流末尾）。
     * 接口重置统一放在下一份作业开始前做。 */
    joblog_phase(JOB_DONE, s_job_bytes, s_job_bytes);
    oneshot_clear();        /* 规则 9：作业一结束就销毁，成功失败都一样 */
    ESP_LOGI(TAG, "作业结束，共 %u 字节 CRC32=%08x", (unsigned)s_job_bytes, (unsigned)s_job_crc);
    {
        char t[40];
        if (s_job_bytes >= 1024 * 1024)
            snprintf(t, sizeof t, "%.1f MB 已送出", (double)s_job_bytes / (1024 * 1024));
        else
            snprintf(t, sizeof t, "%.0f KB 已送出", (double)s_job_bytes / 1024);
        lcd_ui_job(t);
    }
    xSemaphoreGive(s_job_mutex);
}
