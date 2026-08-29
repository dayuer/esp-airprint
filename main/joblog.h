#pragma once
#include <stdint.h>

typedef enum {
    JOB_IDLE = 0, JOB_RECEIVED, JOB_RESET_IF, JOB_WAKING, JOB_WAIT_READY,
    JOB_SENDING, JOB_UEL, JOB_DONE, JOB_FAILED, JOB_CANCELED,
} joblog_phase_t;

typedef struct {
    uint32_t seq, sent, total, uptime_s;
    uint32_t phase;
} joblog_rec_t;

/* 开机时把上次作业的最后阶段打出来——崩在哪一步一目了然 */
void joblog_boot_report(void);
/* 阶段切换时调用，写入 flash */
void joblog_phase(joblog_phase_t phase, uint32_t sent, uint32_t total);
