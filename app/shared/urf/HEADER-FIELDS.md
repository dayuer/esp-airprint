# URF 页头字段的核对状态

编码器写出去的每个页头字段，这里记它的依据和核对状态。
**没核对过的字段不要在别处声称已验证。**

| 字段 | 偏移 | 当前取值 | 依据 | 核对状态 |
|---|---|---|---|---|
| bits per pixel | 0 | 8 | 打印机能力串 `W8` = 8 位灰度 | 未核对 |
| colorspace | 1 | 0 | 格式文档；`W8` 是灰度 | **未核对，风险最高** |
| duplex | 2 | 0 | 单面 | 未核对 |
| quality | 3 | 0 | 默认 | 未核对 |
| 保留 | 4–11 | 全 0 | `docs/API-cloud-print.md` 第 7 节最小样本此处全 0 | 已核对 |
| 宽 | 12–15 | 大端 uint32 | `fix_page_count` 用 `h[12:20]` 逐页扫 | **已核对** |
| 高 | 16–19 | 大端 uint32 | 同上 | **已核对** |
| dpi | 20–23 | 大端 uint32 | 格式文档 | 未核对 |
| 保留 | 24–31 | 全 0 | 同上第 7 节样本 | 已核对 |

## 怎么核对

在装了 CUPS 的机器上，用 `tools/reference/render.py` 的路径产出一份真实 URF，
再用 `app/tools/urfdump.py` 转储，把上表的「当前取值」逐个对上：

    cupsfilter -P <PPD> -m image/urf sample.pdf > /tmp/real.urf
    python3 app/tools/urfdump.py /tmp/real.urf

对不上就改 `app/shared/urf/include/urf/urf.h` 里的常量，并更新本表。

`colorspace` 是风险最高的一个：填错不会报错，打印机可能出一沓乱码纸或整页黑。
第一次真机打印之前必须核对它。

## 行重复计数

| 项 | 状态 |
|---|---|
| 语义假设 | 值 N 表示该行再重复 N 次，共出现 N+1 行 |
| 默认 | **关闭**（`Writer(path)` 的第二参数默认 false） |
| 核对方式 | 用 `cupsfilter` 产出一份含大片空白的真实 URF，`urfdump.py` 看行数扫描是否与页高一致 |
| 打开的前提 | 上面核对通过，**并且**同步修改 `Validate` 让它按 N+1 计行 |

不核对就打开的后果：打印机按错误的行数解码，出半页或整页错位，而本地校验器
会说一切正常。
