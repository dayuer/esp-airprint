#!/usr/bin/env python3
"""把任意文档渲染成打印机认的 URF。

⚠ 不再部署。服务端已改为纯管道，光栅由 App 用手机算力完成（见
docs/superpowers/specs/2026-08-30-go-print-server-design.md 第 7 节）。

留档的理由只有一个：下面的 fix_page_count 是客户端 URF 编码器的参考实现。
Debian 版 cupsfilter 把 URF 头部页数字段写成 0，打印机据此认为文档为空——
自己写编码器一样会踩这个坑。移植它，不要重新发现它。


用法: render.py <输入文件> <输出.urf>

Debian 版 cupsfilter 产出的 URF 头部页数字段是 0（macOS 版给的是实际页数），
打印机据此可能认为文档为空。这里扫描一遍真实页数并回填。
"""
import subprocess, sys, struct, os

PPD = '/opt/stickbox/ppd/hp136a.ppd'

def is_text(path):
    """能按 UTF-8 解码且无控制字符 = 当文本处理"""
    try:
        d = open(path, 'rb').read(8192)
        if b'\x00' in d: return False
        d.decode('utf-8')
        return True
    except Exception:
        return False

def to_urf(src):
    low = src.lower()
    if not low.endswith('.pdf'):
        pdf = src + '.pdf'
        if is_text(src):
            # 文本走 PangoCairo：CUPS 的 texttopdf 不带 CJK 字体，
            # Debian 的 paps 0.6.7 又写死 /Helvetica，两者中文都是方框。
            subprocess.run(['python3', '/opt/stickbox/bin/text2pdf.py', src, pdf],
                           capture_output=True, check=True)
        else:
            with open(pdf, 'wb') as f:
                subprocess.run(['cupsfilter', '-m', 'application/pdf', src],
                               stdout=f, stderr=subprocess.DEVNULL, check=True)
        src = pdf
    r = subprocess.run(['cupsfilter', '-P', PPD, '-m', 'image/urf', src],
                       capture_output=True)
    if r.returncode or not r.stdout:
        raise RuntimeError('渲染失败: ' + r.stderr.decode()[:300])
    return r.stdout

def fix_page_count(d):
    """扫描 URF 页头，把头部的页数字段回填成真实值"""
    if d[:8] != b'UNIRAST\0':
        raise RuntimeError('不是合法 URF')
    declared = struct.unpack('>I', d[8:12])[0]
    # 逐页扫：每页 32 字节页头，之后是 RLE 行数据
    pos, pages = 12, 0
    while pos + 32 <= len(d):
        h = d[pos:pos+32]
        w, ht = struct.unpack('>II', h[12:20])
        if not (0 < w < 30000 and 0 < ht < 30000):
            break
        pages += 1
        pos += 32
        rows = 0
        while rows < ht and pos < len(d):
            pos += 1                      # 行重复计数
            rows += 1
            while pos < len(d):           # 该行的 RLE 包
                n = d[pos]; pos += 1
                if n == 128: break
                pos += 1 if n < 128 else (257 - n)
    if pages and pages != declared:
        d = d[:8] + struct.pack('>I', pages) + d[12:]
    return d, declared, pages

if __name__ == '__main__':
    out = to_urf(sys.argv[1])
    out, declared, actual = fix_page_count(out)
    open(sys.argv[2], 'wb').write(out)
    print('URF %d 字节  页数 %d->%d' % (len(out), declared, actual))
