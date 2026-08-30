#!/usr/bin/env python3
"""文本 → PDF，用 PangoCairo 排版。

为什么不用 CUPS 的 texttopdf 或 paps：
  - texttopdf 不带 CJK 字体，中文变方框
  - Debian 的 paps 是 0.6.7，输出 PostScript 时写死 /Helvetica，同样是方框
PangoCairo 是 Linux 上的标准文本排版引擎，走 fontconfig，中英混排/换行都正确。
"""
import sys, gi
gi.require_version('Pango', '1.0')
gi.require_version('PangoCairo', '1.0')
from gi.repository import Pango, PangoCairo
import cairo

A4_W, A4_H = 595.0, 842.0      # 点
MARGIN     = 56.0              # 约 2cm
FONT       = 'Noto Sans CJK SC 10.5'

def render(src, dst):
    text = open(src, 'rb').read().decode('utf-8', 'replace')
    surf = cairo.PDFSurface(dst, A4_W, A4_H)
    ctx  = cairo.Context(surf)
    layout = PangoCairo.create_layout(ctx)
    layout.set_font_description(Pango.FontDescription(FONT))
    layout.set_width(int((A4_W - 2 * MARGIN) * Pango.SCALE))
    layout.set_wrap(Pango.WrapMode.WORD_CHAR)
    layout.set_text(text, -1)

    # 按页高切分：逐行累积，超出就翻页
    it = layout.get_iter()
    page_top, lines = 0, []
    usable = A4_H - 2 * MARGIN
    while True:
        line = it.get_line_readonly()
        ink, log = it.get_line_extents()
        y  = log.y / Pango.SCALE
        h  = log.height / Pango.SCALE
        if y + h - page_top > usable and lines:
            _flush(ctx, lines, page_top); surf.show_page(); ctx = cairo.Context(surf)
            page_top = y; lines = []
        lines.append((line, y, log.x / Pango.SCALE))
        if not it.next_line():
            break
    if lines:
        _flush(ctx, lines, page_top)
    surf.finish()

def _flush(ctx, lines, page_top):
    for line, y, x in lines:
        ctx.move_to(MARGIN + x, MARGIN + (y - page_top))
        PangoCairo.show_layout_line(ctx, line)

if __name__ == '__main__':
    render(sys.argv[1], sys.argv[2])
    print('OK')
