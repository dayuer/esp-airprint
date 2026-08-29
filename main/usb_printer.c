/* USB host 打印机类驱动：枚举 -> claim 7/x/x 接口 -> 提供 bulk OUT 写通道 */
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include "usb/usb_host.h"
#include "usb_printer.h"
#include "lcd_ui.h"
#include "driver/gpio.h"
#include "esp_rom_crc.h"

static const char *TAG = "usb_prn";

#define PIN_LED_GREEN   GPIO_NUM_15
#define CHUNK_SIZE      8192

static usb_host_client_handle_t s_client;
static usb_device_handle_t      s_dev;
static uint8_t                  s_itf, s_ep, s_ep_in;
static uint16_t                 s_mps, s_mps_in;
static volatile bool            s_connected;
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
        s_dev = dev; s_itf = itf; s_ep = ep; s_mps = mps;
        s_ep_in = ep_in; s_mps_in = mps_in;
        s_connected = true;
        gpio_set_level(PIN_LED_GREEN, 1);
        const usb_device_desc_t *dd;
        usb_host_get_device_descriptor(dev, &dd);
        ESP_LOGI(TAG, "打印机就绪 VID=0x%04X PID=0x%04X 出=0x%02X/%u 入=0x%02X/%u",
                 dd->idVendor, dd->idProduct, ep, mps, ep_in, mps_in);
        lcd_ui_prn("HP 136a 已连接");
    }
}

/* 打印机是双向接口(7/1/2)：不排空 bulk IN，它的回传缓冲满了就整台停摆。
 * 顺带把回传内容打出来——打印机的报错就写在里面。 */
static SemaphoreHandle_t s_in_done;
static volatile bool s_in_paused, s_in_idle;
static volatile usb_transfer_status_t s_in_status;
static void in_cb(usb_transfer_t *t){ s_in_status = t->status; xSemaphoreGive(s_in_done); }

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
        x->num_bytes = (s_mps_in && (512 % s_mps_in) == 0) ? 512 : s_mps_in;
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
            char txt[129];
            int k = 0;
            for (int i = 0; i < n && k < 128; i++) {
                uint8_t c = x->data_buffer[i];
                txt[k++] = (c >= 0x20 && c < 0x7f) ? c : '.';
            }
            txt[k] = 0;
            ESP_LOGW(TAG, "打印机回传 %d 字节: %s", n, txt);
        }
        vTaskDelay(pdMS_TO_TICKS(150));
    }
}

/* 打印机类控制请求 */
static SemaphoreHandle_t s_ctrl_done;
static void ctrl_cb(usb_transfer_t *t){ xSemaphoreGive(s_ctrl_done); }

static void printer_port_status(void)
{
    if (!s_connected) return;
    usb_transfer_t *x;
    if (usb_host_transfer_alloc(64, 0, &x) != ESP_OK) return;
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
        uint8_t st = x->data_buffer[sizeof(usb_setup_packet_t)];
        ESP_LOGI(TAG, "端口状态 0x%02X  纸尽=%d 选中=%d 无错=%d",
                 st, !!(st & 0x20), !!(st & 0x10), !!(st & 0x08));
    } else {
        ESP_LOGW(TAG, "端口状态读取失败");
    }
    usb_host_transfer_free(x);
}

/* 打印机类 SOFT_RESET(bRequest=2)：把设备侧 buffer 和 bulk toggle 打回默认。
 * CUPS 对全体三星引擎（含 HP Laser 13x）强制每作业后执行，收件人先用
 * RECIPIENT_OTHER，STALL 了再退回 RECIPIENT_INTERFACE。 */
static bool soft_reset_once(uint8_t bmRequestType)
{
    usb_transfer_t *x;
    if (usb_host_transfer_alloc(64, 0, &x) != ESP_OK) return false;
    usb_setup_packet_t *sp = (usb_setup_packet_t *)x->data_buffer;
    sp->bmRequestType = bmRequestType;
    sp->bRequest      = 2;             /* SOFT_RESET */
    sp->wValue        = 0;
    sp->wIndex        = s_itf;
    sp->wLength       = 0;
    x->device_handle    = s_dev;
    x->bEndpointAddress = 0;
    x->callback         = ctrl_cb;
    x->num_bytes        = sizeof(usb_setup_packet_t);
    x->timeout_ms       = 5000;
    bool ok = false;
    if (usb_host_transfer_submit_control(s_client, x) == ESP_OK &&
        xSemaphoreTake(s_ctrl_done, pdMS_TO_TICKS(5500)) == pdTRUE) {
        ok = (x->status == USB_TRANSFER_STATUS_COMPLETED);
    }
    usb_host_transfer_free(x);
    return ok;
}

static void printer_soft_reset(void)
{
    if (!s_connected) return;
    if (soft_reset_once(0x23)) { ESP_LOGI(TAG, "软复位成功 (RECIP_OTHER)"); return; }
    if (soft_reset_once(0x21)) { ESP_LOGI(TAG, "软复位成功 (RECIP_INTERFACE)"); return; }
    ESP_LOGW(TAG, "软复位被拒（该机型不支持，跳过）");
}

/* 接口重置：清空在途传输 + release/claim。
 * 必须在「下一份作业开始前」做，不能在上一份写完就做——那时打印机还在渲染，
 * 半路抽掉接口会把正在打的页面弄丢。 */
/* 标准请求 CLEAR_FEATURE(ENDPOINT_HALT)：USB 2.0 §9.4.5 规定它会把
 * **设备侧**该端点的 data toggle 复位到 DATA0。这是唯一能让设备跟着
 * 主机一起归零的手段——ESP-IDF 的 claim 只动主机侧，单独用会制造失配，
 * 表现就是每份作业开头 64 字节被设备当重传丢弃 => 打出空白页。 */
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

/* 已确认：只有「开机后第一份」能可靠打印。release/claim + CLEAR_FEATURE 都不足以
 * 把打印机恢复到刚枚举完的状态。这里用最彻底的办法——断掉 host 口 VBUS，
 * 逼它重新枚举，等于给每份作业一个真正的「第一份」。 */
#define PIN_USB_DEV_VBUS_EN GPIO_NUM_12

static void usb_power_cycle(void)
{
    ESP_LOGW(TAG, "VBUS 断电重枚举…");
    s_in_paused = true;
    for (int i = 0; i < 100 && !s_in_idle; i++) vTaskDelay(pdMS_TO_TICKS(20));

    if (s_dev) {
        usb_host_interface_release(s_client, s_dev, s_itf);
        usb_host_device_close(s_client, s_dev);
        s_dev = NULL;
    }
    s_connected = false;
    gpio_set_level(PIN_LED_GREEN, 0);

    gpio_set_level(PIN_USB_DEV_VBUS_EN, 0);     /* 断电 */
    vTaskDelay(pdMS_TO_TICKS(1500));
    gpio_set_level(PIN_USB_DEV_VBUS_EN, 1);     /* 上电，enum_task 会重新认到 */
    s_in_paused = false;

    for (int i = 0; i < 400 && !s_connected; i++) vTaskDelay(pdMS_TO_TICKS(25));
    ESP_LOGW(TAG, "重枚举%s（耗时内）", s_connected ? "成功" : "超时失败");
    vTaskDelay(pdMS_TO_TICKS(500));
}

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
    xTaskCreate(status_reader_task, "usb_stat", 4096, NULL, 3, NULL);
}

bool usb_printer_connected(void){ return s_connected; }

esp_err_t usb_printer_job_begin(void)
{
    if (!s_connected) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_job_mutex, portMAX_DELAY);
    if (!s_connected) { xSemaphoreGive(s_job_mutex); return ESP_ERR_INVALID_STATE; }

    /* 第二份及以后：先给上一份留出打印时间，再重置接口开新作业 */
    if (s_job_count++ > 0) {
        printer_port_status();
        interface_cycle();
    }

    s_job_bytes = 0;
    s_job_crc = 0;
    return ESP_OK;
}

esp_err_t usb_printer_job_write(const uint8_t *data, size_t len)
{
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
    }
    return ESP_OK;
}

void usb_printer_job_end(void)
{
    /* 作业结束信号：USB 短包只表示「传输结束」，不表示「作业结束」。
     * 打印机会一直等后续数据（现象：必须手动按取消键才能打下一份）。
     * HP/三星系的作业分隔符是 UEL —— 驱动都会在作业末尾发它。 */
    if (s_connected && s_job_bytes > 0) {
        static const uint8_t uel[] = "\x1b%-12345X";   /* 9 字节，不含结尾 NUL */
        size_t saved = s_job_bytes;
        if (usb_printer_job_write(uel, sizeof(uel) - 1) == ESP_OK)
            ESP_LOGI(TAG, "已发送 UEL 作业结束符");
        else
            ESP_LOGW(TAG, "UEL 发送失败");
        s_job_bytes = saved;      /* UEL 不计入作业字节数，便于与源文件核对 */
    }
    /* 作业结束后绝不碰端点：此刻最后一个短包可能还没物理发完，
     * halt/flush 会把尾巴掐掉（症状：打印机报 Decoding Fail，位置在流末尾）。
     * 接口重置统一放在下一份作业开始前做。 */
    ESP_LOGI(TAG, "作业结束，共 %u 字节 CRC32=%08x", (unsigned)s_job_bytes, (unsigned)s_job_crc);
    {
        char t[40];
        snprintf(t, sizeof t, "%u 字节 已送打印机", (unsigned)s_job_bytes);
        lcd_ui_job(t);
    }
    xSemaphoreGive(s_job_mutex);
}
