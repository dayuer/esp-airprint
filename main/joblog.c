/*
 * 作业日志：把每份作业的关键节点写进 flash(NVS)。
 * 目的不是审计，是取证——崩溃时最后写下的阶段就是案发现场。
 * 只在阶段切换时写（每份作业约 5 次），不会磨损 flash。
 */
#include <string.h>
#include <stdio.h>
#include "esp_log.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "joblog.h"

static const char *TAG = "joblog";
#define NS "joblog"

static const char *PHASE_NAME[] = {
    "空闲", "收到任务", "重置接口", "唤醒打印机", "等待就绪",
    "传输中", "发结束符", "完成", "失败", "被取消",
};

static joblog_rec_t s_cur;

void joblog_boot_report(void)
{
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) {
        ESP_LOGI(TAG, "无历史作业记录");
        return;
    }
    joblog_rec_t r;
    size_t n = sizeof r;
    esp_err_t e = nvs_get_blob(h, "last", &r, &n);
    uint32_t total = 0, crashes = 0;
    nvs_get_u32(h, "total", &total);
    nvs_get_u32(h, "crashes", &crashes);
    nvs_close(h);

    if (e != ESP_OK || n != sizeof r) { ESP_LOGI(TAG, "无历史作业记录"); return; }

    const char *ph = r.phase < sizeof PHASE_NAME / sizeof PHASE_NAME[0]
                     ? PHASE_NAME[r.phase] : "?";
    ESP_LOGW(TAG, "上次作业 #%u：阶段=%s 已传=%u/%u 字节 用时=%us",
             (unsigned)r.seq, ph, (unsigned)r.sent, (unsigned)r.total,
             (unsigned)r.uptime_s);
    ESP_LOGW(TAG, "累计 %u 份作业，其中 %u 份未正常收尾", (unsigned)total, (unsigned)crashes);

    /* 阶段不是「完成/失败/被取消」= 上次是崩在半路的 */
    if (r.phase != JOB_DONE && r.phase != JOB_FAILED && r.phase != JOB_CANCELED) {
        ESP_LOGE(TAG, "⚠ 上次作业崩溃在【%s】阶段——这就是案发现场", ph);
        nvs_handle_t w;
        if (nvs_open(NS, NVS_READWRITE, &w) == ESP_OK) {
            nvs_set_u32(w, "crashes", crashes + 1);
            nvs_commit(w); nvs_close(w);
        }
    }
    ESP_LOGW(TAG, "本次复位原因：%d", (int)esp_reset_reason());
}

void joblog_phase(joblog_phase_t phase, uint32_t sent, uint32_t total)
{
    s_cur.phase    = phase;
    s_cur.sent     = sent;
    s_cur.total    = total;
    s_cur.uptime_s = (uint32_t)(esp_log_timestamp() / 1000);
    if (phase == JOB_RECEIVED) s_cur.seq++;

    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_blob(h, "last", &s_cur, sizeof s_cur);
    if (phase == JOB_DONE) {
        uint32_t t = 0;
        nvs_get_u32(h, "total", &t);
        nvs_set_u32(h, "total", t + 1);
    }
    nvs_commit(h);
    nvs_close(h);
}
