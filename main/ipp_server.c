/*
 * 极简 IPP/1.1-2.0 打印服务器，AirPrint 够用为准。
 * 裸 TCP 实现 HTTP：iOS 发作业用 Expect:100-continue + chunked，esp_http_server
 * 不解 chunked 请求体，所以自己写。文档数据流式转发进 USB bulk，不落地。
 */
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/stream_buffer.h"
#include "lwip/sockets.h"
#include <errno.h>
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "driver/gpio.h"
#include "usb_printer.h"

static const char *TAG = "ipp";
static volatile int s_nconn;          /* 当前连接数，供卸载策略判断 */
#define PIN_LED_YELLOW GPIO_NUM_16
#define IPP_PORT 631

/* ---------------- 带解码的请求体读取器 ---------------- */
/* 带重试的 recv：超时(EAGAIN)不算结束，只有真正 EOF/错误才算 */
static int sock_recv(int fd, void *buf, int len)
{
    for (int try = 0; try < 5; try++) {
        int n = recv(fd, buf, len, 0);
        if (n > 0) return n;
        if (n == 0) return 0;                       /* 对端关闭 */
        if (errno == EAGAIN || errno == EWOULDBLOCK) continue;   /* 超时，再等 */
        return -1;
    }
    return -1;
}

typedef struct {
    int  fd;
    bool chunked;
    long cl_remaining;      /* Content-Length 模式剩余 */
    long ck_remaining;      /* 当前 chunk 剩余 */
    bool eof;
    long consumed;
    uint8_t *buf;          /* 指向连接私有堆缓冲，不占栈 */
    int  pos, len;
} body_t;

static int raw_byte(body_t *b)
{
    if (b->pos >= b->len) {
        int n = sock_recv(b->fd, b->buf, 1024);
        if (n <= 0) return -1;
        b->pos = 0; b->len = n;
    }
    b->consumed++;
    return b->buf[b->pos++];
}

static int chunk_head(body_t *b)      /* 读 "<hex>\r\n"，返回 chunk 大小 */
{
    long sz = 0; int c, seen = 0;
    while ((c = raw_byte(b)) >= 0) {
        if (c == '\r') { if (raw_byte(b) != '\n') return -1; return seen ? (int)sz : -1; }
        if (c == ';') { while ((c = raw_byte(b)) >= 0 && c != '\n'); return seen ? (int)sz : -1; }
        int v = (c>='0'&&c<='9') ? c-'0' : (c>='a'&&c<='f') ? c-'a'+10 : (c>='A'&&c<='F') ? c-'A'+10 : -1;
        if (v < 0) return -1;
        sz = sz*16 + v; seen = 1;
    }
    return -1;
}

static int body_read(body_t *b, uint8_t *out, int want)
{
    if (b->eof) return 0;
    if (!b->chunked) {
        if (b->cl_remaining <= 0) { b->eof = true; return 0; }
        if (want > b->cl_remaining) want = b->cl_remaining;
        int got = 0;
        while (got < 1) {                      /* 至少给 1 字节 */
            if (b->pos < b->len) {
                int n = b->len - b->pos; if (n > want) n = want;
                memcpy(out, b->buf + b->pos, n); b->pos += n; got = n;
            } else {
                int n = sock_recv(b->fd, out, want);
                if (n <= 0) { b->eof = true; return got; }
                got = n;
            }
        }
        b->cl_remaining -= got;
        b->consumed += got;
        if (b->cl_remaining == 0) b->eof = true;
        return got;
    }
    /* chunked */
    while (b->ck_remaining == 0) {
        int sz = chunk_head(b);
        if (sz < 0) { b->eof = true; return 0; }
        if (sz == 0) {                          /* trailer 直到空行；chunk_head 已吃掉 "0\r\n" 的 \n，故 nl 从 1 起 */
            int c, nl = 1;
            while (nl < 2 && (c = raw_byte(b)) >= 0) { if (c=='\n') nl++; else if (c!='\r') nl = 0; }
            b->eof = true; return 0;
        }
        b->ck_remaining = sz;
    }
    int n = b->ck_remaining < want ? b->ck_remaining : want;
    int got = 0;
    if (b->pos < b->len) {
        int m = b->len - b->pos; if (m > n) m = n;
        memcpy(out, b->buf + b->pos, m); b->pos += m; got = m;
    } else {
        got = sock_recv(b->fd, out, n);
        if (got <= 0) { b->eof = true; return 0; }
    }
    b->ck_remaining -= got;
    b->consumed += got;
    if (b->ck_remaining == 0) { raw_byte(b); raw_byte(b); }   /* 吃掉 chunk 尾 \r\n */
    return got;
}

static void body_drain(body_t *b){ uint8_t t[512]; while (body_read(b, t, sizeof t) > 0); }

/* ---------------- IPP 应答构造 ---------------- */
typedef struct { uint8_t *d; int len, cap; bool ovf; } obuf_t;
static void ob_raw(obuf_t *o, const void *p, int n)
{
    if (o->ovf) return;                       /* 溢出后一律丢弃，不再碰指针 */
    if (o->len + n <= o->cap) { memcpy(o->d + o->len, p, n); o->len += n; }
    else { o->ovf = true; }
}
static void ob_u8 (obuf_t *o, uint8_t v){ ob_raw(o,&v,1); }
static void ob_u16(obuf_t *o, uint16_t v){ uint8_t b[2]={v>>8,v}; ob_raw(o,b,2); }
static void ob_u32(obuf_t *o, uint32_t v){ uint8_t b[4]={v>>24,v>>16,v>>8,v}; ob_raw(o,b,4); }
static void ob_attr(obuf_t *o, uint8_t tag, const char *name, const void *v, int vlen)
{
    ob_u8(o, tag); ob_u16(o, strlen(name)); ob_raw(o, name, strlen(name));
    ob_u16(o, vlen); ob_raw(o, v, vlen);
}
static void ob_str (obuf_t *o, uint8_t tag, const char *n, const char *v){ ob_attr(o,tag,n,v,strlen(v)); }
static void ob_int (obuf_t *o, uint8_t tag, const char *n, uint32_t v){ uint8_t b[4]={v>>24,v>>16,v>>8,v}; ob_attr(o,tag,n,b,4); }
static void ob_bool(obuf_t *o, const char *n, bool v){ uint8_t b=v; ob_attr(o,0x22,n,&b,1); }
static void ob_reso(obuf_t *o, const char *n, uint32_t x, uint32_t y)
{ uint8_t b[9]={x>>24,x>>16,x>>8,x,y>>24,y>>16,y>>8,y,3}; ob_attr(o,0x32,n,b,9); }
/* 追加值（1setOf）：同 tag、空名字 */
static void ob_more_str(obuf_t *o, uint8_t tag, const char *v){ ob_u8(o,tag); ob_u16(o,0); ob_u16(o,strlen(v)); ob_raw(o,v,strlen(v)); }
static void ob_more_int(obuf_t *o, uint8_t tag, uint32_t v){ uint8_t b[4]={v>>24,v>>16,v>>8,v}; ob_u8(o,tag); ob_u16(o,0); ob_u16(o,4); ob_raw(o,b,4); }
static void ob_more_reso(obuf_t *o, uint32_t x, uint32_t y)
{ uint8_t b[9]={x>>24,x>>16,x>>8,x,y>>24,y>>16,y>>8,y,3}; ob_u8(o,0x32); ob_u16(o,0); ob_u16(o,9); ob_raw(o,b,9); }

/* ---- IPP 集合（collection）编码 ---- */
static void col_begin_named(obuf_t *o, const char *name)
{ ob_u8(o,0x34); ob_u16(o,strlen(name)); ob_raw(o,name,strlen(name)); ob_u16(o,0); }
static void col_begin_anon(obuf_t *o)
{ ob_u8(o,0x34); ob_u16(o,0); ob_u16(o,0); }
static void col_member(obuf_t *o, const char *m)
{ ob_u8(o,0x4A); ob_u16(o,0); ob_u16(o,strlen(m)); ob_raw(o,m,strlen(m)); }
static void col_end(obuf_t *o)
{ ob_u8(o,0x37); ob_u16(o,0); ob_u16(o,0); }
static void col_val_int(obuf_t *o, uint32_t v)
{ uint8_t b[4]={v>>24,v>>16,v>>8,v}; ob_u8(o,0x21); ob_u16(o,0); ob_u16(o,4); ob_raw(o,b,4); }
static void col_val_kw(obuf_t *o, const char *v)
{ ob_u8(o,0x44); ob_u16(o,0); ob_u16(o,strlen(v)); ob_raw(o,v,strlen(v)); }

/* 一个完整 media-col 集合体（不含 beg/end 之外的名字）：尺寸单位 1/100mm */
static void media_col_body(obuf_t *o, uint32_t x, uint32_t y)
{
    col_member(o, "media-size");
    col_begin_anon(o);
    col_member(o, "x-dimension"); col_val_int(o, x);
    col_member(o, "y-dimension"); col_val_int(o, y);
    col_end(o);
    col_member(o, "media-top-margin");    col_val_int(o, 423);
    col_member(o, "media-bottom-margin"); col_val_int(o, 423);
    col_member(o, "media-left-margin");   col_val_int(o, 423);
    col_member(o, "media-right-margin");  col_val_int(o, 423);
    col_member(o, "media-source"); col_val_kw(o, "main");
    col_member(o, "media-type");   col_val_kw(o, "stationery");
}

static void ob_op_group(obuf_t *o)
{
    ob_u8(o, 0x01);                                   /* operation-attributes */
    ob_str(o, 0x47, "attributes-charset", "utf-8");
    ob_str(o, 0x48, "attributes-natural-language", "en");
}

/* ---------------- 请求侧极简 IPP 解析 ---------------- */
/* 只需吃掉属性区（到 0x03），并顺手提取 requested job-id（可选） */
static bool ipp_eat_attrs(body_t *b, uint16_t *op, uint32_t *reqid)
{
    uint8_t h[8];
    int got = 0;
    while (got < 8) { int n = body_read(b, h+got, 8-got); if (n<=0) return false; got += n; }
    *op    = (h[2]<<8)|h[3];
    *reqid = (h[4]<<24)|(h[5]<<16)|(h[6]<<8)|h[7];

    /* 逐字节状态机吃属性区直到 end-of-attributes(0x03) */
    uint8_t c;
    while (body_read(b, &c, 1) == 1) {
        if (c == 0x03) return true;
        if (c <= 0x0f) continue;                       /* 组分隔 tag */
        uint8_t l2[2]; uint16_t n;
        if (body_read(b,l2,1)!=1 || body_read(b,l2+1,1)!=1) return false;
        n = (l2[0]<<8)|l2[1];
        uint8_t skip[128];
        while (n) { int k = n > sizeof(skip) ? sizeof(skip) : n; if (body_read(b,skip,k)<=0) return false; n -= k; }
        if (body_read(b,l2,1)!=1 || body_read(b,l2+1,1)!=1) return false;
        n = (l2[0]<<8)|l2[1];
        while (n) { int k = n > sizeof(skip) ? sizeof(skip) : n; if (body_read(b,skip,k)<=0) return false; n -= k; }
    }
    return false;
}

/* ---------------- 各操作应答 ---------------- */
static char s_uri[64];
static char s_host[16];
static char s_uuid[64];
static int  s_jobid = 0;
static volatile int s_job_state = 9;   /* IPP: 3=pending 5=processing 7=canceled 8=aborted 9=completed */

static void resp_printer_attrs(obuf_t *o, uint32_t reqid)
{
    obuf_t *b = o;
    ob_u16(b, 0x0200); ob_u16(b, 0x0000); ob_u32(b, reqid);   /* ver2.0 ok */
    ob_op_group(b);
    ob_u8(b, 0x04);                                   /* printer-attributes */
    ob_str(b, 0x45, "printer-uri-supported", s_uri);
    ob_str(b, 0x44, "uri-security-supported", "none");
    ob_str(b, 0x44, "uri-authentication-supported", "none");
    ob_str(b, 0x42, "printer-name", "HP136a");
    ob_str(b, 0x41, "printer-make-and-model", "HP Laser MFP 136a (ESP32 bridge)");
    ob_str(b, 0x41, "printer-location", "USB bridge");
    ob_str(b, 0x41, "printer-info", "HP Laser MFP 136a via ESP32-S3");
    ob_int(b, 0x23, "printer-state", 3);              /* idle */
    ob_str(b, 0x44, "printer-state-reasons", "none");
    ob_str(b, 0x44, "ipp-versions-supported", "1.1"); ob_more_str(b, 0x44, "2.0");
    ob_str(b, 0x44, "ipp-features-supported", "ipp-everywhere");
    ob_int(b, 0x23, "operations-supported", 0x0002);  /* Print-Job */
    ob_more_int(b, 0x23, 0x0004); ob_more_int(b, 0x23, 0x0005);
    ob_more_int(b, 0x23, 0x0006); ob_more_int(b, 0x23, 0x0008);
    ob_more_int(b, 0x23, 0x0009); ob_more_int(b, 0x23, 0x000A);
    ob_more_int(b, 0x23, 0x000B); ob_more_int(b, 0x23, 0x003C);  /* Identify-Printer */
    ob_more_int(b, 0x23, 0x003B);  /* Close-Job：iOS 走 Create-Job 流程时会用 */
    ob_str(b, 0x47, "charset-configured", "utf-8");
    ob_str(b, 0x47, "charset-supported", "utf-8");
    ob_str(b, 0x48, "natural-language-configured", "en");
    ob_str(b, 0x48, "generated-natural-language-supported", "en");
    ob_str(b, 0x49, "document-format-default", "image/urf");
    ob_str(b, 0x49, "document-format-supported", "image/urf");
    ob_more_str(b, 0x49, "application/octet-stream");
    ob_bool(b, "printer-is-accepting-jobs", true);
    ob_int(b, 0x21, "queued-job-count", 0);
    ob_str(b, 0x44, "pdl-override-supported", "attempted");
    ob_int(b, 0x21, "printer-up-time", (uint32_t)(xTaskGetTickCount()/configTICK_RATE_HZ)+1);
    ob_str(b, 0x44, "compression-supported", "none");
    ob_str(b, 0x44, "media-default", "iso_a4_210x297mm");
    ob_str(b, 0x44, "media-ready",   "iso_a4_210x297mm");
    ob_str(b, 0x44, "media-supported", "iso_a4_210x297mm");
    ob_more_str(b, 0x44, "na_letter_8.5x11in"); ob_more_str(b, 0x44, "na_legal_8.5x14in");
    ob_reso(b, "printer-resolution-default", 300, 300);
    ob_reso(b, "printer-resolution-supported", 300, 300); ob_more_reso(b, 600, 600);
    ob_int(b, 0x23, "print-quality-default", 4);
    ob_int(b, 0x23, "print-quality-supported", 4);
    ob_str(b, 0x44, "sides-default", "one-sided");
    ob_str(b, 0x44, "sides-supported", "one-sided");
    ob_str(b, 0x44, "print-color-mode-default", "monochrome");
    ob_str(b, 0x44, "print-color-mode-supported", "monochrome"); ob_more_str(b, 0x44, "auto");
    ob_str(b, 0x44, "urf-supported", "V1.4");
    ob_more_str(b, 0x44, "W8"); ob_more_str(b, 0x44, "SRGB24");
    ob_more_str(b, 0x44, "CP1"); ob_more_str(b, 0x44, "IS1");
    ob_more_str(b, 0x44, "OB10"); ob_more_str(b, 0x44, "PQ4");
    ob_more_str(b, 0x44, "RS300"); ob_more_str(b, 0x44, "DM1");
    /* iOS 硬性要求的 media-col 家族 */
    col_begin_named(b, "media-col-default"); media_col_body(b, 21000, 29700); col_end(b);
    col_begin_named(b, "media-col-ready");   media_col_body(b, 21000, 29700); col_end(b);
    col_begin_named(b, "media-col-database"); media_col_body(b, 21000, 29700); col_end(b);
    col_begin_anon(b); media_col_body(b, 21590, 27940); col_end(b);   /* letter，同属 database 的 1setOf */
    ob_int(b, 0x21, "media-top-margin-supported", 423);
    ob_int(b, 0x21, "media-bottom-margin-supported", 423);
    ob_int(b, 0x21, "media-left-margin-supported", 423);
    ob_int(b, 0x21, "media-right-margin-supported", 423);
    ob_str(b, 0x44, "media-source-supported", "main");
    ob_str(b, 0x44, "media-type-supported", "stationery");
    ob_str(b, 0x45, "printer-uuid", s_uuid);
    ob_str(b, 0x41, "printer-device-id",
        "MFG:HP;CMD:URF;MDL:HP Laser MFP 136a;CLS:PRINTER;");
    char more[80];
    snprintf(more, sizeof more, "http://%s:631/", s_host);
    ob_str(b, 0x45, "printer-more-info", more);
    ob_str(b, 0x42, "printer-dns-sd-name", "HP Laser MFP 136a");
    ob_bool(b, "color-supported", false);
    ob_int(b, 0x21, "pages-per-minute", 20);
    ob_int(b, 0x21, "printer-state-change-time", 1);
    ob_int(b, 0x21, "printer-config-change-time", 1);
    ob_str(b, 0x44, "which-jobs-supported", "completed");
    ob_more_str(b, 0x44, "not-completed");
    ob_bool(b, "multiple-document-jobs-supported", false);
    ob_int(b, 0x23, "multiple-operation-time-out", 60);
    ob_str(b, 0x44, "job-creation-attributes-supported", "copies");
    ob_more_str(b, 0x44, "media"); ob_more_str(b, 0x44, "media-col");
    ob_more_str(b, 0x44, "print-quality"); ob_more_str(b, 0x44, "sides");
    ob_more_str(b, 0x44, "print-color-mode"); ob_more_str(b, 0x44, "printer-resolution");
    ob_more_str(b, 0x44, "orientation-requested");
    ob_int(b, 0x21, "copies-default", 1);
    ob_attr(b, 0x33, "copies-supported", (const uint8_t[]){0,0,0,1,0,0,0,99}, 8);
    ob_int(b, 0x23, "orientation-requested-default", 3);
    ob_int(b, 0x23, "orientation-requested-supported", 3);
    ob_more_int(b, 0x23, 4);
    ob_str(b, 0x44, "print-scaling-default", "auto");
    ob_str(b, 0x44, "print-scaling-supported", "auto");
    ob_more_str(b, 0x44, "fill"); ob_more_str(b, 0x44, "fit"); ob_more_str(b, 0x44, "none");
    ob_str(b, 0x44, "identify-actions-default", "display");
    ob_str(b, 0x44, "identify-actions-supported", "display");
    ob_u8(b, 0x03);
}

static void resp_job(obuf_t *o, uint32_t reqid, int jobid, int state, const char *reason)
{
    ob_u16(o, 0x0200); ob_u16(o, 0x0000); ob_u32(o, reqid);
    ob_op_group(o);
    ob_u8(o, 0x02);                                   /* job-attributes */
    char juri[80]; snprintf(juri, sizeof juri, "%s/job/%d", s_uri, jobid);
    ob_str(o, 0x45, "job-uri", juri);
    ob_int(o, 0x21, "job-id", jobid);
    ob_int(o, 0x23, "job-state", state);
    ob_str(o, 0x44, "job-state-reasons", reason);
    ob_u8(o, 0x03);
}

static void resp_simple(obuf_t *o, uint32_t reqid, uint16_t status)
{
    ob_u16(o, 0x0200); ob_u16(o, status); ob_u32(o, reqid);
    ob_op_group(o);
    ob_u8(o, 0x03);
}

/* USB 泵：从蓄水池尽量大块地喂打印机 */
struct pump_ctx_s { StreamBufferHandle_t sb; volatile bool *rx_done, *usb_fail, *pump_done; };
void pump_fn(void *arg)
{
    struct pump_ctx_s *c = arg;
    static uint8_t pbuf[8192];
    while (1) {
        size_t got = xStreamBufferReceive(c->sb, pbuf, sizeof pbuf, pdMS_TO_TICKS(200));
        if (got == 0) {
            if (*c->rx_done) break;
            continue;
        }
        if (usb_printer_job_write(pbuf, got) != ESP_OK) { *c->usb_fail = true; break; }
    }
    *c->pump_done = true;
    vTaskDelete(NULL);
}

/* ---------------- HTTP 层 ---------------- */
static int send_all(int fd, const void *p, int n)
{
    const char *c = p;
    while (n > 0) { int k = send(fd, c, n, 0); if (k <= 0) return -1; c += k; n -= k; }
    return 0;
}

static bool shedding(void) { return s_nconn >= 2; }   /* iPhone 并发爆发很猛，尽早卸载 */

static void http_ipp_reply(int fd, obuf_t *o)
{
    char h[160];
    if (o->ovf) {                /* 应答溢出，宁可 500 也不发损坏数据 */
        const char *e = "HTTP/1.1 500 Internal Server Error\r\nContent-Length: 0\r\n\r\n";
        send_all(fd, e, strlen(e));
        return;
    }
    int n = snprintf(h, sizeof h,
        "HTTP/1.1 200 OK\r\nContent-Type: application/ipp\r\n"
        "Content-Length: %d\r\nConnection: %s\r\n\r\n", o->len,
        shedding() ? "close" : "keep-alive");
    send_all(fd, h, n);
    send_all(fd, o->d, o->len);
}

static uint8_t *s_gpa;                     /* 预生成的属性应答体（不含 8 字节头）*/
static int      s_gpa_len;
static StreamBufferHandle_t s_sb;          /* 全局唯一蓄水池，作业间复用 */
static SemaphoreHandle_t   s_sb_lock;

static void handle_conn(void *arg)
{
    int fd = (int)(intptr_t)arg;
    char line[1024];
    /* 空闲 75 秒自动断开：iOS 的 keep-alive 连接不关，不清会耗尽内存 */
    struct timeval tv = { .tv_sec = 8 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    uint8_t *rbuf = malloc(1024);
    uint8_t *bodybuf = malloc(1024);
    uint8_t *dbuf = NULL;
    if (!rbuf || !bodybuf) {
        ESP_LOGE(TAG, "连接缓冲分配失败 堆=%u", (unsigned)esp_get_free_heap_size());
        free(rbuf); free(bodybuf); close(fd); s_nconn--; vTaskDelete(NULL); return;
    }
    ESP_LOGI(TAG, "+连接 并发=%d 堆=%u", s_nconn, (unsigned)esp_get_free_heap_size());

    while (1) {
        /* ---- 读请求头 ---- */
        int hl = 0; bool got = false;
        long content_len = -1; bool chunked = false, expect100 = false;
        while (hl < (int)sizeof(line) - 1) {
            char c;
            int n = recv(fd, &c, 1, 0);
            if (n <= 0) goto done;
            line[hl++] = c;
            if (hl >= 4 && !memcmp(line + hl - 4, "\r\n\r\n", 4)) { got = true; break; }
        }
        if (!got) { ESP_LOGW(TAG, "请求头不完整，关闭"); goto done; }
        line[hl] = 0;



        bool is_post = !strncmp(line, "POST ", 5);
        for (char *p = strstr(line, "\r\n"); p; p = strstr(p + 2, "\r\n")) {
            if (!strncasecmp(p + 2, "Content-Length:", 15)) content_len = atol(p + 17);
            else if (!strncasecmp(p + 2, "Transfer-Encoding:", 18) && strcasestr(p + 20, "chunked")) chunked = true;
            else if (!strncasecmp(p + 2, "Expect:", 7) && strcasestr(p + 9, "100-continue")) expect100 = true;
        }

        if (!is_post) {                                    /* GET 等一律 200 空页 */
            const char *r = "HTTP/1.1 200 OK\r\nContent-Length: 0\r\nConnection: keep-alive\r\n\r\n";
            if (send_all(fd, r, strlen(r)) < 0) goto done;
            continue;
        }
        if (expect100) {
            const char *r = "HTTP/1.1 100 Continue\r\n\r\n";
            if (send_all(fd, r, strlen(r)) < 0) goto done;
        }

        body_t body = { .fd = fd, .chunked = chunked, .buf = bodybuf,
                        .cl_remaining = content_len < 0 ? 0 : content_len };
        if (!chunked && content_len <= 0) body.eof = true;

        uint16_t op = 0; uint32_t reqid = 1;
        obuf_t o = { .d = rbuf, .cap = 1024 };
        o.len = 0; o.ovf = false;

        if (!ipp_eat_attrs(&body, &op, &reqid)) { body_drain(&body); goto done; }
        ESP_LOGI(TAG, "IPP op=0x%04X reqid=%u cl=%ld chunked=%d attrs=%ld",
                 op, (unsigned)reqid, content_len, chunked, body.consumed);

        switch (op) {
        case 0x000B: {                                 /* Get-Printer-Attributes */
            body_drain(&body);
            /* 体是预生成的共享只读数据，这里只拼 8 字节头，零拷贝零溢出风险 */
            uint8_t hdr[8] = { 0x02, 0x00, 0x00, 0x00,
                               reqid >> 24, reqid >> 16, reqid >> 8, reqid };
            char hh[128];
            bool shed = shedding();
            int hl = snprintf(hh, sizeof hh,
                "HTTP/1.1 200 OK\r\nContent-Type: application/ipp\r\n"
                "Content-Length: %d\r\nConnection: %s\r\n\r\n",
                8 + s_gpa_len, shed ? "close" : "keep-alive");
            if (send_all(fd, hh, hl) < 0 || send_all(fd, hdr, 8) < 0 ||
                send_all(fd, s_gpa, s_gpa_len) < 0 || shed) goto done;
            continue;                                  /* 已自行发送，跳过统一回复 */
        }
        case 0x0004:                                   /* Validate-Job */
            body_drain(&body);
            resp_simple(&o, reqid, usb_printer_connected() ? 0x0000 : 0x0506);
            break;
        case 0x0005:                                   /* Create-Job */
            body_drain(&body);
            s_job_state = 3;                           /* pending：等文档 */
            resp_job(&o, reqid, ++s_jobid, 3, "none");
            break;
        case 0x0002:                                   /* Print-Job */
        case 0x0006: {                                 /* Send-Document */
            int jid = (op == 0x0002) ? ++s_jobid : s_jobid;
            s_job_state = 5;                           /* processing */
            if (usb_printer_job_begin() != ESP_OK) {
                body_drain(&body);
                resp_simple(&o, reqid, 0x0506);        /* server-error-not-accepting */
                break;
            }
            if (!dbuf) dbuf = malloc(8192);
            if (!dbuf) { body_drain(&body); usb_printer_job_end(); resp_simple(&o, reqid, 0x0500); break; }
            gpio_set_level(PIN_LED_YELLOW, 1);
            size_t total = 0; bool fail = false;
            int n;
            /* 网络收与 USB 写解耦：96KB 蓄水池抹平 Wi-Fi 抖动，
             * 打印机的 URF 解码器受不了页内断流 */
            xSemaphoreTake(s_sb_lock, portMAX_DELAY);
            StreamBufferHandle_t sb = s_sb;
            xStreamBufferReset(sb);
            volatile bool rx_done = false, usb_fail = false;
            volatile bool pump_done = false;
            struct pump_ctx { StreamBufferHandle_t sb; volatile bool *rx_done, *usb_fail, *pump_done; } ctx = { sb, &rx_done, &usb_fail, &pump_done };
            TaskHandle_t pump;
            void pump_fn(void *arg);
            xTaskCreate(pump_fn, "usb_pump", 4096, &ctx, 6, &pump);
            bool first_chunk = true;
            while ((n = body_read(&body, dbuf, 8192)) > 0) {
                if (first_chunk) {
                    first_chunk = false;
                    char hx[64]; int hl = 0;
                    for (int i = 0; i < 16 && i < n; i++)
                        hl += snprintf(hx + hl, sizeof hx - hl, "%02x", dbuf[i]);
                    ESP_LOGI(TAG, "文档头: %s", hx);
                }
                size_t off = 0;
                while (off < (size_t)n && !usb_fail)
                    off += xStreamBufferSend(sb, dbuf + off, n - off, pdMS_TO_TICKS(1000));
                if (usb_fail) { fail = true; break; }
                total += n;
            }
            rx_done = true;
            for (int w = 0; w < 1500 && !pump_done; w++)   /* 最多等 30s */
                vTaskDelay(pdMS_TO_TICKS(20));
            if (usb_fail || !pump_done) fail = true;
            xSemaphoreGive(s_sb_lock);
            usb_printer_job_end();
            gpio_set_level(PIN_LED_YELLOW, 0);
            ESP_LOGI(TAG, "作业 %d：%u 字节 %s", jid, (unsigned)total, fail ? "失败" : "完成");
            s_job_state = fail ? 8 : 9;
            if (fail) { body_drain(&body); resp_simple(&o, reqid, 0x0500); }
            else resp_job(&o, reqid, jid, 9, "job-completed-successfully");
            break;
        }
        case 0x003B:                                   /* Close-Job */
            body_drain(&body);
            resp_job(&o, reqid, s_jobid ? s_jobid : 1, s_job_state, "none");
            break;
        case 0x003C:                                   /* Identify-Printer */
            body_drain(&body);
            ESP_LOGI(TAG, "Identify-Printer");
            resp_simple(&o, reqid, 0x0000);
            break;
        case 0x0008:                                   /* Cancel-Job */
            body_drain(&body);
            if (s_job_state == 3 || s_job_state == 5) s_job_state = 7;
            resp_simple(&o, reqid, 0x0000);
            break;
        case 0x0009: {                                 /* Get-Job-Attributes */
            body_drain(&body);
            int st = s_job_state;
            const char *rs = (st == 3) ? "none" :
                             (st == 5) ? "job-printing" :
                             (st == 7) ? "job-canceled-by-user" :
                             (st == 8) ? "job-completed-with-errors" :
                                         "job-completed-successfully";
            resp_job(&o, reqid, s_jobid ? s_jobid : 1, st, rs);
            break;
        }
        case 0x000A:                                   /* Get-Jobs -> 空 */
            body_drain(&body);
            resp_simple(&o, reqid, 0x0000);
            break;
        default:
            body_drain(&body);
            resp_simple(&o, reqid, 0x0501);            /* operation-not-supported */
        }
        http_ipp_reply(fd, &o);
        free(dbuf); dbuf = NULL;
        if (shedding()) goto done;           /* 已声明 close，收工让位 */             /* 文档缓冲用完即还，应答/请求缓冲留着 */
    }
done:
    free(rbuf); free(bodybuf); free(dbuf);
    close(fd);
    s_nconn--;
    ESP_LOGI(TAG, "-连接 并发=%d 堆=%u 栈余=%u", s_nconn,
             (unsigned)esp_get_free_heap_size(),
             (unsigned)uxTaskGetStackHighWaterMark(NULL));
    vTaskDelete(NULL);
}

static void server_task(void *a)
{
    int ls = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in sa = { .sin_family = AF_INET, .sin_port = htons(IPP_PORT),
                              .sin_addr.s_addr = htonl(INADDR_ANY) };
    bind(ls, (struct sockaddr *)&sa, sizeof sa);
    listen(ls, 12);
    ESP_LOGI(TAG, "IPP 服务器就绪 :%d 空闲堆=%u", IPP_PORT,
             (unsigned)esp_get_free_heap_size());
    while (1) {
        int fd = accept(ls, NULL, NULL);
        if (fd < 0) continue;
        /* 绝不 reject：拒绝 accept 会让 iOS 直接判定「打印机不可用」。
         * 名额紧张时靠下面的 Connection: close 主动卸载空闲长连接。 */
        if (s_nconn >= 14 || esp_get_free_heap_size() < 9000) {
            ESP_LOGW(TAG, "无奈拒绝：并发=%d 堆=%u", s_nconn,
                     (unsigned)esp_get_free_heap_size());
            close(fd);
            continue;
        }
        int ka = 1; setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &ka, sizeof ka);
        s_nconn++;
        if (xTaskCreate(handle_conn, "ipp_conn", 4608, (void *)(intptr_t)fd, 5, NULL) != pdPASS) {
            ESP_LOGE(TAG, "建任务失败 并发=%d 堆=%u", s_nconn,
                     (unsigned)esp_get_free_heap_size());
            close(fd); s_nconn--;
        }
    }
}

void ipp_server_start(const char *ip, const uint8_t mac[6])
{
    strlcpy(s_host, ip, sizeof s_host);
    snprintf(s_uri, sizeof s_uri, "ipp://%s:631/ipp/print", ip);
    /* 必须是合法十六进制 UUID：之前 "e5p32b71" 里的 p/s 不是 hex，iOS 会拒 */
    snprintf(s_uuid, sizeof s_uuid,
             "urn:uuid:e5932b71-d6e0-4917-8a%02x-%02x%02x%02x%02x%02x%02x",
             mac[0], mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    /* 预生成属性应答（去掉前 8 字节头，头随请求变） */
    {
        uint8_t *tmp = malloc(12288);
        obuf_t g = { .d = tmp, .cap = 12288 };
        resp_printer_attrs(&g, 0);
        if (tmp && !g.ovf && g.len > 8) {
            s_gpa_len = g.len - 8;
            s_gpa = malloc(s_gpa_len);
            memcpy(s_gpa, tmp + 8, s_gpa_len);
            ESP_LOGI(TAG, "属性应答预生成 %d 字节", s_gpa_len);
        } else {
            ESP_LOGE(TAG, "属性应答生成失败 ovf=%d len=%d", g.ovf, g.len);
        }
        free(tmp);
    }
    s_sb_lock = xSemaphoreCreateMutex();
    s_sb = xStreamBufferCreate(16 * 1024, 1);      /* 一次性分配，终身复用 */
    ESP_LOGI(TAG, "蓄水池 %s，空闲堆=%u", s_sb ? "16KB 就绪" : "分配失败!",
             (unsigned)esp_get_free_heap_size());
    xTaskCreate(server_task, "ipp_srv", 4096, NULL, 5, NULL);
}
