#!/usr/bin/env python3
"""打印 .urf 文件的头部字段。用来核对编码器写的常量和真实产物是否一致。

    ./urfdump.py a.urf

字节布局的依据是 tools/reference/render.py 的 fix_page_count。
"""
import struct
import sys


def dump(path):
    d = open(path, 'rb').read()
    if d[:8] != b'UNIRAST\0':
        print('不是 URF：前 8 字节是 %r' % d[:8])
        return 1
    print('文件      %s  %d 字节' % (path, len(d)))
    print('页数字段  %d' % struct.unpack('>I', d[8:12])[0])

    pos, page = 12, 0
    while pos + 32 <= len(d):
        h = d[pos:pos + 32]
        w, ht = struct.unpack('>II', h[12:20])
        if not (0 < w < 30000 and 0 < ht < 30000):
            break
        page += 1
        dpi = struct.unpack('>I', h[20:24])[0]
        print('第 %d 页  bpp=%d colorspace=%d duplex=%d quality=%d '
              '尺寸=%dx%d dpi=%d' % (page, h[0], h[1], h[2], h[3], w, ht, dpi))
        print('        保留字节 [4:12]=%s [24:32]=%s'
              % (h[4:12].hex(), h[24:32].hex()))

        pos += 32
        rows = 0
        while rows < ht and pos < len(d):
            pos += 1
            rows += 1
            while pos < len(d):
                n = d[pos]
                pos += 1
                if n == 128:
                    break
                pos += 1 if n < 128 else (257 - n)
        print('        行数据扫出 %d 行' % rows)
    print('实际页数  %d' % page)
    return 0


if __name__ == '__main__':
    sys.exit(dump(sys.argv[1]))
