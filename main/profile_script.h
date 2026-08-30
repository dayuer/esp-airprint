#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * 服务端下发的 USB 层怪癖档案（接口文档 3.7b）。
 *
 * 它不是参数表，是一份可编排的动作序列：服务端用四个原语编排三个钩子。
 * 那 9 个字节的 UEL 不再是固件里的常量，而是 job_end 里的一串十六进制——
 * 新机型的新怪癖改服务端一行文本即可，不用全网 OTA。
 *
 * 本文件与 profile_script.c **不依赖 ESP-IDF**，只用标准 C。解析和校验是
 * 最容易出错的一环，保持纯净是为了能在主机上跑测试：
 *
 *     cc -o /tmp/t tools/test_profile_script.c main/profile_script.c -Imain && /tmp/t
 */

/* 硬上限。设备可用堆只有 50~70KB，越界一律【整份拒绝】而不是截断——
 * 半份 profile 比没有 profile 更危险。服务端也验一遍，但不能假设上游一定对。 */
#define PROF_MAX_STEPS      8
#define PROF_MAX_SEND       64
#define PROF_MAX_JSON       1024
#define PROF_MAX_DELAY_MS   5000
#define PROF_MAX_DELAY_SUM  10000
#define PROF_MAX_SKIPPED    4     /* 最多记几个跳过的原语名，用于心跳上报 */

typedef enum {
    PROF_OP_SEND_HEX = 0,
    PROF_OP_DELAY_MS,
    PROF_OP_IFACE_RESET,
    PROF_OP_READ_STATUS,
} prof_op_t;

typedef struct {
    prof_op_t op;
    uint8_t   data[PROF_MAX_SEND];   /* PROF_OP_SEND_HEX 用 */
    uint8_t   len;
    uint16_t  ms;                    /* PROF_OP_DELAY_MS 用 */
} prof_step_t;

typedef struct {
    prof_step_t step[PROF_MAX_STEPS];
    uint8_t     n;
} prof_hook_t;

typedef enum {
    PROF_HOOK_JOB_BEGIN = 0,
    PROF_HOOK_JOB_END,
    PROF_HOOK_WAKE,
    PROF_HOOK_N,
} prof_hook_id_t;

typedef struct {
    bool        valid;
    int         rev;
    char        serial[48];
    char        src[16];
    bool        unidir;
    bool        pjl_ok;
    prof_hook_t hook[PROF_HOOK_N];

    /* 本固件不认识、已跳过的原语名。心跳里上报，让服务端知道这台设备还没
     * 跟上新原语。标了 required 的未知原语会让整份解析失败，走不到这里。 */
    char        skipped[PROF_MAX_SKIPPED][16];
    uint8_t     n_skipped;
} prof_script_t;

/* 解析并校验一份完整档案。成功返回 true 并填满 out；
 * 失败返回 false，原因写进 err。
 *
 * 失败即【整份拒绝】，调用方应退回内置兜底档案，不要部分套用。 */
bool profile_script_parse(const char *json, size_t len,
                          prof_script_t *out, char *err, size_t errcap);

/* 只解析 hooks 段，用于作业信令里的一次性覆盖（接口文档 3.4）。
 * out 先整份拷自 base，只有 JSON 里出现的钩子被替换；没提到的保持不变。
 * 空数组是有意义的：{"job_end":[]} 就是「本次不发作业结束符」。 */
bool profile_script_parse_hooks(const char *json, size_t len,
                                const prof_script_t *base, prof_script_t *out,
                                char *err, size_t errcap);

const char *prof_op_name(prof_op_t op);
const char *prof_hook_name(prof_hook_id_t h);
