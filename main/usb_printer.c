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
#include "esp_rom_crc.h"

static const char *TAG = "usb_prn";

#define PIN_LED_GREEN   GPIO_NUM_15
#define CHUNK_SIZE      8192

static usb_host_client_handle_t s_client;
static usb_device_handle_t      s_dev;
static uint8_t                  s_itf, s_ep;
static uint16_t                 s_mps;
static volatile bool            s_connected;
static QueueHandle_t            s_evt_q;
static SemaphoreHandle_t        s_xfer_done, s_job_mutex;
static usb_transfer_t          *s_xfer;
static volatile usb_transfer_status_t s_status;
static size_t                   s_job_bytes;
static uint32_t                 s_job_crc;

static void xfer_cb(usb_transfer_t *t){ s_status = t->status; xSemaphoreGive(s_xfer_done); }

static void client_cb(const usb_host_client_event_msg_t *m, void *arg)
{
    uint8_t v;
    if (m->event == USB_HOST_CLIENT_EVENT_NEW_DEV) { v = m->new_dev.address; xQueueSend(s_evt_q, &v, 0); }
    else if (m->event == USB_HOST_CLIENT_EVENT_DEV_GONE) { v = 0; xQueueSend(s_evt_q, &v, 0); }
}

static bool find_bulk_out(const usb_config_desc_t *cfg, uint8_t *itf, uint8_t *alt, uint8_t *ep, uint16_t *mps)
{
    int off = 0; bool in_prn = false; uint8_t ci = 0, ca = 0;
    const usb_standard_desc_t *d = (const usb_standard_desc_t *)cfg;
    while ((d = usb_parse_next_descriptor(d, cfg->wTotalLength, &off)) != NULL) {
        if (d->bDescriptorType == USB_B_DESCRIPTOR_TYPE_INTERFACE) {
            const usb_intf_desc_t *i = (const usb_intf_desc_t *)d;
            ci = i->bInterfaceNumber; ca = i->bAlternateSetting;
            in_prn = (i->bInterfaceClass == 0x07);
        } else if (d->bDescriptorType == USB_B_DESCRIPTOR_TYPE_ENDPOINT && in_prn) {
            const usb_ep_desc_t *e = (const usb_ep_desc_t *)d;
            if ((e->bmAttributes & USB_BM_ATTRIBUTES_XFERTYPE_MASK) == USB_BM_ATTRIBUTES_XFER_BULK &&
                !(e->bEndpointAddress & 0x80)) {
                *itf = ci; *alt = ca; *ep = e->bEndpointAddress; *mps = e->wMaxPacketSize;
                return true;
            }
        }
    }
    return false;
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
        const usb_config_desc_t *cfg; uint8_t itf, alt, ep; uint16_t mps;
        if (usb_host_get_active_config_descriptor(dev, &cfg) != ESP_OK ||
            !find_bulk_out(cfg, &itf, &alt, &ep, &mps) ||
            usb_host_interface_claim(s_client, dev, itf, alt) != ESP_OK) {
            ESP_LOGW(TAG, "设备 %u 不是可用打印机", addr);
            usb_host_device_close(s_client, dev);
            continue;
        }
        s_dev = dev; s_itf = itf; s_ep = ep; s_mps = mps; s_connected = true;
        gpio_set_level(PIN_LED_GREEN, 1);
        const usb_device_desc_t *dd;
        usb_host_get_device_descriptor(dev, &dd);
        ESP_LOGI(TAG, "打印机就绪 VID=0x%04X PID=0x%04X ep=0x%02X mps=%u",
                 dd->idVendor, dd->idProduct, ep, mps);
        lcd_ui_prn("HP 136a 已连接");
    }
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
}

bool usb_printer_connected(void){ return s_connected; }

esp_err_t usb_printer_job_begin(void)
{
    if (!s_connected) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_job_mutex, portMAX_DELAY);
    if (!s_connected) { xSemaphoreGive(s_job_mutex); return ESP_ERR_INVALID_STATE; }
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
    /* 末包恰为 MPS 整数倍时补 ZLP 结束作业 */
    if (s_connected && s_mps && s_job_bytes && (s_job_bytes % s_mps) == 0) {
        s_xfer->device_handle = s_dev;
        s_xfer->bEndpointAddress = s_ep;
        s_xfer->callback = xfer_cb;
        s_xfer->num_bytes = 0;
        s_xfer->timeout_ms = 5000;
        if (usb_host_transfer_submit(s_xfer) == ESP_OK)
            xSemaphoreTake(s_xfer_done, portMAX_DELAY);
    }
    ESP_LOGI(TAG, "作业结束，共 %u 字节 CRC32=%08x", (unsigned)s_job_bytes, (unsigned)s_job_crc);
    {
        char t[40];
        snprintf(t, sizeof t, "%u 字节 已送打印机", (unsigned)s_job_bytes);
        lcd_ui_job(t);
    }
    xSemaphoreGive(s_job_mutex);
}
