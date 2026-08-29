/*
 * ESP32 AirPrint 桥：Wi-Fi station + mDNS(_ipp._tcp,_universal) + IPP 服务器
 *                    + USB host 打印机透传。文档流不落地、不渲染。
 */
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "driver/gpio.h"
#include "mdns.h"
#include "usb_printer.h"
#include "joblog.h"
#include "provision.h"
#include "usb_printer.h"
#include "joblog.h"
#include "lcd_ui.h"
#include "driver/gpio.h"
#include "esp_system.h"

static const char *TAG = "bridge";
static EventGroupHandle_t s_evt;
static char s_ip[16];
static char s_ssid[33], s_pass[65];
#define EVT_GOT_IP BIT0

static void start_mdns(const uint8_t mac[6]);
void ipp_server_start(const char *ip, const uint8_t mac[6]);

/* mdns/ipp 初始化不能在事件任务里做（栈只有 3.5KB，会爆栈 panic），单开任务 */
static void services_task(void *arg)
{
    xEventGroupWaitBits(s_evt, EVT_GOT_IP, pdFALSE, pdFALSE, portMAX_DELAY);
    extern void netlog_start(void);
    netlog_start();
    uint8_t mac[6];
    esp_wifi_get_mac(WIFI_IF_STA, mac);

    /* IPP 服务先起，但 mDNS 广播要等打印机真的就绪——否则客户端会在
     * 桥还没准备好时就来试作业，被拒一次就把这台打印机拉黑。 */
    ipp_server_start(s_ip, mac);
    for (int i = 0; i < 60 && !usb_printer_connected(); i++)
        vTaskDelay(pdMS_TO_TICKS(500));
    ESP_LOGI(TAG, "打印机%s，开始广播", usb_printer_connected() ? "已就绪" : "未就绪(超时)");
    start_mdns(mac);
    joblog_boot_report();
    ESP_LOGI(TAG, "网络服务全部就绪");
    lcd_ui_log("mDNS+IPP 服务已就绪");
    vTaskDelete(NULL);
}

#define PIN_USB_MODE_SEL      GPIO_NUM_18
#define PIN_USB_LIMIT_EN      GPIO_NUM_17
#define PIN_USB_DEV_VBUS_EN   GPIO_NUM_12
#define PIN_BATTERY_BOOST_EN  GPIO_NUM_13
#define PIN_LED_GREEN         GPIO_NUM_15
#define PIN_LED_YELLOW        GPIO_NUM_16

void ipp_server_start(const char *ip, const uint8_t mac[6]);

static void board_usb_host_power(void)
{
    const gpio_config_t io = {
        .pin_bit_mask = BIT64(PIN_USB_MODE_SEL) | BIT64(PIN_USB_LIMIT_EN) |
                        BIT64(PIN_USB_DEV_VBUS_EN) | BIT64(PIN_BATTERY_BOOST_EN) |
                        BIT64(PIN_LED_GREEN) | BIT64(PIN_LED_YELLOW),
        .mode = GPIO_MODE_OUTPUT,
    };
    ESP_ERROR_CHECK(gpio_config(&io));
    gpio_set_level(PIN_LED_GREEN, 0);
    gpio_set_level(PIN_LED_YELLOW, 0);
    gpio_set_level(PIN_BATTERY_BOOST_EN, 0);
    gpio_set_level(PIN_USB_DEV_VBUS_EN, 1);
    gpio_set_level(PIN_USB_LIMIT_EN, 1);
    vTaskDelay(pdMS_TO_TICKS(50));
    gpio_set_level(PIN_USB_MODE_SEL, 1);
    ESP_LOGI(TAG, "USB 已切 host 模式");
}

static void start_mdns(const uint8_t mac[6])
{
    char uuid[48];
    /* 必须合法十六进制，且与 IPP 的 printer-uuid 完全一致 */
    /* 换了 UUID 前缀：iOS 按 UUID 认打印机，改掉才能绕开它缓存里的旧记录 */
    snprintf(uuid, sizeof uuid, "a7d41f60-9c2b-4e83-b1%02x-%02x%02x%02x%02x%02x%02x",
             mac[0], mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    esp_err_t err = mdns_init();
    if (err != ESP_OK) { ESP_LOGE(TAG, "mdns_init: %s", esp_err_to_name(err)); return; }
    mdns_hostname_set("hp136a-bridge");
    mdns_instance_name_set("HP 136a Bridge");
    mdns_txt_item_t txt[] = {
        {"txtvers",  "1"},
        {"qtotal",   "1"},
        {"rp",       "ipp/print"},
        {"ty",       "HP 136a Bridge"},
        {"product",  "(HP Laser MFP 136a)"},
        {"note",     "USB bridge"},
        {"adminurl", "http://hp136a-bridge.local./"},
        {"pdl",      "image/urf"},
        {"URF",      "V1.4,W8,CP1,IS1,OB10,PQ4,RS300,DM1"},
        {"Color",    "F"},
        {"Duplex",   "F"},
        {"usb_MFG",  "HP"},
        {"usb_MDL",  "HP Laser MFP 136a"},
        {"UUID",     uuid},
        {"priority", "30"},
        {"kind",     "document"},
        {"PaperMax", "legal-A4"},
        {"air",      "none"},
    };
    err = mdns_service_add("HP 136a Bridge", "_ipp", "_tcp", 631,
                           txt, sizeof(txt)/sizeof(txt[0]));
    if (err != ESP_OK) ESP_LOGE(TAG, "service_add: %s", esp_err_to_name(err));
    err = mdns_service_subtype_add_for_host("HP 136a Bridge",
                           "_ipp", "_tcp", NULL, "_universal");
    if (err != ESP_OK) ESP_LOGE(TAG, "subtype_add: %s", esp_err_to_name(err));
    ESP_LOGI(TAG, "mDNS 已广播 _ipp._tcp + _universal 子类型");
}

static void on_wifi(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *d = data;
        ESP_LOGW(TAG, "Wi-Fi 断开 reason=%d rssi=%d，重连", d->reason, d->rssi);
        lcd_ui_wifi("重连中");
        /* 从未连上过且连错 12 次：多半是换了路由器或改了密码，回配网模式 */
        static int fails;
        if (!s_ip[0] && ++fails >= 12) {
            ESP_LOGE(TAG, "始终连不上，清配置重启进配网");
            prov_erase();
            esp_restart();
        }
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = data;
        snprintf(s_ip, sizeof s_ip, IPSTR, IP2STR(&e->ip_info.ip));
        ESP_LOGI(TAG, "拿到 IP: %s", s_ip);
        lcd_ui_wifi(s_ip);
        xEventGroupSetBits(s_evt, EVT_GOT_IP);
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "=== ESP32 AirPrint 桥 ===");
    ESP_LOGI(TAG, "堆@启动=%u", (unsigned)esp_get_free_heap_size());
    lcd_ui_init();
    ESP_LOGI(TAG, "堆@LCD后=%u", (unsigned)esp_get_free_heap_size());
    s_evt = xEventGroupCreate();
    esp_err_t r = nvs_flash_init();
    if (r == ESP_ERR_NVS_NO_FREE_PAGES || r == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }

    /* 开机按住 MENU 键 = 清空 Wi-Fi 配置重新配网 */
    gpio_config_t btn = { .pin_bit_mask = BIT64(GPIO_NUM_14),
                          .mode = GPIO_MODE_INPUT, .pull_up_en = GPIO_PULLUP_ENABLE };
    gpio_config(&btn);
    vTaskDelay(pdMS_TO_TICKS(50));
    if (gpio_get_level(GPIO_NUM_14) == 0) {
        ESP_LOGW(TAG, "检测到 MENU 键按下——清空 Wi-Fi 配置");
        lcd_ui_wifi("已清空配置");
        prov_erase();
        vTaskDelay(pdMS_TO_TICKS(1500));
    }

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    /* 没有保存过凭据就先进配网门户（内部配好会自动重启，不会返回） */
    if (!prov_load(s_ssid, sizeof s_ssid, s_pass, sizeof s_pass)) {
        ESP_LOGW(TAG, "无 Wi-Fi 配置，进入配网模式");
        prov_portal_run();
    }
    ESP_LOGI(TAG, "已读取配置：SSID=\"%s\"", s_ssid);

    esp_netif_create_default_wifi_sta();
    wifi_init_config_t wc = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&wc));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_wifi, NULL));

    wifi_config_t sta = { 0 };
    strlcpy((char *)sta.sta.ssid, s_ssid, sizeof(sta.sta.ssid));
    strlcpy((char *)sta.sta.password, s_pass, sizeof(sta.sta.password));
    sta.sta.threshold.authmode = WIFI_AUTH_WPA_PSK;
    sta.sta.pmf_cfg.capable = true;
    sta.sta.pmf_cfg.required = false;
    sta.sta.ft_enabled = false;      /* AP 开了 802.11r，显式不参与 FT */
    sta.sta.btm_enabled = false;
    sta.sta.rm_enabled = false;
    sta.sta.owe_enabled = false;
    sta.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    sta.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;   /* 多节点时挑信号最好的 */
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta));
    ESP_ERROR_CHECK(esp_wifi_start());
    esp_wifi_set_max_tx_power(52);

    /* 一次性诊断：把叫这个名字的所有 BSS 打出来 */
    wifi_scan_config_t sc = { .ssid = (uint8_t *)s_ssid };
    if (esp_wifi_scan_start(&sc, true) == ESP_OK) {
        uint16_t n = 10;
        wifi_ap_record_t recs[10];
        esp_wifi_scan_get_ap_records(&n, recs);
        for (int i = 0; i < n; i++)
            ESP_LOGI(TAG, "BSS %02x:%02x:%02x:%02x:%02x:%02x ch=%d rssi=%d auth=%d 11r=%d",
                     recs[i].bssid[0], recs[i].bssid[1], recs[i].bssid[2],
                     recs[i].bssid[3], recs[i].bssid[4], recs[i].bssid[5],
                     recs[i].primary, recs[i].rssi, recs[i].authmode, recs[i].ftm_responder);
    }
    ESP_LOGI(TAG, "连接 Wi-Fi \"%s\"…", s_ssid);
    xTaskCreate(services_task, "svc", 8192, NULL, 4, NULL);

    EventBits_t bits = xEventGroupWaitBits(s_evt, EVT_GOT_IP, pdFALSE, pdFALSE,
                                           pdMS_TO_TICKS(30000));
    ESP_LOGI(TAG, "Wi-Fi 阶段结果: %s", (bits & EVT_GOT_IP) ? "已联网" : "30 秒超时，继续启动");

#if CONFIG_BRIDGE_DIAG_NO_USB
    ESP_LOGI(TAG, "诊断模式：不切 USB host，串口常驻，观察 Wi-Fi");
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(10000));
        wifi_ap_record_t ap;
        if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK)
            ESP_LOGI(TAG, "在线 ch=%d rssi=%d", ap.primary, ap.rssi);
        else
            ESP_LOGW(TAG, "仍未联网");
    }
#else
    ESP_LOGI(TAG, "3 秒后切 USB host（DEV 口即将消失）…");
    vTaskDelay(pdMS_TO_TICKS(3000));
    board_usb_host_power();
    usb_printer_start();
    ESP_LOGI(TAG, "桥就绪，堆=%u", (unsigned)esp_get_free_heap_size());
#endif
}
