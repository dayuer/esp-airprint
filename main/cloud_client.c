/*
 * 云端客户端：一条向外的 MQTT over TLS 长连接，取代本地的 IPP 服务器 + mDNS。
 *
 * 为什么不走 MQTT 传打印数据：一份作业 100~500KB，而本机可用堆只有几十 KB，
 * MQTT 消息必须整包进内存。所以 MQTT 只做「信令」（几十字节），
 * 文档本体走 HTTPS 流式下载 —— 边下边写 USB，TCP 自带反压，内存恒定。
 *
 *   服务器 --MQTT--> printer/{id}/job   {"id":"...","size":123456}
 *   设备   --HTTPS-> /api/job/{id}/data  流式取回 URF，直推打印机
 *   设备   --MQTT--> printer/{id}/status {"job":"...","state":"done","bytes":N}
 */
#include <string.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_system.h"
#include "mqtt_client.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "usb_printer.h"
#include "lcd_ui.h"
#include "joblog.h"
#include "cloud_client.h"

static const char *TAG = "cloud";

/* 云端地址与口令放在 cloud_creds.h（已 gitignore），
 * 照着 cloud_creds.h.example 复制一份填自己的。 */
#include "cloud_creds.h"
#define DL_CHUNK   4096

static esp_mqtt_client_handle_t s_mqtt;
static char  s_devid[24];
static char  s_topic_job[64], s_topic_status[64];
static QueueHandle_t s_jobq;          /* 收到的作业 id */

typedef struct { char id[40]; uint32_t size; } job_msg_t;

static void report(const char *job, const char *state, uint32_t bytes)
{
    usb_prn_status_t st;
    usb_printer_status(&st);
    char p[288];
    int n = snprintf(p, sizeof p,
        "{\"dev\":\"%s\",\"job\":\"%s\",\"state\":\"%s\",\"bytes\":%u,\"heap\":%u,"
        "\"prn\":{\"code\":%d,\"display\":\"%s\",\"online\":%s,\"asleep\":%s,"
        "\"paper_out\":%s,\"error\":%s}}",
        s_devid, job ? job : "", state, (unsigned)bytes,
        (unsigned)esp_get_free_heap_size(),
        st.code, st.display,
        st.online ? "true" : "false", st.asleep ? "true" : "false",
        st.paper_out ? "true" : "false", st.error ? "true" : "false");
    if (s_mqtt) esp_mqtt_client_publish(s_mqtt, s_topic_status, p, n, 1, 0);
}

/* ── 取回文档并直推打印机：全程流式，内存恒定 ── */
static void fetch_and_print(const job_msg_t *job)
{
    char url[160];
    snprintf(url, sizeof url, "%s/job/%s/data", API_BASE, job->id);
    ESP_LOGI(TAG, "取作业 %s (%u 字节)", job->id, (unsigned)job->size);
    lcd_ui_job("下载中…");

    esp_http_client_config_t cfg = {
        .url = url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 30000,
        .buffer_size = 2048,
    };
    esp_http_client_handle_t h = esp_http_client_init(&cfg);
    if (!h) { report(job->id, "error-http-init", 0); return; }

    uint32_t total = 0;
    bool ok = false;
    if (esp_http_client_open(h, 0) == ESP_OK) {
        int64_t len = esp_http_client_fetch_headers(h);
        int status = esp_http_client_get_status_code(h);
        if (status == 200) {
            if (usb_printer_job_begin() == ESP_OK) {
                lcd_ui_job("打印中…");
                uint8_t *buf = malloc(DL_CHUNK);
                ok = (buf != NULL);
                while (ok) {
                    int n = esp_http_client_read(h, (char *)buf, DL_CHUNK);
                    if (n < 0) { ok = false; break; }
                    if (n == 0) break;                    /* 下载完毕 */
                    if (usb_printer_job_write(buf, n) != ESP_OK) { ok = false; break; }
                    total += n;
                    if ((total & 0xFFFF) < DL_CHUNK)      /* 每 ~64KB 落一次盘 */
                        joblog_phase(JOB_SENDING, total, (uint32_t)len);
                }
                free(buf);
                usb_printer_job_end();                    /* 内含 UEL 作业结束符 */
            } else {
                report(job->id, "error-printer-offline", 0);
            }
        } else {
            ESP_LOGE(TAG, "下载失败 HTTP %d", status);
            report(job->id, "error-http", 0);
        }
    }
    esp_http_client_cleanup(h);

    if (ok && total) {
        ESP_LOGI(TAG, "作业 %s 完成，%u 字节", job->id, (unsigned)total);
        char t[40]; snprintf(t, sizeof t, "%u 字节 已打印", (unsigned)total);
        lcd_ui_job(t);
        report(job->id, "done", total);
    } else {
        ESP_LOGW(TAG, "作业 %s 失败（已传 %u 字节）", job->id, (unsigned)total);
        lcd_ui_job("失败");
        report(job->id, "failed", total);
    }
}

/* 打印机枚举比 MQTT 连接慢几秒，开机那次上报必然误报 no-printer。
 * 用一个看护任务盯着状态变化，变了就报，顺带当心跳。 */
static void watch_task(void *a)
{
    bool     last = false;
    bool     first = true;
    uint32_t last_seq = 0;
    int      quiet = 0;                 /* 距上次上报过了几个 2 秒 */
    while (1) {
        usb_prn_status_t st;
        usb_printer_status(&st);
        bool now = st.connected;
        bool changed = first || now != last || st.seq != last_seq;
        if (first || now != last) lcd_ui_prn(now ? "已就绪" : "未连接");

        /* 两条都要：状态一变立刻推（缺纸/开盖/休眠不能等 30 秒），
         * 没变也要按时推（服务器按最后一次心跳判活，只在变化时推
         * 会让设备在服务器眼里凭空消失）。 */
        if (changed || quiet >= 15) {
            report(NULL, now ? "ready" : "no-printer", 0);
            quiet = 0;
            last = now; last_seq = st.seq; first = false;
        } else {
            quiet++;
        }
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}

static void job_task(void *a)
{
    job_msg_t j;
    while (xQueueReceive(s_jobq, &j, portMAX_DELAY) == pdTRUE)
        fetch_and_print(&j);
}

/* ── 极简 JSON 取值，够用即可 ── */
static void jget(const char *s, const char *key, char *out, size_t cap)
{
    out[0] = 0;
    char pat[24]; snprintf(pat, sizeof pat, "\"%s\"", key);
    const char *p = strstr(s, pat);
    if (!p) return;
    p = strchr(p + strlen(pat), ':');
    if (!p) return;
    p++;
    while (*p == ' ' || *p == '"') p++;
    size_t i = 0;
    while (*p && *p != '"' && *p != ',' && *p != '}' && i < cap - 1) out[i++] = *p++;
    out[i] = 0;
}

static void on_mqtt(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    esp_mqtt_event_handle_t e = data;
    switch ((esp_mqtt_event_id_t)id) {
    case MQTT_EVENT_CONNECTED:
        ESP_LOGI(TAG, "已连接云端 %s", MQTT_URI);
        lcd_ui_wifi("云端已连接");
        esp_mqtt_client_subscribe(s_mqtt, s_topic_job, 1);
        break;
    case MQTT_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "与云端断开，自动重连中");
        lcd_ui_wifi("云端重连中");
        break;
    case MQTT_EVENT_DATA: {
        char buf[192];
        int n = e->data_len < (int)sizeof buf - 1 ? e->data_len : (int)sizeof buf - 1;
        memcpy(buf, e->data, n); buf[n] = 0;
        ESP_LOGI(TAG, "收到作业通知: %s", buf);
        job_msg_t j = { 0 };
        char sz[16];
        jget(buf, "id", j.id, sizeof j.id);
        jget(buf, "size", sz, sizeof sz);
        j.size = (uint32_t)atoi(sz);
        if (j.id[0]) {
            joblog_phase(JOB_RECEIVED, 0, j.size);
            if (xQueueSend(s_jobq, &j, 0) != pdTRUE)
                report(j.id, "busy", 0);
        }
        break;
    }
    case MQTT_EVENT_ERROR:
        ESP_LOGE(TAG, "MQTT 错误");
        break;
    default: break;
    }
}

void cloud_client_start(const uint8_t mac[6])
{
    snprintf(s_devid, sizeof s_devid, "%02x%02x%02x%02x%02x%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    snprintf(s_topic_job,    sizeof s_topic_job,    "printer/%s/job", s_devid);
    snprintf(s_topic_status, sizeof s_topic_status, "printer/%s/status", s_devid);
    ESP_LOGI(TAG, "设备 ID: %s", s_devid);

    s_jobq = xQueueCreate(2, sizeof(job_msg_t));
    xTaskCreate(job_task, "job", 6144, NULL, 5, NULL);
    xTaskCreate(watch_task, "watch", 3072, NULL, 3, NULL);

    char lwt[96];
    int lwtn = snprintf(lwt, sizeof lwt, "{\"dev\":\"%s\",\"state\":\"offline\"}", s_devid);

    const esp_mqtt_client_config_t cfg = {
        .broker.address.uri = MQTT_URI,
        .broker.verification.crt_bundle_attach = esp_crt_bundle_attach,
        .credentials.username = MQTT_USER,
        .credentials.authentication.password = MQTT_PASS,
        .credentials.client_id = s_devid,
        .session.keepalive = 60,
        .session.last_will = {                     /* 掉线时服务器立刻知道 */
            .topic = s_topic_status, .msg = lwt, .msg_len = lwtn, .qos = 1, .retain = 1,
        },
        .network.reconnect_timeout_ms = 5000,
        .network.disable_auto_reconnect = false,
    };
    s_mqtt = esp_mqtt_client_init(&cfg);
    esp_mqtt_client_register_event(s_mqtt, ESP_EVENT_ANY_ID, on_mqtt, NULL);
    esp_mqtt_client_start(s_mqtt);
}

const char *cloud_device_id(void) { return s_devid; }
