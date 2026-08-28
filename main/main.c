/*
 * ESP32 AirPrint 桥：Wi-Fi station + mDNS(_ipp._tcp,_universal) + IPP 服务器
 *                    + USB host 打印机透传。文档流不落地、不渲染。
 */
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "driver/gpio.h"
#include "mdns.h"
#include "usb_printer.h"
#include "wifi_creds.h"

static const char *TAG = "bridge";

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
    snprintf(uuid, sizeof uuid, "e5p32b71-d6e0-4917-%02x%02x-%02x%02x%02x%02x0001",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    ESP_ERROR_CHECK(mdns_init());
    ESP_ERROR_CHECK(mdns_hostname_set("hp136a-bridge"));
    ESP_ERROR_CHECK(mdns_instance_name_set("HP Laser MFP 136a"));
    mdns_txt_item_t txt[] = {
        {"txtvers",  "1"},
        {"qtotal",   "1"},
        {"rp",       "ipp/print"},
        {"ty",       "HP Laser MFP 136a (ESP32 bridge)"},
        {"product",  "(HP Laser MFP 136a)"},
        {"note",     "USB bridge"},
        {"pdl",      "image/urf,image/pwg-raster"},
        {"URF",      "V1.4,W8,SRGB24,CP1,IS1,OB10,PQ4,RS300,DM1"},
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
    ESP_ERROR_CHECK(mdns_service_add("HP Laser MFP 136a", "_ipp", "_tcp", 631,
                                     txt, sizeof(txt)/sizeof(txt[0])));
    ESP_ERROR_CHECK(mdns_service_subtype_add_for_host("HP Laser MFP 136a",
                                     "_ipp", "_tcp", NULL, "_universal"));
    ESP_LOGI(TAG, "mDNS 已广播 _ipp._tcp + _universal 子类型");
}

static void on_wifi(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGW(TAG, "Wi-Fi 断开，2 秒后重连");
        vTaskDelay(pdMS_TO_TICKS(2000));
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = data;
        char ip[16];
        snprintf(ip, sizeof ip, IPSTR, IP2STR(&e->ip_info.ip));
        ESP_LOGI(TAG, "拿到 IP: %s", ip);
        static bool started = false;
        if (!started) {
            started = true;
            extern void netlog_start(void);
            netlog_start();
            uint8_t mac[6];
            esp_wifi_get_mac(WIFI_IF_STA, mac);
            start_mdns(mac);
            ipp_server_start(ip, mac);
        }
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "=== ESP32 AirPrint 桥 ===");
    esp_err_t r = nvs_flash_init();
    if (r == ESP_ERR_NVS_NO_FREE_PAGES || r == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }

    /* 调试窗口：切 host 前 DEV 口日志仍可见 */
    for (int i = 3; i > 0; i--) {
        ESP_LOGI(TAG, "%d 秒后切 USB host…", i);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    board_usb_host_power();
    usb_printer_start();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t wc = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&wc));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_wifi, NULL));

    wifi_config_t sta = { 0 };
    strlcpy((char *)sta.sta.ssid, WIFI_SSID, sizeof(sta.sta.ssid));
    strlcpy((char *)sta.sta.password, WIFI_PASS, sizeof(sta.sta.password));
    sta.sta.threshold.authmode = WIFI_AUTH_WPA_PSK;
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));   /* 省电模式会吃掉组播查询，mDNS 必关 */
    ESP_LOGI(TAG, "连接 Wi-Fi \"%s\"…", WIFI_SSID);
}
