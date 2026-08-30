/*
 * 用**固件里那份真正的解析代码**去解服务端下发的东西，把结果打成一行行
 * 便于比对的文本。
 *
 * 板子烧一轮要几分钟，而协议对不对是可以在主机上验的——这个小工具让
 * 「服务端下发的字节」和「固件会怎么执行」之间不再靠脑补。
 *
 *   cc -o decode_profile tools/fwtest/decode_profile.c main/profile_script.c -Imain
 *   decode_profile profile   < profile.json      # 整份档案
 *   decode_profile hooks     < job.json          # 作业信令里的一次性覆盖
 */
#include "profile_script.h"

#include <stdio.h>
#include <string.h>

static void dump(const prof_script_t *sc)
{
    printf("rev=%d serial=%s src=%s unidir=%d pjl_ok=%d\n",
           sc->rev, sc->serial, sc->src, sc->unidir, sc->pjl_ok);
    for (int h = 0; h < PROF_HOOK_N; h++) {
        printf("hook %s n=%u\n", prof_hook_name((prof_hook_id_t)h), sc->hook[h].n);
        for (int i = 0; i < sc->hook[h].n; i++) {
            const prof_step_t *s = &sc->hook[h].step[i];
            printf("  %s", prof_op_name(s->op));
            if (s->op == PROF_OP_SEND_HEX) {
                printf(" len=%d bytes=", s->len);
                for (int k = 0; k < s->len; k++) printf("%02x", s->data[k]);
            } else if (s->op == PROF_OP_DELAY_MS) {
                printf(" ms=%u", s->ms);
            }
            printf("\n");
        }
    }
    for (int i = 0; i < sc->n_skipped; i++)
        printf("skipped %s\n", sc->skipped[i]);
}

int main(int argc, char **argv)
{
    const char *mode = argc > 1 ? argv[1] : "profile";
    char buf[8192];
    size_t n = fread(buf, 1, sizeof buf - 1, stdin);
    buf[n] = 0;

    prof_script_t sc, base;
    char err[192];

    if (!strcmp(mode, "hooks")) {
        /* 一次性覆盖要有个基线才谈得上「没提到的保持不变」。
         * 这里用服务端的默认档案当基线，跟设备上的情形一致。 */
        static const char DEFAULT_JSON[] =
            "{\"rev\":1,\"src\":\"default\",\"flags\":{\"pjl_ok\":true},"
            "\"hooks\":{\"job_end\":[{\"op\":\"send_hex\","
            "\"data\":\"1b252d313233343558\",\"required\":true}]}}";
        memset(&base, 0, sizeof base);
        if (!profile_script_parse(DEFAULT_JSON, 0, &base, err, sizeof err)) {
            printf("ERR 基线解析失败: %s\n", err);
            return 1;
        }
        if (!profile_script_parse_hooks(buf, n, &base, &sc, err, sizeof err)) {
            printf("ERR %s\n", err);
            return 1;
        }
    } else {
        memset(&sc, 0, sizeof sc);
        if (!profile_script_parse(buf, n, &sc, err, sizeof err)) {
            printf("ERR %s\n", err);
            return 1;
        }
    }
    dump(&sc);
    return 0;
}
