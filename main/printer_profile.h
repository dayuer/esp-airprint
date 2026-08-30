#pragma once
#include <stdint.h>
#include <stdbool.h>

/*
 * 打印机档案：把「这台机器怎么伺候」从代码里抽成数据。
 * 不同机型的怪癖差异很大，硬编码在流程里会越滚越乱。
 */
typedef struct {
    const char *name;              /* 档案名，日志用 */
    uint16_t    vid, pid;          /* 0,0 = 通用兜底档案 */

    /* ── 对外身份（IPP / mDNS 用）── */
    const char *make_and_model;
    const char *device_id;         /* IEEE-1284 风格 */

    /* ── 能力 ── */
    const char *urf;               /* urf-supported，逗号分隔；须与 mDNS TXT 的 URF 逐字一致 */
    const char *media_default;     /* PWG 纸张名 */
    uint32_t    media_x, media_y;  /* 纸张尺寸，单位 1/100 mm */
    uint32_t    margin;            /* 四边留白，单位 1/100 mm */
    uint16_t    resolution;        /* dpi */
    uint16_t    ppm;               /* 页/分钟 */
    bool        color;
    bool        duplex;

    /* ── 怪癖：每条都是实测踩出来的，不是猜的 ── */
    bool     uel_job_end;      /* 作业末尾补 UEL。USB 短包只代表「传输结束」，
                                * 不代表「作业结束」；缺了它打印机会一直等后续
                                * 数据，必须手动按取消键才能打下一份 */
    bool     uel_wake;         /* 作业开始前发 UEL 敲门唤醒 */
    uint32_t wake_delay_ms;    /* 敲门后等多久再灌数据（激光机要预热） */
    bool     iface_cycle;      /* 作业之间 release/claim 接口做作业边界 */
    bool     pjl_ustatus;      /* 开启 PJL 异步状态上报，用于休眠检测。
                                * GET_PORT_STATUS 在休眠时照样报 0x18，没用 */
    bool     soft_reset;       /* 打印机类 SOFT_RESET。136a 直接 STALL，
                                * CUPS 的 soft-reset quirk 只针对三星原厂 VID 0x04e8 */
} printer_profile_t;

/* 按 VID/PID 选档案，认不出就返回通用档案 */
const printer_profile_t *profile_lookup(uint16_t vid, uint16_t pid);
