/*
 * Wi-Fi 配网门户：首次启动（或凭据失效）时把自己变成开放热点，
 * 手机连上后自动弹出网页 —— 扫描周边 Wi-Fi、填密码、当场验证、
 * 验证通过写入 NVS 并重启接入网络。
 */
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_mac.h"
#include "esp_http_server.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "lwip/sockets.h"
#include "provision.h"
#include "portal_html.h"
#include "lcd_ui.h"

static const char *TAG = "prov";
#define NVS_NS "wifi"
#define NVS_CLOUD "cloud"      /* 设备密钥，见 API 文档 1.3 */

/* 连接测试状态：0=进行中 1=成功 2=失败 */
static volatile int  s_test_state;
static char          s_test_err[48];
static char          s_test_ip[16];
static char          s_try_ssid[33], s_try_pass[65];
static char          s_try_key[80];      /* 令牌 45 字符，留足余量 */
static EventGroupHandle_t s_eg;
#define EV_OK   BIT0
#define EV_FAIL BIT1

/* ------------------------------------------------------------ NVS */

bool prov_load(char *ssid, size_t sl, char *pass, size_t pl)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return false;
    size_t n = sl;
    bool ok = (nvs_get_str(h, "ssid", ssid, &n) == ESP_OK) && ssid[0];
    if (ok) { n = pl; if (nvs_get_str(h, "pass", pass, &n) != ESP_OK) pass[0] = 0; }
    nvs_close(h);
    return ok;
}

bool prov_load_devkey(char *out, size_t cap)
{
    out[0] = 0;
    nvs_handle_t h;
    if (nvs_open(NVS_CLOUD, NVS_READONLY, &h) != ESP_OK) return false;
    size_t n = cap;
    bool ok = (nvs_get_str(h, "devkey", out, &n) == ESP_OK) && out[0];
    nvs_close(h);
    return ok;
}

static void prov_save(const char *ssid, const char *pass)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_str(h, "ssid", ssid);
    nvs_set_str(h, "pass", pass);
    nvs_commit(h);
    nvs_close(h);

    /* 设备密钥单独一个 namespace：Wi-Fi 可能换，密钥不该跟着丢。
     * 只在这次真填了才写——留空表示「不动已有的密钥」。 */
    if (s_try_key[0] && nvs_open(NVS_CLOUD, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_str(h, "devkey", s_try_key);
        nvs_commit(h);
        nvs_close(h);
        ESP_LOGI(TAG, "设备密钥已写入 NVS（%d 字符）", (int)strlen(s_try_key));
    }
    ESP_LOGI(TAG, "凭据已写入 NVS");
}

void prov_erase(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_CLOUD, NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_all(h); nvs_commit(h); nvs_close(h);
    }
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_erase_all(h);
    nvs_commit(h);
    nvs_close(h);
    ESP_LOGW(TAG, "已清空保存的 Wi-Fi 凭据");
}

/* --------------------------------------------------- DNS 劫持（弹窗） */

/* 把所有 A 查询都答成热点自身 IP，手机的联网检测因此失败 -> 自动弹出配网页 */
static void dns_task(void *arg)
{
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in me = { .sin_family = AF_INET, .sin_port = htons(53),
                              .sin_addr.s_addr = htonl(INADDR_ANY) };
    if (fd < 0 || bind(fd, (struct sockaddr *)&me, sizeof me) < 0) {
        ESP_LOGE(TAG, "DNS 端口绑定失败"); vTaskDelete(NULL); return;
    }
    uint8_t buf[256];
    while (1) {
        struct sockaddr_in from; socklen_t fl = sizeof from;
        int n = recvfrom(fd, buf, sizeof buf, 0, (struct sockaddr *)&from, &fl);
        if (n < 12 || n > 200) continue;
        buf[2] |= 0x80; buf[3] = 0x00;          /* 标记为响应 */
        buf[6] = 0; buf[7] = 1;                 /* 答案数 = 1 */
        buf[8] = 0; buf[9] = 0; buf[10] = 0; buf[11] = 0;
        uint8_t *p = buf + n;
        *p++ = 0xC0; *p++ = 0x0C;               /* 指针指向问题里的域名 */
        *p++ = 0; *p++ = 1;                     /* TYPE  A */
        *p++ = 0; *p++ = 1;                     /* CLASS IN */
        *p++ = 0; *p++ = 0; *p++ = 0; *p++ = 30;/* TTL 30s */
        *p++ = 0; *p++ = 4;                     /* RDLENGTH */
        *p++ = 192; *p++ = 168; *p++ = 4; *p++ = 1;
        sendto(fd, buf, p - buf, 0, (struct sockaddr *)&from, fl);
    }
}

/* ------------------------------------------------------------ HTTP */

static esp_err_t h_root(httpd_req_t *r)
{
    httpd_resp_set_type(r, "text/html; charset=utf-8");
    return httpd_resp_send(r, PORTAL_HTML, HTTPD_RESP_USE_STRLEN);
}

/* 各平台的联网检测地址一律重定向到配网页，触发「需要登录」弹窗 */
static esp_err_t h_redirect(httpd_req_t *r)
{
    httpd_resp_set_status(r, "302 Found");
    httpd_resp_set_hdr(r, "Location", "http://192.168.4.1/");
    return httpd_resp_send(r, NULL, 0);
}

static esp_err_t h_scan(httpd_req_t *r)
{
    wifi_scan_config_t sc = { .show_hidden = false };
    esp_wifi_scan_start(&sc, true);
    uint16_t n = 24;
    static wifi_ap_record_t recs[24];
    esp_wifi_scan_get_ap_records(&n, recs);

    char *out = malloc(3072);
    if (!out) return httpd_resp_send_500(r);
    int len = snprintf(out, 3072, "[");
    for (int i = 0; i < n && len < 2900; i++) {
        if (!recs[i].ssid[0]) continue;
        /* 同名只保留信号最强的那个（多 AP 环境会重复） */
        bool dup = false;
        for (int j = 0; j < i; j++)
            if (!strcmp((char *)recs[j].ssid, (char *)recs[i].ssid)) { dup = true; break; }
        if (dup) continue;
        len += snprintf(out + len, 3072 - len, "%s{\"s\":\"", len > 1 ? "," : "");
        for (const char *c = (char *)recs[i].ssid; *c && len < 2950; c++) {
            if (*c == '"' || *c == '\\') out[len++] = '\\';
            out[len++] = *c;
        }
        len += snprintf(out + len, 3072 - len, "\",\"r\":%d,\"k\":%d}",
                        recs[i].rssi, recs[i].authmode != WIFI_AUTH_OPEN);
    }
    snprintf(out + len, 3072 - len, "]");
    httpd_resp_set_type(r, "application/json");
    esp_err_t e = httpd_resp_send(r, out, HTTPD_RESP_USE_STRLEN);
    free(out);
    return e;
}

/* 极简 JSON 取值：{"s":"...","p":"..."} */
static void json_str(const char *body, const char *key, char *out, size_t cap)
{
    out[0] = 0;
    char pat[8];
    snprintf(pat, sizeof pat, "\"%s\"", key);
    const char *p = strstr(body, pat);
    if (!p) return;
    p = strchr(p + strlen(pat), '"');
    if (!p) return;
    p++;
    size_t i = 0;
    while (*p && *p != '"' && i < cap - 1) {
        if (*p == '\\' && p[1]) p++;
        out[i++] = *p++;
    }
    out[i] = 0;
}

static void try_connect_task(void *arg)
{
    wifi_config_t c = { 0 };
    strlcpy((char *)c.sta.ssid, s_try_ssid, sizeof c.sta.ssid);
    strlcpy((char *)c.sta.password, s_try_pass, sizeof c.sta.password);
    c.sta.ft_enabled = false; c.sta.btm_enabled = false; c.sta.rm_enabled = false;
    c.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    c.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;

    xEventGroupClearBits(s_eg, EV_OK | EV_FAIL);
    esp_wifi_disconnect();
    esp_wifi_set_config(WIFI_IF_STA, &c);
    esp_wifi_connect();
    ESP_LOGI(TAG, "测试连接 \"%s\"", s_try_ssid);
    lcd_ui_wifi("验证密码中");

    EventBits_t b = xEventGroupWaitBits(s_eg, EV_OK | EV_FAIL, pdTRUE, pdFALSE,
                                        pdMS_TO_TICKS(25000));
    if (b & EV_OK) {
        prov_save(s_try_ssid, s_try_pass);
        s_test_state = 1;
        ESP_LOGI(TAG, "验证通过，3 秒后重启接入网络");
        lcd_ui_wifi("配网成功，重启中");
        vTaskDelay(pdMS_TO_TICKS(3000));
        esp_restart();
    } else {
        if (!s_test_err[0]) strlcpy(s_test_err, "超时未连上", sizeof s_test_err);
        s_test_state = 2;
        ESP_LOGW(TAG, "验证失败：%s", s_test_err);
        lcd_ui_wifi("密码错误");
        esp_wifi_disconnect();
    }
    vTaskDelete(NULL);
}

static esp_err_t h_connect(httpd_req_t *r)
{
    char body[384];      /* ssid 32 + pass 64 + 密钥 45 + JSON 外壳 */
    int n = r->content_len < sizeof body - 1 ? r->content_len : (int)sizeof body - 1;
    int got = httpd_req_recv(r, body, n);
    if (got <= 0) return httpd_resp_send_500(r);
    body[got] = 0;

    json_str(body, "s", s_try_ssid, sizeof s_try_ssid);
    json_str(body, "p", s_try_pass, sizeof s_try_pass);
    json_str(body, "k", s_try_key,  sizeof s_try_key);   /* 设备密钥，可留空 */
    if (!s_try_ssid[0]) {
        httpd_resp_set_type(r, "application/json");
        return httpd_resp_send(r, "{\"ok\":0}", HTTPD_RESP_USE_STRLEN);
    }
    s_test_state = 0; s_test_err[0] = 0; s_test_ip[0] = 0;
    xTaskCreate(try_connect_task, "wifi_try", 4096, NULL, 5, NULL);

    httpd_resp_set_type(r, "application/json");
    return httpd_resp_send(r, "{\"ok\":1}", HTTPD_RESP_USE_STRLEN);
}

static esp_err_t h_status(httpd_req_t *r)
{
    char out[128];
    snprintf(out, sizeof out, "{\"st\":%d,\"ip\":\"%s\",\"e\":\"%s\"}",
             s_test_state, s_test_ip, s_test_err);
    httpd_resp_set_type(r, "application/json");
    return httpd_resp_send(r, out, HTTPD_RESP_USE_STRLEN);
}

/* ------------------------------------------------------- 事件与主流程 */

static void prov_evt(void *a, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *d = data;
        const char *why = "连接被拒";
        switch (d->reason) {
        case WIFI_REASON_NO_AP_FOUND:            why = "找不到该网络（是 5GHz？）"; break;
        case WIFI_REASON_AUTH_FAIL:
        case WIFI_REASON_HANDSHAKE_TIMEOUT:
        case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT: why = "密码错误"; break;
        case WIFI_REASON_AUTH_EXPIRE:            why = "认证超时"; break;
        }
        snprintf(s_test_err, sizeof s_test_err, "%s(%d)", why, d->reason);
        xEventGroupSetBits(s_eg, EV_FAIL);
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = data;
        snprintf(s_test_ip, sizeof s_test_ip, IPSTR, IP2STR(&e->ip_info.ip));
        xEventGroupSetBits(s_eg, EV_OK);
    }
}

void prov_portal_run(void)
{
    s_eg = xEventGroupCreate();

    esp_netif_create_default_wifi_ap();
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t ic = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&ic));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, prov_evt, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, prov_evt, NULL));

    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    wifi_config_t ap = { 0 };
    snprintf((char *)ap.ap.ssid, sizeof ap.ap.ssid, "StickBox-Setup-%02X%02X", mac[4], mac[5]);
    ap.ap.ssid_len = strlen((char *)ap.ap.ssid);
    ap.ap.authmode = WIFI_AUTH_OPEN;        /* 开放热点，免密码 */
    ap.ap.max_connection = 4;
    ap.ap.channel = 1;

    /* APSTA：开着热点的同时才能扫描和试连 */
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "配网热点已开：%s  →  http://192.168.4.1/", (char *)ap.ap.ssid);
    lcd_ui_wifi("配网模式");
    lcd_ui_log("连热点：");
    lcd_ui_log((char *)ap.ap.ssid);
    lcd_ui_log("浏览器打开 192.168.4.1");

    xTaskCreate(dns_task, "prov_dns", 3072, NULL, 4, NULL);

    httpd_handle_t srv = NULL;
    httpd_config_t hc = HTTPD_DEFAULT_CONFIG();
    hc.max_uri_handlers = 12;
    hc.lru_purge_enable = true;
    hc.stack_size = 6144;
    ESP_ERROR_CHECK(httpd_start(&srv, &hc));

    const httpd_uri_t routes[] = {
        { "/",        HTTP_GET,  h_root,     NULL },
        { "/scan",    HTTP_GET,  h_scan,     NULL },
        { "/status",  HTTP_GET,  h_status,   NULL },
        { "/connect", HTTP_POST, h_connect,  NULL },
        /* 各平台联网检测地址 —— 命中即触发配网弹窗 */
        { "/hotspot-detect.html",     HTTP_GET, h_redirect, NULL },  /* iOS / macOS */
        { "/library/test/success.html", HTTP_GET, h_redirect, NULL },/* iOS 旧版 */
        { "/generate_204",            HTTP_GET, h_redirect, NULL },  /* Android */
        { "/gen_204",                 HTTP_GET, h_redirect, NULL },
        { "/connecttest.txt",         HTTP_GET, h_redirect, NULL },  /* Windows */
        { "/ncsi.txt",                HTTP_GET, h_redirect, NULL },
    };
    for (size_t i = 0; i < sizeof routes / sizeof routes[0]; i++)
        httpd_register_uri_handler(srv, &routes[i]);

    /* 连接成功会在 try_connect_task 里 esp_restart()，这里不会返回 */
    while (1) vTaskDelay(pdMS_TO_TICKS(1000));
}
