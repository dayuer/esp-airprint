/*
 * profile_script.c 的主机端测试。
 *
 *     cc -o /tmp/t tools/test_profile_script.c main/profile_script.c -Imain && /tmp/t
 *
 * 解析和校验是整个固件里最容易出错、又最难在板子上调的一环。放在主机上跑，
 * 是因为改一行就能立刻验一遍——插板子烧录一轮要几分钟。
 */
#include "profile_script.h"

#include <stdio.h>
#include <string.h>

static int g_fail;

static void ok(int cond, const char *what)
{
    if (!cond) { printf("  ✗ %s\n", what); g_fail++; }
}

/* UEL：整个项目最关键的 9 个字节，现在它住在档案里而不是固件里 */
static const uint8_t UEL_BYTES[] = { 0x1b, '%', '-', '1', '2', '3', '4', '5', 'X' };

static bool parse(const char *json, prof_script_t *sc, char *err, size_t cap)
{
    memset(sc, 0, sizeof *sc);
    return profile_script_parse(json, 0, sc, err, cap);
}

#define DEFAULT_PROFILE \
    "{\"rev\":1,\"serial\":\"CNB9K1P2X4\",\"src\":\"default\"," \
    "\"flags\":{\"unidir\":false,\"pjl_ok\":true}," \
    "\"hooks\":{\"job_end\":[{\"op\":\"send_hex\"," \
    "\"data\":\"1b252d313233343558\",\"required\":true}]}}"

static void t_default(void)
{
    printf("服务端默认档案\n");
    prof_script_t sc; char err[128];
    ok(parse(DEFAULT_PROFILE, &sc, err, sizeof err), "应当解析成功");
    ok(sc.rev == 1, "rev");
    ok(!strcmp(sc.serial, "CNB9K1P2X4"), "serial");
    ok(!strcmp(sc.src, "default"), "src");
    ok(sc.pjl_ok && !sc.unidir, "flags");
    ok(sc.hook[PROF_HOOK_JOB_END].n == 1, "job_end 一步");
    const prof_step_t *st = &sc.hook[PROF_HOOK_JOB_END].step[0];
    ok(st->op == PROF_OP_SEND_HEX, "是 send_hex");
    ok(st->len == 9, "UEL 是 9 字节");
    ok(!memcmp(st->data, UEL_BYTES, 9), "UEL 字节正确");
    ok(sc.hook[PROF_HOOK_JOB_BEGIN].n == 0 && sc.hook[PROF_HOOK_WAKE].n == 0,
       "没提到的钩子为空");
}

static void t_full(void)
{
    printf("三个钩子都有内容\n");
    prof_script_t sc; char err[128];
    ok(parse("{\"hooks\":{"
             "\"job_begin\":[{\"op\":\"iface_reset\"}],"
             "\"job_end\":[{\"op\":\"send_hex\",\"data\":\"1b252d313233343558\"}],"
             "\"wake\":[{\"op\":\"send_hex\",\"data\":\"1b252d313233343558\"},"
             "{\"op\":\"delay_ms\",\"ms\":300}]}}", &sc, err, sizeof err),
       "应当解析成功");
    ok(sc.hook[PROF_HOOK_JOB_BEGIN].n == 1, "job_begin 一步");
    ok(sc.hook[PROF_HOOK_JOB_BEGIN].step[0].op == PROF_OP_IFACE_RESET, "iface_reset");
    ok(sc.hook[PROF_HOOK_WAKE].n == 2, "wake 两步");
    ok(sc.hook[PROF_HOOK_WAKE].step[1].ms == 300, "延时 300ms");
}

static void t_reject(void)
{
    printf("必须被拒的输入（每条都整份拒绝，不能部分套用）\n");
    struct { const char *json, *why; } bad[] = {
        { "not json at all",                     "根本不是 JSON" },
        { "{\"hooks\":",                          "截断" },
        { "{\"hooks\":{\"job_end\":[{\"op\":\"send_hex\",\"data\":\"zz\"}]}}",
          "data 不是十六进制" },
        { "{\"hooks\":{\"job_end\":[{\"op\":\"send_hex\",\"data\":\"1b2\"}]}}",
          "hex 长度是奇数" },
        { "{\"hooks\":{\"job_end\":[{\"op\":\"send_hex\",\"data\":\"\"}]}}",
          "空 data" },
        { "{\"hooks\":{\"job_end\":[{\"op\":\"delay_ms\",\"ms\":0}]}}",
          "delay 为 0" },
        { "{\"hooks\":{\"job_end\":[{\"op\":\"delay_ms\",\"ms\":5001}]}}",
          "delay 超单次上限" },
        { "{\"hooks\":{\"wake\":[{\"op\":\"delay_ms\",\"ms\":5000},"
          "{\"op\":\"delay_ms\",\"ms\":5000},{\"op\":\"delay_ms\",\"ms\":5000}]}}",
          "delay 合计超限" },
        { "{\"hooks\":{\"job_end\":[{\"op\":\"iface_reset\"}]}}",
          "job_end 里做接口复位（会截断流尾）" },
        { "{\"hooks\":{\"job_end\":[{\"op\":\"nope\",\"required\":true}]}}",
          "不认识的必需原语" },
        { "{\"hooks\":{\"job_end\":[{\"data\":\"1b\"}]}}", "步骤缺 op" },
        { "{\"hooks\":{\"job_end\":\"notarray\"}}", "钩子不是数组" },
        { "{\"hooks\":{\"job_end\":[]}} trailing", "对象后有多余内容" },
        { "{\"serial\":\"\\u0041\"}", "不支持的转义" },
    };
    prof_script_t sc; char err[128];
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        bool got = parse(bad[i].json, &sc, err, sizeof err);
        if (got) { printf("  ✗ 竟然通过了：%s\n", bad[i].why); g_fail++; }
        else if (!err[0]) { printf("  ✗ 拒了但没给原因：%s\n", bad[i].why); g_fail++; }
    }
}

static void t_limits(void)
{
    printf("边界\n");
    prof_script_t sc; char err[128];

    /* 8 步刚好，9 步超限 */
    char buf[PROF_MAX_JSON];
    for (int n = 8; n <= 9; n++) {
        int k = snprintf(buf, sizeof buf, "{\"hooks\":{\"wake\":[");
        for (int i = 0; i < n; i++)
            k += snprintf(buf + k, sizeof buf - k, "%s{\"op\":\"read_status\"}",
                          i ? "," : "");
        snprintf(buf + k, sizeof buf - k, "]}}");
        bool got = parse(buf, &sc, err, sizeof err);
        ok(n == 8 ? got : !got, n == 8 ? "8 步应当通过" : "9 步应当被拒");
    }

    /* send_hex 64 字节刚好，65 超限 */
    for (int n = 64; n <= 65; n++) {
        int k = snprintf(buf, sizeof buf,
                         "{\"hooks\":{\"wake\":[{\"op\":\"send_hex\",\"data\":\"");
        for (int i = 0; i < n; i++) k += snprintf(buf + k, sizeof buf - k, "41");
        snprintf(buf + k, sizeof buf - k, "\"}]}}");
        bool got = parse(buf, &sc, err, sizeof err);
        ok(n == 64 ? got : !got,
           n == 64 ? "send_hex 64 字节应当通过" : "send_hex 65 字节应当被拒");
    }

    /* 超过 1KB 整份拒绝——半份 profile 比没有 profile 更危险 */
    memset(buf, 'x', sizeof buf);
    ok(!profile_script_parse(buf, PROF_MAX_JSON + 1, &sc, err, sizeof err),
       "超过 1KB 应当被拒");
}

static void t_forward_compat(void)
{
    printf("前向兼容：服务端先行支持新东西，老固件不能变砖\n");
    prof_script_t sc; char err[128];

    /* 未知顶层字段、未知 flag、未知钩子名 —— 一律跳过 */
    ok(parse("{\"rev\":9,\"future_field\":{\"a\":[1,2,{\"b\":null}]},"
             "\"flags\":{\"unidir\":false,\"new_flag\":true},"
             "\"hooks\":{\"job_end\":[{\"op\":\"send_hex\",\"data\":\"1b\"}],"
             "\"future_hook\":[{\"op\":\"whatever\"}]}}", &sc, err, sizeof err),
       "未知字段应当被跳过而不是报错");
    ok(sc.rev == 9, "已知字段照常解析");
    ok(sc.hook[PROF_HOOK_JOB_END].n == 1, "已知钩子照常解析");

    /* 未知原语且非必需 —— 跳过并记下来，心跳要上报 */
    ok(parse("{\"hooks\":{\"wake\":[{\"op\":\"send_hex\",\"data\":\"1b\"},"
             "{\"op\":\"laser_align\"},{\"op\":\"laser_align\"}]}}",
             &sc, err, sizeof err), "未知可选原语应当跳过");
    ok(sc.hook[PROF_HOOK_WAKE].n == 1, "跳过的步骤不进执行序列");
    ok(sc.n_skipped == 1, "跳过的原语名去重后只记一次");
    ok(!strcmp(sc.skipped[0], "laser_align"), "记下了原语名");
}

static void t_oneshot(void)
{
    printf("作业信令里的一次性覆盖\n");
    prof_script_t base, out; char err[128];
    ok(parse(DEFAULT_PROFILE, &base, err, sizeof err), "先有基线");

    /* 空数组是有意义的：本次不发作业结束符。适配测试就靠它试变体。 */
    ok(profile_script_parse_hooks("{\"id\":\"j1\",\"size\":123,"
                                  "\"hooks\":{\"job_end\":[]}}", 0,
                                  &base, &out, err, sizeof err), "应当解析成功");
    ok(out.hook[PROF_HOOK_JOB_END].n == 0, "job_end 被清空");
    ok(!strcmp(out.serial, "CNB9K1P2X4"), "基线的其余字段保持不变");

    /* 没提到的钩子保持基线内容 */
    ok(profile_script_parse_hooks("{\"hooks\":{\"wake\":[{\"op\":\"read_status\"}]}}", 0,
                                  &base, &out, err, sizeof err), "只覆盖 wake");
    ok(out.hook[PROF_HOOK_JOB_END].n == 1, "job_end 未被提及，保持基线");
    ok(out.hook[PROF_HOOK_WAKE].n == 1, "wake 被替换");

    /* 完全没有 hooks 字段：整份照抄基线 */
    ok(profile_script_parse_hooks("{\"id\":\"j1\",\"size\":123}", 0,
                                  &base, &out, err, sizeof err), "没有 hooks 也合法");
    ok(out.hook[PROF_HOOK_JOB_END].n == 1, "全部保持基线");

    /* 一次性覆盖同样受校验约束，不能借它绕过 */
    ok(!profile_script_parse_hooks("{\"hooks\":{\"job_end\":[{\"op\":\"iface_reset\"}]}}", 0,
                                   &base, &out, err, sizeof err),
       "一次性覆盖也不能在 job_end 里做接口复位");
}

int main(void)
{
    t_default();
    t_full();
    t_reject();
    t_limits();
    t_forward_compat();
    t_oneshot();
    if (g_fail) { printf("\n%d 项失败\n", g_fail); return 1; }
    printf("\n全部通过\n");
    return 0;
}
