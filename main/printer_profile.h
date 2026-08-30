#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include "profile_script.h"

/*
 * 打印机档案：把「这台机器怎么伺候」从代码里抽成数据。
 * 不同机型的怪癖差异很大，硬编码在流程里会越滚越乱。
 *
 * 结构体分两半，改之前先看清楚是哪一半：
 *   ① 身份与能力 —— 设备侧【当前没有任何代码读它】。这些字段原本用来生成
 *      IPP 属性应答和 mDNS TXT，那条路已废弃。现在渲染在服务端做，服务端
 *      认的是 CUPS PPD（server/bin/render.py 的 PPD 常量），不读这里。
 *      保留是因为它们是实测记录（尤其 urf 那串能力值），换机型时是起点。
 *      **改这里不会改变任何行为** —— 要改渲染，去改服务端的 PPD。
 *   ② 怪癖 —— usb_printer.c 真正消费的只有这一半，共 4 个字段。
 */
typedef struct {
    const char *name;              /* 档案名，日志用 */
    uint16_t    vid, pid;          /* 0,0 = 通用兜底档案 */

    /* ── ① 身份与能力：记录用，设备侧不消费（见上方说明）── */
    const char *make_and_model;
    const char *device_id;         /* IEEE-1284 风格 */
    const char *urf;               /* 打印机自报的 urf-supported，逗号分隔 */
    const char *media_default;     /* PWG 纸张名 */
    uint32_t    media_x, media_y;  /* 纸张尺寸，单位 1/100 mm */
    uint32_t    margin;            /* 四边留白，单位 1/100 mm */
    uint16_t    resolution;        /* dpi */
    uint16_t    ppm;               /* 页/分钟 */
    bool        color;
    bool        duplex;

    /* ── ② 怪癖：每条都是实测踩出来的，不是猜的。只有这一半在生效 ── */
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
    bool     unidir;           /* 只支持单向 I/O：不读 bulk IN、不发 PJL。
                                * 对这类机器读 IN 端点会一直超时甚至卡住整机。
                                * 由 CUPS quirks 表自动置位，也可在档案里写死 */

    /* ── ③ 来自 CUPS usb-quirks 的原始位，见 usb_quirks_db.h ──
     * 只作记录与上报（服务端据此判断该机型的已知状况）。
     * 真正影响行为的位已经在上面展开成具名字段了，别在流程里直接读这个。 */
    uint8_t  cups_quirks;
} printer_profile_t;

/* ── 服务端下发的档案（接口文档 3.7b）──
 *
 * 优先级：NVS 里这份 > 内置 PROFILES 表 > usb_quirks_db.h。
 * 编译进固件的那张表因此从「真相」降级为「连不上服务端时的兜底」。
 *
 * 存的是**原始 JSON**而不是解析后的结构体：将来服务端加了新原语，
 * 老固件存下的这份仍能被新固件重新解析出来。结构体一变，NVS 里的旧数据
 * 就只能丢弃。 */

/* 收到 profile 消息时调：落 NVS。serial 校验由调用方做（它才知道当前插的是谁）。 */
void profile_raw_save(const char *json, size_t len);
/* 开机时调：把上次那份读回来。没有则返回 0。 */
size_t profile_raw_load(char *out, size_t cap);
/* 清掉存档（解析不通过时用，免得每次开机都再失败一遍） */
void profile_raw_clear(void);

/* 把内置档案合成成同一种动作序列。
 *
 * 这样执行路径只有一条：不管怪癖是来自服务端还是编译进来的表，
 * usb_printer.c 都只认 prof_script_t，不需要维护两套分支。 */
void profile_script_from_builtin(const printer_profile_t *p, prof_script_t *out);

/* 按 VID/PID 选档案，认不出就返回通用档案。 */
const printer_profile_t *profile_lookup(uint16_t vid, uint16_t pid);
