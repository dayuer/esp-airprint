/*
 * profile_script.c —— 怪癖档案的解析与校验。
 *
 * 刻意手写而不用通用 JSON 库：输入形状是固定的、有界的（≤1KB），
 * 而「只接受预期形状、其余一律拒绝」正是这里想要的行为。
 * 通用解析器会宽容地接受很多我们并不想接受的东西。
 *
 * 不依赖 ESP-IDF——见头文件里的主机测试命令。
 */
#include "profile_script.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    const char *p;
    const char *end;
    char       *err;
    size_t      errcap;
} scan_t;

static bool fail(scan_t *s, const char *fmt, ...)
{
    if (s->err && s->errcap) {
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(s->err, s->errcap, fmt, ap);
        va_end(ap);
    }
    return false;
}

static void ws(scan_t *s)
{
    while (s->p < s->end &&
           (*s->p == ' ' || *s->p == '\t' || *s->p == '\n' || *s->p == '\r'))
        s->p++;
}

static bool ch(scan_t *s, char c)
{
    ws(s);
    if (s->p < s->end && *s->p == c) { s->p++; return true; }
    return false;
}

/* 只认必要的转义。\u 之类一律拒绝——档案里不该出现，宽容没有好处。 */
static bool p_string(scan_t *s, char *out, size_t cap)
{
    ws(s);
    if (!ch(s, '"')) return fail(s, "期望字符串");
    size_t k = 0;
    while (s->p < s->end && *s->p != '"') {
        char c = *s->p++;
        if (c == '\\') {
            if (s->p >= s->end) return fail(s, "字符串在转义处截断");
            char e = *s->p++;
            switch (e) {
            case '"': c = '"'; break;
            case '\\': c = '\\'; break;
            case '/': c = '/'; break;
            case 'n': c = '\n'; break;
            case 'r': c = '\r'; break;
            case 't': c = '\t'; break;
            default: return fail(s, "不支持的转义 \\%c", e);
            }
        }
        if (out) {
            if (k + 1 >= cap) return fail(s, "字符串超出 %zu 字节", cap - 1);
            out[k++] = c;
        }
    }
    if (s->p >= s->end) return fail(s, "字符串没有闭合");
    s->p++;                      /* 吃掉收尾的引号 */
    if (out) out[k] = 0;
    return true;
}

static bool p_long(scan_t *s, long *out)
{
    ws(s);
    bool neg = false;
    if (s->p < s->end && (*s->p == '-' || *s->p == '+')) neg = (*s->p++ == '-');
    if (s->p >= s->end || *s->p < '0' || *s->p > '9') return fail(s, "期望数字");
    long v = 0;
    while (s->p < s->end && *s->p >= '0' && *s->p <= '9') {
        v = v * 10 + (*s->p++ - '0');
        if (v > 100000000L) return fail(s, "数字过大");
    }
    *out = neg ? -v : v;
    return true;
}

static bool p_bool(scan_t *s, bool *out)
{
    ws(s);
    size_t left = (size_t)(s->end - s->p);
    if (left >= 4 && !memcmp(s->p, "true", 4))  { s->p += 4; *out = true;  return true; }
    if (left >= 5 && !memcmp(s->p, "false", 5)) { s->p += 5; *out = false; return true; }
    return fail(s, "期望 true/false");
}

static bool skip_value(scan_t *s);

static bool skip_container(scan_t *s, char open, char close)
{
    if (!ch(s, open)) return fail(s, "期望 %c", open);
    if (ch(s, close)) return true;
    for (;;) {
        if (open == '{') {
            if (!p_string(s, NULL, 0)) return false;
            if (!ch(s, ':')) return fail(s, "对象里缺冒号");
        }
        if (!skip_value(s)) return false;
        if (ch(s, ',')) continue;
        if (ch(s, close)) return true;
        return fail(s, "期望 , 或 %c", close);
    }
}

/* 跳过任意值。有它才谈得上前向兼容：服务端加了新字段，老固件照样能解析。 */
static bool skip_value(scan_t *s)
{
    ws(s);
    if (s->p >= s->end) return fail(s, "值缺失");
    switch (*s->p) {
    case '"': return p_string(s, NULL, 0);
    case '{': return skip_container(s, '{', '}');
    case '[': return skip_container(s, '[', ']');
    case 't': case 'f': { bool b; return p_bool(s, &b); }
    case 'n':
        if ((size_t)(s->end - s->p) >= 4 && !memcmp(s->p, "null", 4)) { s->p += 4; return true; }
        return fail(s, "期望 null");
    default: { long v; return p_long(s, &v); }
    }
}

static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

const char *prof_op_name(prof_op_t op)
{
    switch (op) {
    case PROF_OP_SEND_HEX:    return "send_hex";
    case PROF_OP_DELAY_MS:    return "delay_ms";
    case PROF_OP_IFACE_RESET: return "iface_reset";
    case PROF_OP_READ_STATUS: return "read_status";
    }
    return "?";
}

const char *prof_hook_name(prof_hook_id_t h)
{
    switch (h) {
    case PROF_HOOK_JOB_BEGIN: return "job_begin";
    case PROF_HOOK_JOB_END:   return "job_end";
    case PROF_HOOK_WAKE:      return "wake";
    default:                  return "?";
    }
}

static void note_skipped(prof_script_t *sc, const char *op)
{
    for (int i = 0; i < sc->n_skipped; i++)
        if (!strcmp(sc->skipped[i], op)) return;      /* 已记过，去重 */
    if (sc->n_skipped >= PROF_MAX_SKIPPED) return;
    snprintf(sc->skipped[sc->n_skipped], sizeof sc->skipped[0], "%s", op);
    sc->n_skipped++;
}

/* 解析一个步骤对象。返回 -1 出错、0 跳过（未知原语且非必需）、1 成功。 */
static int p_step(scan_t *s, prof_script_t *sc, prof_hook_id_t hid, prof_step_t *out)
{
    char opname[16] = {0};
    char hexbuf[PROF_MAX_SEND * 2 + 2] = {0};
    bool have_op = false, required = false;
    long ms = -1;

    if (!ch(s, '{')) { fail(s, "步骤应当是对象"); return -1; }
    if (!ch(s, '}')) {
        for (;;) {
            char key[16];
            if (!p_string(s, key, sizeof key)) return -1;
            if (!ch(s, ':')) { fail(s, "步骤里缺冒号"); return -1; }
            if (!strcmp(key, "op")) {
                if (!p_string(s, opname, sizeof opname)) return -1;
                have_op = true;
            } else if (!strcmp(key, "data")) {
                if (!p_string(s, hexbuf, sizeof hexbuf)) return -1;
            } else if (!strcmp(key, "ms")) {
                if (!p_long(s, &ms)) return -1;
            } else if (!strcmp(key, "required")) {
                if (!p_bool(s, &required)) return -1;
            } else if (!skip_value(s)) {
                return -1;
            }
            if (ch(s, ',')) continue;
            if (ch(s, '}')) break;
            fail(s, "步骤里期望 , 或 }");
            return -1;
        }
    }
    if (!have_op) { fail(s, "步骤缺 op"); return -1; }

    if (!strcmp(opname, "send_hex")) {
        size_t n = strlen(hexbuf);
        if (n == 0 || n % 2) { fail(s, "send_hex 的 data 长度非法（%zu）", n); return -1; }
        if (n / 2 > PROF_MAX_SEND) {
            fail(s, "send_hex 要发 %zu 字节，上限 %d", n / 2, PROF_MAX_SEND);
            return -1;
        }
        for (size_t i = 0; i < n; i += 2) {
            int hi = hexval(hexbuf[i]), lo = hexval(hexbuf[i + 1]);
            if (hi < 0 || lo < 0) { fail(s, "send_hex 的 data 不是十六进制"); return -1; }
            out->data[i / 2] = (uint8_t)((hi << 4) | lo);
        }
        out->op = PROF_OP_SEND_HEX;
        out->len = (uint8_t)(n / 2);
        return 1;
    }
    if (!strcmp(opname, "delay_ms")) {
        if (ms <= 0 || ms > PROF_MAX_DELAY_MS) {
            fail(s, "delay_ms=%ld，须在 1~%d 之间", ms, PROF_MAX_DELAY_MS);
            return -1;
        }
        out->op = PROF_OP_DELAY_MS;
        out->ms = (uint16_t)ms;
        return 1;
    }
    if (!strcmp(opname, "iface_reset")) {
        /* 端点复位只在下一份作业开始时做。曾经在 job_end 里做过，结果稳定在
         * 距流尾几 KB 处 Decoding Fail——最后一个短包还没物理冲出去就被
         * halt/flush 掉了（HANDOFF 3.3）。 */
        if (hid == PROF_HOOK_JOB_END) {
            fail(s, "job_end 里不能做 iface_reset（会截断流尾）");
            return -1;
        }
        out->op = PROF_OP_IFACE_RESET;
        return 1;
    }
    if (!strcmp(opname, "read_status")) {
        out->op = PROF_OP_READ_STATUS;
        return 1;
    }

    /* 未知原语。标了 required 的整份拒绝——静默跳过一个关键的 send_hex 会让
     * 打印机行为错乱，而症状（第二份不出、乱码纸）最难归因回这一步。 */
    if (required) { fail(s, "不认识的必需原语 %s", opname); return -1; }
    note_skipped(sc, opname);
    return 0;
}

static bool p_hook(scan_t *s, prof_script_t *sc, prof_hook_id_t hid)
{
    prof_hook_t *h = &sc->hook[hid];
    h->n = 0;
    long delay_sum = 0;

    if (!ch(s, '[')) return fail(s, "%s 应当是数组", prof_hook_name(hid));
    if (ch(s, ']')) return true;                 /* 空数组是合法的，且有意义 */
    for (;;) {
        prof_step_t st = {0};
        int r = p_step(s, sc, hid, &st);
        if (r < 0) return false;
        if (r > 0) {
            if (h->n >= PROF_MAX_STEPS)
                return fail(s, "%s 超过 %d 步", prof_hook_name(hid), PROF_MAX_STEPS);
            if (st.op == PROF_OP_DELAY_MS) {
                delay_sum += st.ms;
                if (delay_sum > PROF_MAX_DELAY_SUM)
                    return fail(s, "%s 的 delay 合计 %ldms，上限 %d",
                                prof_hook_name(hid), delay_sum, PROF_MAX_DELAY_SUM);
            }
            h->step[h->n++] = st;
        }
        if (ch(s, ',')) continue;
        if (ch(s, ']')) return true;
        return fail(s, "%s 里期望 , 或 ]", prof_hook_name(hid));
    }
}

static bool p_hooks_obj(scan_t *s, prof_script_t *sc)
{
    if (!ch(s, '{')) return fail(s, "hooks 应当是对象");
    if (ch(s, '}')) return true;
    for (;;) {
        char key[16];
        if (!p_string(s, key, sizeof key)) return false;
        if (!ch(s, ':')) return fail(s, "hooks 里缺冒号");
        prof_hook_id_t hid = PROF_HOOK_N;
        if (!strcmp(key, "job_begin")) hid = PROF_HOOK_JOB_BEGIN;
        else if (!strcmp(key, "job_end")) hid = PROF_HOOK_JOB_END;
        else if (!strcmp(key, "wake")) hid = PROF_HOOK_WAKE;

        if (hid == PROF_HOOK_N) {
            /* 未知钩子名：跳过。服务端可以先行支持新钩子，老固件不至于变砖。 */
            if (!skip_value(s)) return false;
        } else if (!p_hook(s, sc, hid)) {
            return false;
        }
        if (ch(s, ',')) continue;
        if (ch(s, '}')) return true;
        return fail(s, "hooks 里期望 , 或 }");
    }
}

static bool p_flags(scan_t *s, prof_script_t *sc)
{
    if (!ch(s, '{')) return fail(s, "flags 应当是对象");
    if (ch(s, '}')) return true;
    for (;;) {
        char key[16];
        if (!p_string(s, key, sizeof key)) return false;
        if (!ch(s, ':')) return fail(s, "flags 里缺冒号");
        if (!strcmp(key, "unidir")) {
            if (!p_bool(s, &sc->unidir)) return false;
        } else if (!strcmp(key, "pjl_ok")) {
            if (!p_bool(s, &sc->pjl_ok)) return false;
        } else if (!skip_value(s)) {
            return false;
        }
        if (ch(s, ',')) continue;
        if (ch(s, '}')) return true;
        return fail(s, "flags 里期望 , 或 }");
    }
}

static bool parse_root(const char *json, size_t len, prof_script_t *sc,
                       bool hooks_only, char *err, size_t errcap)
{
    if (err && errcap) err[0] = 0;
    if (!json) { if (err) snprintf(err, errcap, "档案为空"); return false; }
    if (len == 0) len = strlen(json);
    if (len > PROF_MAX_JSON) {
        if (err) snprintf(err, errcap, "档案 %zu 字节，上限 %d", len, PROF_MAX_JSON);
        return false;
    }
    scan_t s = { json, json + len, err, errcap };

    if (!ch(&s, '{')) return fail(&s, "档案应当是 JSON 对象");
    if (!ch(&s, '}')) {
        for (;;) {
            char key[16];
            if (!p_string(&s, key, sizeof key)) return false;
            if (!ch(&s, ':')) return fail(&s, "缺冒号");
            if (!strcmp(key, "hooks")) {
                if (!p_hooks_obj(&s, sc)) return false;
            } else if (hooks_only) {
                if (!skip_value(&s)) return false;    /* 一次性覆盖只认 hooks */
            } else if (!strcmp(key, "flags")) {
                if (!p_flags(&s, sc)) return false;
            } else if (!strcmp(key, "serial")) {
                if (!p_string(&s, sc->serial, sizeof sc->serial)) return false;
            } else if (!strcmp(key, "src")) {
                if (!p_string(&s, sc->src, sizeof sc->src)) return false;
            } else if (!strcmp(key, "rev")) {
                long v; if (!p_long(&s, &v)) return false;
                sc->rev = (int)v;
            } else if (!skip_value(&s)) {
                return false;
            }
            if (ch(&s, ',')) continue;
            if (ch(&s, '}')) break;
            return fail(&s, "期望 , 或 }");
        }
    }
    ws(&s);
    if (s.p != s.end) return fail(&s, "对象之后还有多余内容");
    sc->valid = true;
    return true;
}

bool profile_script_parse(const char *json, size_t len,
                          prof_script_t *out, char *err, size_t errcap)
{
    prof_script_t tmp;
    memset(&tmp, 0, sizeof tmp);
    tmp.pjl_ok = true;                     /* 默认允许 PJL，与服务端默认一致 */
    if (!parse_root(json, len, &tmp, false, err, errcap)) return false;
    *out = tmp;
    return true;
}

bool profile_script_parse_hooks(const char *json, size_t len,
                                const prof_script_t *base, prof_script_t *out,
                                char *err, size_t errcap)
{
    prof_script_t tmp;
    if (base) tmp = *base;
    else { memset(&tmp, 0, sizeof tmp); tmp.pjl_ok = true; }
    tmp.n_skipped = 0;
    if (!parse_root(json, len, &tmp, true, err, errcap)) return false;
    *out = tmp;
    return true;
}
