import re, sys
src = open(sys.argv[1]).read().splitlines()

# ESP32 上真正用得上的 quirk -> 位。no-reattach 是 Linux usblp 内核模块专用，
# whitelist 只是「确认可用」的注记，两者都不生成位。
BITS = {
    'blacklist':    ('QK_BLACKLIST',    0x01),
    'unidir':       ('QK_UNIDIR',       0x02),
    'soft-reset':   ('QK_SOFT_RESET',   0x04),
    'delay-close':  ('QK_DELAY_CLOSE',  0x08),
    'usb-init':     ('QK_USB_INIT',     0x10),
    'vendor-class': ('QK_VENDOR_CLASS', 0x20),
    'no-alt-set':   ('QK_NO_ALT_SET',   0x40),
}
SKIP = {'no-reattach', 'whitelist'}

rows, skipped, unknown = [], 0, set()
for ln in src:
    s = ln.split('#')[0].strip()
    if not s:
        continue
    f = s.split()
    vid = int(f[0], 16)
    if len(f) > 1 and f[1].lower().startswith('0x'):
        pid, quirks = int(f[1], 16), f[2:]
    else:
        pid, quirks = 0xFFFF, f[1:]          # 厂商通配
    bits = 0
    for q in quirks:
        if q in SKIP:
            continue
        if q not in BITS:
            unknown.add(q); continue
        bits |= BITS[q][1]
    if bits == 0:
        skipped += 1
        continue
    rows.append((vid, pid, bits, ' '.join(quirks)))

rows.sort(key=lambda r: (r[0], r[1]))

out = []
out.append('''/*
 * CUPS USB quirks 表 —— 自动生成，请勿手改。
 *
 * 来源：OpenPrinting CUPS, backend/org.cups.usb-quirks
 *       https://github.com/OpenPrinting/cups  (Apache License 2.0)
 * 生成方式见 docs/HANDOFF-cloud-print.md「兼容性数据来源」一节。
 *
 * 只导入在本平台上说得通的 quirk：
 *   no-reattach —— Linux usblp 内核模块专用，ESP32 上没有这个概念，已丢弃
 *   whitelist   —— 只是「确认可用」的注记，不产生行为，已丢弃
 *
 * 这张表是【冷启动的起点】，不是完整的兼容性库：全表只有几十个可用条目，
 * 且高度偏向佳能和惠普的老喷墨。真正吃苦头的那些怪癖（作业结束符要不要发、
 * 唤醒等多久、URF 能不能断流）CUPS 里【没有】——因为在 Linux 上它们不发生。
 */
#pragma once
#include <stdint.h>

#define QK_BLACKLIST     0x01   /* CUPS 判定该机型不能用 USB 后端 */
#define QK_UNIDIR        0x02   /* 只支持单向 I/O：不要读 bulk IN、不要发 PJL */
#define QK_SOFT_RESET    0x04   /* 打印后需要 SOFT_RESET 收尾 */
#define QK_DELAY_CLOSE   0x08   /* 释放接口前要留延时 */
#define QK_USB_INIT      0x10   /* 需要厂商私有初始化串（本项目未实现） */
#define QK_VENDOR_CLASS  0x20   /* 接口是厂商私有 class/subclass，不是 7/1/x */
#define QK_NO_ALT_SET    0x40   /* 不要发 SET_INTERFACE */

typedef struct { uint16_t vid, pid; uint8_t flags; } usb_quirk_t;

#define QUIRK_PID_ANY 0xFFFF    /* 该厂商所有产品 */

static const usb_quirk_t CUPS_USB_QUIRKS[] = {''')

for vid, pid, bits, orig in rows:
    names = ' | '.join(BITS[q][0] for q in orig.split() if q in BITS)
    pidtxt = 'QUIRK_PID_ANY' if pid == 0xFFFF else '0x%04X' % pid
    out.append('    { 0x%04X, %-13s %-42s },  /* %s */' % (vid, pidtxt + ',', names, orig))

out.append('};')
out.append('')
out.append('#define CUPS_USB_QUIRKS_N (sizeof CUPS_USB_QUIRKS / sizeof CUPS_USB_QUIRKS[0])')
out.append('')
open(sys.argv[2], 'w').write('\n'.join(out))

print('生成 %d 条（丢弃 %d 条：quirk 全部与本平台无关）' % (len(rows), skipped))
if unknown:
    print('未识别的 quirk（已忽略，需人工确认）:', sorted(unknown))
