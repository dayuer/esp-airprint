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
#include <stdarg.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_idf_version.h"
#include "mqtt_client.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "usb_printer.h"
#include "lcd_ui.h"
#include "joblog.h"
#include "cloud_client.h"
#include "provision.h"
#include "printer_profile.h"
#include "profile_script.h"
#include "esp_random.h"
#include "esp_timer.h"

static const char *TAG = "cloud";

/* 云端地址与口令放在 cloud_creds.h（已 gitignore），
 * 照着 cloud_creds.h.example 复制一份填自己的。 */
#include "cloud_creds.h"
#define DL_CHUNK   4096
#define FETCH_RETRIES  6      /* 首次 + 5 次重试，见 API 文档 4.2 */
#define FETCH_RETRY_S  3
#define PROGRESS_REPORT_US  (10 * 1000000LL)   /* 传输中多久推一次进度 */
#define BACKOFF_MIN_MS  2000      /* 重连退避起步，见 API 文档 3.9 */
#define BACKOFF_MAX_MS 60000

static esp_mqtt_client_handle_t s_mqtt;
static char  s_devid[24];
/* 设备密钥 {key_id}.{secret}，45 字符。按不透明字符串处理——
 * 不解析、不截断、**不打进日志**（API 文档第 6 节规则 7）。 */
static char  s_devkey[80];
static char  s_bearer[96];      /* "Bearer <token>"，预拼好省得每次拼 */
/* 心跳里要上报的档案信息：服务端据此知道下发到没到位、
 * 以及这台设备是不是还没跟上新原语（接口文档 3.6）。 */
static int   s_profile_rev;
static char  s_skipped[PROF_MAX_SKIPPED][16];
static uint8_t s_n_skipped;

static char  s_topic_job[64], s_topic_status[64];
static char  s_topic_ident[64], s_topic_cmd[64], s_topic_profile[64];
static QueueHandle_t s_jobq;          /* 收到的作业 id */
static QueueHandle_t s_probeq;        /* 待执行的 PJL 探针命令 */
static QueueHandle_t s_identq;        /* 「打印机接上了，去采集档案」信号 */
static bool     s_auth_failed;        /* CONNACK 0x05：停止重连，别刷服务端 */
static uint32_t s_backoff_ms = BACKOFF_MIN_MS;
static void publish_ident(void);

typedef struct { char id[40]; uint32_t size; } job_msg_t;

/* JSON 字符串转义：device ID 里有分号、逗号、空格，也可能有引号和反斜杠。
 * 不转义会拼出非法 JSON，服务端整条记录丢掉。 */
static int jesc(char *dst, size_t cap, const char *src)
{
    size_t k = 0;
    for (const char *p = src; *p && k + 2 < cap; p++) {
        if (*p == '"' || *p == '\\') { dst[k++] = '\\'; dst[k++] = *p; }
        else if ((unsigned char)*p < 0x20) { if (k + 6 >= cap) break;
            k += snprintf(dst + k, cap - k, "\\u%04x", *p); }
        else dst[k++] = *p;
    }
    dst[k] = 0;
    return (int)k;
}

/* ── 机型档案上报 ──
 *
 * 分两条路，因为量级差了两个数量级：
 *   MQTT printer/{dev}/ident  —— 精简身份，几百字节，retain=1，服务端随时可读
 *   HTTPS POST /device/{dev}/ident —— 全量 dump，几 KB 到十几 KB
 *
 * 不把全量塞进 MQTT：信令通道的原则是「只传几十字节」，一份十几 KB 的
 * retain 消息会让每个订阅者一连上就吃一大口，也违背了当初把文档挪到
 * HTTPS 的理由。 */

/* 第 1 层探针清单。顺序有讲究：先便宜的、最可能成功的，
 * VARIABLES 放最后——它最大，也最可能超时。 */
static const char *PJL_PROBES[] = {
    "@PJL INFO ID",
    "@PJL INFO STATUS",
    "@PJL INFO PAGECOUNT",     /* 累计打印页数：机器有多旧、用得多狠 */
    "@PJL INFO CONFIG",        /* 纸盒、内存、选装件 */
    "@PJL INFO SUPPLIES",      /* 耗材；136a 实测返回空 */
    "@PJL INFO MEMORY",
    /* @PJL INFO VARIABLES 刻意不放进自动采集：它一条就 3KB，把载荷从
     * 6.5KB 顶到 9.8KB，而实测握着 9.8KB 做 HTTPS 会在证书验签那步
     * 分不到内存（报 PK verify failed，看着像证书问题其实是堆）。
     * 它描述的是机型能力，一辈子不变，按需用 cmd 探针取一次就够：
     *   mosquitto_pub -t printer/{dev}/cmd -m '{"probe":"@PJL INFO VARIABLES"}'
     */
};
#define PJL_PROBE_N (sizeof PJL_PROBES / sizeof PJL_PROBES[0])
#define DUMP_CAP     8192       /* 第 0 层 1.8KB + 六条探针，实测 ~6.5KB */
#define PROBE_CAP    4096
#define IDENT_RETRIES     8     /* 分不到缓冲时重试几次 */
#define IDENT_RETRY_S    15     /* 每次间隔多少秒——一份作业几十秒就传完 */

/* 有界追加。snprintf 在截断时返回的是「本该写多少」而不是「实际写了多少」，
 * 直接 k += 会让下标冲出缓冲——这里统一夹住。 */
static size_t app(char *b, size_t cap, size_t k, const char *fmt, ...)
{
    if (k + 1 >= cap) return k;
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(b + k, cap - k, fmt, ap);
    va_end(ap);
    if (n < 0) return k;
    return ((size_t)n < cap - k) ? k + (size_t)n : cap - 1;
}

/* state 只能取 API 文档 3.6 那六个值之一：
 *   ready / downloading / printing / done / failed / offline(仅 LWT)
 * 失败原因走 err 字段（<=200 字节），不要自造 state——服务端按 state 决定
 * 要不要派下一件，认不出的值会让作业卡到 180 秒超时。
 * done / failed 时 job 必须填对应作业 ID，否则服务端不知道是哪件结束了。 */
static void report_err(const char *job, const char *state, uint32_t bytes,
                       const char *err)
{
    usb_prn_status_t st;
    usb_printer_status(&st);
    char p[400];
    int n = snprintf(p, sizeof p,
        "{\"dev\":\"%s\",\"job\":\"%s\",\"state\":\"%s\",\"bytes\":%u,\"heap\":%u,"
        /* serial 是换机场景的关键：服务端靠它决定派哪些作业。
         * 报不准 = 把为 A 机光栅的作业打到 B 机上，出一沓废纸。
         * 拔掉打印机后 usb_printer_serial() 立刻返回空串（规则 10）。 */
        "\"serial\":\"%s\","
        "\"prn\":{\"code\":%d,\"display\":\"%s\",\"online\":%s,\"asleep\":%s,"
        "\"paper_out\":%s,\"error\":%s}",
        s_devid, job ? job : "", state, (unsigned)bytes,
        (unsigned)esp_get_free_heap_size(), usb_printer_serial(),
        st.code, st.display,
        st.online ? "true" : "false", st.asleep ? "true" : "false",
        st.paper_out ? "true" : "false", st.error ? "true" : "false");
    if (err && *err) {
        char esc[208];
        jesc(esc, sizeof esc, err);
        n += snprintf(p + n, sizeof p - n, ",\"err\":\"%s\"", esc);
    }
    /* 当前生效档案的版本号。服务端拿它核对下发到没到位——对不上就说明
     * 那份 retain 消息没送达，或者被 serial 校验挡掉了。 */
    if (s_profile_rev)
        n += snprintf(p + n, sizeof p - n, ",\"profile_rev\":%d", s_profile_rev);
    /* 本固件不认识、已跳过的原语。服务端据此知道这台设备还没跟上——
     * 不上报的话，服务端会以为新原语已经生效，而症状最难归因回这一步。 */
    if (s_n_skipped) {
        n += snprintf(p + n, sizeof p - n, ",\"skipped_ops\":[");
        for (uint8_t i = 0; i < s_n_skipped; i++)
            n += snprintf(p + n, sizeof p - n, "%s\"%s\"", i ? "," : "", s_skipped[i]);
        n += snprintf(p + n, sizeof p - n, "]");
    }
    n += snprintf(p + n, sizeof p - n, "}");
    /* 只有 publish 真的成功才算一拍心跳。
     * 失败也点亮屏幕的话，LCD 上就是一个骗人的心跳——而判活是
     * 离线队列和断点续传的地基（见 HANDOFF 第 5 节），不能骗。 */
    if (s_mqtt && esp_mqtt_client_publish(s_mqtt, s_topic_status, p, n, 1, 0) >= 0)
        lcd_ui_beat();
}

static void report(const char *job, const char *state, uint32_t bytes)
{
    report_err(job, state, bytes, NULL);
}

/* 作业体积到了 200KB~15MB（API 文档 8 表第 5c 条），
 * 屏上再显示原始字节数就是一串没法读的数字。 */
static void fmt_size(char *out, size_t cap, uint64_t n)
{
    if (n >= 1024ULL * 1024)
        snprintf(out, cap, "%.1f MB", (double)n / (1024 * 1024));
    else if (n >= 1024)
        snprintf(out, cap, "%.0f KB", (double)n / 1024);
    else
        snprintf(out, cap, "%u B", (unsigned)n);
}

/* 每个设备发起的 HTTP 请求都必须带这两个头（API 文档 4.1）。 */
static void auth_headers(esp_http_client_handle_t h)
{
    esp_http_client_set_header(h, "X-Device", s_devid);
    esp_http_client_set_header(h, "Authorization", s_bearer);
}

/* 把全量 dump POST 给服务端。失败不重试——下一次插拔或重启会再报一次，
 * 这份数据不是实时性要求高的东西。 */
static void post_dump(const char *json, int len)
{
    char url[160];
    snprintf(url, sizeof url, "%s/device/%s/ident", API_BASE, s_devid);
    esp_http_client_config_t cfg = {
        .url = url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .method = HTTP_METHOD_POST,
        .timeout_ms = 20000,
        .buffer_size = 2048,
    };
    esp_http_client_handle_t h = esp_http_client_init(&cfg);
    if (!h) return;
    esp_http_client_set_header(h, "Content-Type", "application/json");
    auth_headers(h);
    esp_http_client_set_post_field(h, json, len);
    /* 重试一次：堆是波动的，失败往往只是这一刻挤不下第二条 TLS */
    esp_err_t e = esp_http_client_perform(h);
    if (e != ESP_OK) {
        ESP_LOGW(TAG, "上传失败(%s)，8 秒后重试一次", esp_err_to_name(e));
        vTaskDelay(pdMS_TO_TICKS(8000));
        e = esp_http_client_perform(h);
    }
    if (e == ESP_OK)
        ESP_LOGI(TAG, "机型档案已上传 %d 字节 HTTP %d", len,
                 esp_http_client_get_status_code(h));
    else
        ESP_LOGW(TAG, "机型档案上传失败: %s", esp_err_to_name(e));
    esp_http_client_cleanup(h);
}

/* 采集全量档案：第 0 层无条件，第 1 层要 CMD: 授权。
 * 单次 POST（API 文档 4.3，上限 256KB）——服务端 v2 不做分片合并。
 *
 * 内存是这里唯一的约束：设备只有几十 KB 堆，MQTT 那条 TLS 一直占着，
 * 再开一条 HTTPS 已经很紧。实测握着 7.4KB 上传成功，9.8KB 就失败在
 * 证书验签。所以 VARIABLES 被移出自动采集，把载荷压在 ~6.5KB。 */
static void ident_task(void *a)
{
    uint8_t sig;
    while (xQueueReceive(s_identq, &sig, portMAX_DELAY) == pdTRUE) {
        /* 等打印机自己稳定下来。刚枚举完就灌 PJL，实测容易赶上它还在初始化。 */
        vTaskDelay(pdMS_TO_TICKS(3000));

        /* 开机那阵常常正好有作业在传，堆只剩几 KB。以前这里直接放弃，
         * 等于「开机就来活的设备永远不上报档案」。改成退避重试。 */
        char *buf = NULL;
        for (int try = 0; try < IDENT_RETRIES && !buf; try++) {
            if (!usb_printer_connected()) break;
            buf = malloc(DUMP_CAP);
            if (buf) break;
            ESP_LOGW(TAG, "堆不够（%u），%d 秒后重试采集机型档案 (%d/%d)",
                     (unsigned)esp_get_free_heap_size(), IDENT_RETRY_S,
                     try + 1, IDENT_RETRIES);
            vTaskDelay(pdMS_TO_TICKS(IDENT_RETRY_S * 1000));
        }
        if (!buf) {
            ESP_LOGW(TAG, "始终分不到缓冲，放弃本次机型档案采集"
                          "（下次插拔或重启会再试）");
            continue;
        }
        if (!usb_printer_connected()) { free(buf); continue; }

        /* ── 第 0 层：控制传输，零风险 ── */
        esp_err_t e = usb_printer_describe(buf, DUMP_CAP);
        if (e != ESP_OK && e != ESP_ERR_INVALID_SIZE) {
            ESP_LOGW(TAG, "第 0 层采集失败: %s", esp_err_to_name(e));
            free(buf); continue;
        }
        if (e == ESP_ERR_INVALID_SIZE)
            ESP_LOGW(TAG, "第 0 层输出被截断，DUMP_CAP 需要调大");
        size_t k = strlen(buf);

        /* ── 第 1 层：PJL 探针，仅当 device ID 的 CMD: 授权 ── */
        if (usb_printer_pjl_allowed() && k + 32 < DUMP_CAP) {
            char *reply = malloc(PROBE_CAP);
            if (reply) {
                k--;                                  /* 退掉最外层的 '}' */
                k = app(buf, DUMP_CAP, k, ",\"pjl\":{");
                for (size_t i2 = 0; i2 < PJL_PROBE_N; i2++) {
                    if (DUMP_CAP - k < 512) {
                        ESP_LOGW(TAG, "缓冲将满，跳过剩余 %u 条探针",
                                 (unsigned)(PJL_PROBE_N - i2));
                        break;
                    }
                    esp_err_t pe = usb_printer_probe(PJL_PROBES[i2], reply,
                                                     PROBE_CAP, 6000);
                    const char *key = PJL_PROBES[i2] + 10;   /* 跳过 "@PJL INFO " */
                    k = app(buf, DUMP_CAP, k, "%s\"%s\":\"", i2 ? "," : "", key);
                    k += jesc(buf + k, DUMP_CAP - k - 8, pe == ESP_OK ? reply : "");
                    k = app(buf, DUMP_CAP, k, "\"");
                    vTaskDelay(pdMS_TO_TICKS(200));   /* 别把打印机灌太急 */
                }
                k = app(buf, DUMP_CAP, k, "}}");
                free(reply);            /* ← 探针跑完就还，别带进 TLS 握手 */
            }
        }

        char *shrunk = realloc(buf, k + 1);
        if (shrunk) buf = shrunk;
        ESP_LOGI(TAG, "机型档案 %u 字节，缩容后堆=%u",
                 (unsigned)k, (unsigned)esp_get_free_heap_size());
        post_dump(buf, (int)k);
        free(buf);

        publish_ident();
    }
}

/* MQTT 上的精简身份，格式见 API 文档 3.8，retain=1，上限 512 字节。
 * 全量档案走 HTTPS——一份十几 KB 的 retain 消息会让每个订阅者
 * 一连上就吃一大口。 */
static void publish_ident(void)
{
    char p[512];
    if (usb_printer_ident_json(p, sizeof p) != ESP_OK) {
        ESP_LOGW(TAG, "精简身份生成失败（打印机未就绪？）");
        return;
    }
    int n = (int)strlen(p);
    if (s_mqtt) esp_mqtt_client_publish(s_mqtt, s_topic_ident, p, n, 1, 1);
    ESP_LOGI(TAG, "已上报精简身份 %d 字节", n);
}

/* 探针任务：把 MQTT 下发的 PJL 命令跑掉并回传结果。
 * 单独一个任务，因为 usb_printer_probe() 会阻塞等回包，
 * 绝不能在 MQTT 事件回调里做（那个任务栈很小）。 */
static void probe_task(void *a)
{
    char cmd[96];
    while (xQueueReceive(s_probeq, cmd, portMAX_DELAY) == pdTRUE) {
        char reply[512];
        esp_err_t e = usb_printer_probe(cmd, reply, sizeof reply, 3000);
        char esc_cmd[128], esc_rep[1100];
        jesc(esc_cmd, sizeof esc_cmd, cmd);
        jesc(esc_rep, sizeof esc_rep, e == ESP_OK ? reply : "");
        char *p = malloc(1600);
        if (!p) continue;
        int n = snprintf(p, 1600,
            "{\"dev\":\"%s\",\"probe\":\"%s\",\"ok\":%s,\"err\":\"%s\",\"reply\":\"%s\"}",
            s_devid, esc_cmd, e == ESP_OK ? "true" : "false",
            esp_err_to_name(e), esc_rep);
        if (s_mqtt) esp_mqtt_client_publish(s_mqtt, s_topic_status, p, n, 1, 0);
        free(p);
    }
}

/* ── 取回文档并直推打印机：全程流式，内存恒定 ──
 *
 * 状态上报按 API 文档 3.6：取件时 downloading，开始灌 USB 后 printing，
 * 传输中每 ~64KB 补一条 printing 续 180 秒超时（作业卡在打印机唤醒上时
 * 很容易超过 180 秒，不续会被服务端判为失败并重传）。 */
static void fetch_and_print(const job_msg_t *job)
{
    char url[160];
    snprintf(url, sizeof url, "%s/job/%s/data", API_BASE, job->id);
    lcd_ui_job("下载中…");
    report(job->id, "downloading", 0);

    uint32_t total = 0;
    bool ok = false;
    const char *err = "";

    /* 404 可能是「作业不存在」，也可能是「服务端还在渲染」（API 文档 4.2）。
     * 后者等一下就好，所以 404 要重试；403 是别人的作业，重试没意义。 */
    for (int attempt = 0; attempt < FETCH_RETRIES && !ok; attempt++) {
        if (attempt) {
            ESP_LOGW(TAG, "作业 %s 尚未就绪，%d 秒后重试 (%d/%d)",
                     job->id, FETCH_RETRY_S, attempt, FETCH_RETRIES - 1);
            vTaskDelay(pdMS_TO_TICKS(FETCH_RETRY_S * 1000));
        }
        esp_http_client_config_t cfg = {
            .url = url,
            .crt_bundle_attach = esp_crt_bundle_attach,
            .timeout_ms = 30000,
            .buffer_size = 2048,
        };
        esp_http_client_handle_t h = esp_http_client_init(&cfg);
        if (!h) { err = "http init"; break; }
        auth_headers(h);

        if (esp_http_client_open(h, 0) != ESP_OK) {
            err = "connect failed";
            esp_http_client_cleanup(h);
            continue;                       /* 网络抖动，重试 */
        }
        int64_t len = esp_http_client_fetch_headers(h);
        int status = esp_http_client_get_status_code(h);

        if (status == 404) {
            /* v2 起服务端不渲染，404 就是「作业没了」（已清理或已退回队列后删除）。
             * 反复重试同一个 ID 没有意义，回一条 failed 等下次派发（API 文档 4.2）。 */
            err = "job gone (404)";
            esp_http_client_cleanup(h);
            break;
        }
        if (status != 200) {
            ESP_LOGE(TAG, "取件失败 HTTP %d", status);
            static char e[40];
            snprintf(e, sizeof e, "HTTP %d", status);
            err = e;
            esp_http_client_cleanup(h);
            break;                          /* 401/403 之类，重试没意义 */
        }

        if (usb_printer_job_begin() != ESP_OK) {
            err = "printer offline";
            esp_http_client_cleanup(h);
            break;
        }
        lcd_ui_job("打印中…");
        report(job->id, "printing", 0);

        uint8_t *buf = malloc(DL_CHUNK);
        ok = (buf != NULL);
        if (!ok) err = "no memory";
        uint32_t last_mark = 0;
        int64_t  last_rep_us = esp_timer_get_time(), last_ui_us = 0;
        while (ok) {
            int n = esp_http_client_read(h, (char *)buf, DL_CHUNK);
            if (n < 0) { ok = false; err = "read error"; break; }
            if (n == 0) break;                    /* 下载完毕 */
            if (usb_printer_job_write(buf, n) != ESP_OK) {
                ok = false; err = "usb write failed"; break;
            }
            total += n;
            if ((total >> 16) != (last_mark >> 16)) {   /* 每 ~64KB */
                last_mark = total;
                joblog_phase(JOB_SENDING, total, (uint32_t)len);
            }
            int64_t now = esp_timer_get_time();
            /* 状态上报按时间限流，不按字节数：一份 15MB 的作业每 64KB 推一条
             * 就是 240 条心跳。目的只是续那 180 秒超时，10 秒一条足够。 */
            if (now - last_rep_us > PROGRESS_REPORT_US) {
                last_rep_us = now;
                report(job->id, "printing", total);
            }
            /* 屏幕 1 秒一次：15MB 能打好几分钟，光显示「打印中…」看不出死活 */
            if (now - last_ui_us > 1000000) {
                last_ui_us = now;
                char a[16], b[16], t[48];
                fmt_size(a, sizeof a, total);
                if (len > 0) {
                    fmt_size(b, sizeof b, (uint64_t)len);
                    snprintf(t, sizeof t, "%d%% %s/%s",
                             (int)(total * 100LL / len), a, b);
                } else {
                    snprintf(t, sizeof t, "已传 %s", a);
                }
                lcd_ui_job(t);
            }
        }
        free(buf);
        usb_printer_job_end();                    /* 内含 UEL 作业结束符 */
        esp_http_client_cleanup(h);
    }

    if (ok && total) {
        ESP_LOGI(TAG, "作业 %s 完成，%u 字节", job->id, (unsigned)total);
        char sz[16], t[40];
        fmt_size(sz, sizeof sz, total);
        snprintf(t, sizeof t, "已打印 %s", sz);
        lcd_ui_job(t);
        report(job->id, "done", total);
    } else {
        ESP_LOGW(TAG, "作业 %s 失败（已传 %u 字节）：%s",
                 job->id, (unsigned)total, err);
        lcd_ui_job("失败");
        report_err(job->id, "failed", total, err);
    }
}

/* 打印机枚举比 MQTT 连接慢几秒，开机那次上报必然是 offline。
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

        /* 打印机刚接上（含开机那次枚举完成）：上报一次身份。
         * 放在这里而不是 MQTT 连上时——枚举比 MQTT 慢几秒，
         * 在 CONNECTED 里发必然是空的。 */
        if (now && (first || !last)) {
            uint8_t sig = 1;
            xQueueSend(s_identq, &sig, 0);
        }

        /* 两条都要：状态一变立刻推（缺纸/开盖/休眠不能等 30 秒），
         * 没变也要按时推（服务器按最后一次心跳判活，只在变化时推
         * 会让设备在服务器眼里凭空消失）。 */
        if (changed || quiet >= 15) {
            /* 没插打印机 = offline。从派件的角度看，「桥断了」和「桥在
             * 但没打印机」是同一件事——都不能接活，服务端都不该派件。
             * 所以不需要额外的状态：复用 offline 即可。
             * 心跳照发，所以服务端仍能从 seen 看出桥是活的，
             * 也能从 prn 看出为什么打不了。 */
            report(NULL, now ? "ready" : "offline", 0);
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

/* 极简 JSON 布尔取值。找不到返回 dflt。 */
static bool jbool(const char *s2, const char *key, bool dflt)
{
    char v[12];
    jget(s2, key, v, sizeof v);
    if (!v[0]) return dflt;
    return (v[0] == 't' || v[0] == 'T' || v[0] == '1');
}
static long jint(const char *s2, const char *key, long dflt)
{
    char v[16];
    jget(s2, key, v, sizeof v);
    return v[0] ? atol(v) : dflt;
}

/* 套用服务端下发的 USB 层怪癖档案（接口文档 3.7b）。
 *
 * 三步：解析校验 → serial 校验 → 落 NVS 并生效。
 * 任何一步不过就整份丢弃、退回内置兜底——半份 profile 比没有 profile 更危险。 */
static void apply_profile(const char *json, size_t len)
{
    prof_script_t sc;
    char err[128];
    if (!profile_script_parse(json, len, &sc, err, sizeof err)) {
        ESP_LOGW(TAG, "档案不合法，整份丢弃：%s", err);
        return;
    }

    /* serial 不符就整份忽略——profile 是 retain 消息，用户换了打印机之后
     * 旧档案会先到，套上去就是错的（规则 8）。 */
    const char *cur = usb_printer_serial();
    if (sc.serial[0] && cur[0] && strcmp(sc.serial, cur)) {
        ESP_LOGW(TAG, "档案是给 %s 的，当前插的是 %s——忽略", sc.serial, cur);
        return;
    }

    profile_raw_save(json, len);
    s_profile_rev = sc.rev;
    s_n_skipped = sc.n_skipped;
    for (int i = 0; i < sc.n_skipped && i < PROF_MAX_SKIPPED; i++)
        snprintf(s_skipped[i], sizeof s_skipped[0], "%s", sc.skipped[i]);
    if (sc.n_skipped)
        ESP_LOGW(TAG, "档案里有 %u 个本固件不认识的原语，已跳过并上报",
                 sc.n_skipped);
    usb_printer_set_script(&sc);
}

/* 开机时把上次那份读回来：设备可能在没有网络的情况下先插上打印机开印。 */
void cloud_profile_restore(void)
{
    char buf[PROF_MAX_JSON + 1];
    size_t n = profile_raw_load(buf, sizeof buf);
    if (!n) return;
    prof_script_t sc;
    char err[128];
    if (!profile_script_parse(buf, n, &sc, err, sizeof err)) {
        /* 存档解析不了就清掉，免得每次开机都再失败一遍 */
        ESP_LOGW(TAG, "NVS 里的档案解析失败，已清除：%s", err);
        profile_raw_clear();
        return;
    }
    s_profile_rev = sc.rev;
    ESP_LOGI(TAG, "从 NVS 恢复档案 src=%s rev=%d serial=%s", sc.src, sc.rev, sc.serial);
    usb_printer_set_script(&sc);
}

static void on_mqtt(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    esp_mqtt_event_handle_t e = data;
    switch ((esp_mqtt_event_id_t)id) {
    case MQTT_EVENT_CONNECTED:
        ESP_LOGI(TAG, "已连接云端 %s", MQTT_URI);
        lcd_ui_wifi("云端已连接");
        s_backoff_ms = BACKOFF_MIN_MS;          /* 连上了，退避归零 */
        esp_mqtt_set_config(s_mqtt, &(esp_mqtt_client_config_t){
            .network.reconnect_timeout_ms = s_backoff_ms });
        esp_mqtt_client_subscribe(s_mqtt, s_topic_job, 1);
        esp_mqtt_client_subscribe(s_mqtt, s_topic_cmd, 1);
        esp_mqtt_client_subscribe(s_mqtt, s_topic_profile, 1);
        break;
    case MQTT_EVENT_DISCONNECTED:
        if (s_auth_failed) break;               /* 已经停了，别再刷日志 */
        /* 指数退避 + ±20% 抖动（API 文档 3.9）。抖动是为了避免全网设备
         * 同时重连把服务端的密钥校验压垮。 */
        s_backoff_ms = s_backoff_ms * 2;
        if (s_backoff_ms > BACKOFF_MAX_MS) s_backoff_ms = BACKOFF_MAX_MS;
        {
            int jitter = (int)(esp_random() % (s_backoff_ms / 5 * 2 + 1))
                       - (int)(s_backoff_ms / 5);
            int next = (int)s_backoff_ms + jitter;
            if (next < 1000) next = 1000;
            ESP_LOGW(TAG, "与云端断开，%d ms 后重连", next);
            esp_mqtt_set_config(s_mqtt, &(esp_mqtt_client_config_t){
                .network.reconnect_timeout_ms = next });
        }
        lcd_ui_wifi("云端重连中");
        break;
    case MQTT_EVENT_DATA: {
        char buf[192];
        int n = e->data_len < (int)sizeof buf - 1 ? e->data_len : (int)sizeof buf - 1;
        memcpy(buf, e->data, n); buf[n] = 0;

        /* 三个订阅共用这个回调，先按 topic 分流 */
        bool is_topic(const char *t) {
            return e->topic && e->topic_len == (int)strlen(t) &&
                   strncmp(e->topic, t, e->topic_len) == 0;
        }
        if (is_topic(s_topic_profile)) {
            ESP_LOGI(TAG, "收到服务端档案: %s", buf);
            apply_profile(buf, strlen(buf));
            break;
        }
        if (is_topic(s_topic_cmd)) {
            char pjl[96];
            jget(buf, "probe", pjl, sizeof pjl);
            if (pjl[0]) {
                ESP_LOGI(TAG, "收到探针指令: %s", pjl);
                if (xQueueSend(s_probeq, pjl, 0) != pdTRUE)
                    ESP_LOGW(TAG, "探针队列满，丢弃");
            }
            break;
        }

        ESP_LOGI(TAG, "收到作业通知: %s", buf);
        job_msg_t j = { 0 };
        char sz[16];
        jget(buf, "id", j.id, sizeof j.id);
        jget(buf, "size", sz, sizeof sz);
        j.size = (uint32_t)atoi(sz);
        if (j.id[0]) {
            /* 作业级一次性钩子覆盖（接口文档 3.4）。只对本次生效，
             * 不落 NVS，作业结束由 usb_printer 自己销毁。
             * 空数组是有意义的：{"job_end":[]} 就是「本次不发作业结束符」，
             * 适配测试靠它试变体。 */
            if (strstr(buf, "\"hooks\""))
                usb_printer_job_hooks(buf, strlen(buf));
            joblog_phase(JOB_RECEIVED, 0, j.size);
            if (xQueueSend(s_jobq, &j, 0) != pdTRUE)
                report(j.id, "busy", 0);
        }
        break;
    }
    case MQTT_EVENT_ERROR:
        /* CONNACK 0x05 = Not authorized：密钥错了，重试一万次也是错的。
         * 必须停下并报出来，否则设备会陷入「被拒 → 重连」的死循环，
         * 白白消耗服务端的密钥校验（API 文档 3.1 / 第 6 节规则 5）。 */
        if (e->error_handle &&
            e->error_handle->error_type == MQTT_ERROR_TYPE_CONNECTION_REFUSED &&
            e->error_handle->connect_return_code ==
                MQTT_CONNECTION_REFUSE_NOT_AUTHORIZED) {
            s_auth_failed = true;
            ESP_LOGE(TAG, "云端拒绝认证（密钥错误或已吊销）——停止重连。"
                          "开机按住 MENU 键清配置后重新配网");
            lcd_ui_wifi("密钥被拒");
            lcd_ui_prn("请重新配网");
            esp_mqtt_client_stop(s_mqtt);
        } else {
            ESP_LOGE(TAG, "MQTT 错误 type=%d",
                     e->error_handle ? e->error_handle->error_type : -1);
        }
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
    snprintf(s_topic_ident,  sizeof s_topic_ident,  "printer/%s/ident", s_devid);
    snprintf(s_topic_cmd,    sizeof s_topic_cmd,    "printer/%s/cmd", s_devid);
    snprintf(s_topic_profile, sizeof s_topic_profile, "printer/%s/profile", s_devid);
    ESP_LOGI(TAG, "设备 ID: %s", s_devid);

    /* 没有设备密钥就不要连——服务端会以 CONNACK 0x05 拒绝，
     * 而「被拒 → 重连」是个死循环。停在这里等用户配网。 */
    if (!prov_load_devkey(s_devkey, sizeof s_devkey)) {
        ESP_LOGE(TAG, "未配置设备密钥，不连接云端。"
                      "开机按住 MENU 键清配置，在配网页里填入密钥");
        lcd_ui_wifi("缺设备密钥");
        lcd_ui_prn("请重新配网");
        return;
    }
    snprintf(s_bearer, sizeof s_bearer, "Bearer %s", s_devkey);
    ESP_LOGI(TAG, "设备密钥已加载（%d 字符）", (int)strlen(s_devkey));

    s_jobq   = xQueueCreate(2, sizeof(job_msg_t));
    s_probeq = xQueueCreate(4, 96);
    s_identq = xQueueCreate(2, 1);
    xTaskCreate(job_task, "job", 6144, NULL, 5, NULL);
    xTaskCreate(probe_task, "probe", 5120, NULL, 4, NULL);
    xTaskCreate(ident_task, "ident", 6144, NULL, 3, NULL);
    xTaskCreate(watch_task, "watch", 3072, NULL, 3, NULL);

    char lwt[96];
    int lwtn = snprintf(lwt, sizeof lwt, "{\"dev\":\"%s\",\"state\":\"offline\"}", s_devid);

    const esp_mqtt_client_config_t cfg = {
        .broker.address.uri = MQTT_URI,
        .broker.verification.crt_bundle_attach = esp_crt_bundle_attach,
        /* API 文档 3.1：username 是设备 ID，password 是完整令牌 */
        .credentials.username = s_devid,
        .credentials.authentication.password = s_devkey,
        .credentials.client_id = s_devid,
        .session.keepalive = 60,
        .session.disable_clean_session = false,   /* clean_session = true */
        .session.last_will = {                     /* 掉线时服务器立刻知道 */
            .topic = s_topic_status, .msg = lwt, .msg_len = lwtn, .qos = 1, .retain = 1,
        },
        .network.reconnect_timeout_ms = BACKOFF_MIN_MS,
        .network.disable_auto_reconnect = false,
    };
    s_mqtt = esp_mqtt_client_init(&cfg);
    esp_mqtt_client_register_event(s_mqtt, ESP_EVENT_ANY_ID, on_mqtt, NULL);
    esp_mqtt_client_start(s_mqtt);
}

const char *cloud_device_id(void) { return s_devid; }
