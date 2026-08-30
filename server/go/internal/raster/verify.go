// Package raster 是上传文档的唯一拦截点。
//
// 服务端不解析文档内容，设备也不认识格式——一份标称 URF 的 PDF 会被原样送进
// 打印机，用户收到几十张乱码纸。这里挡住它。
package raster

import (
	"encoding/binary"
	"fmt"
	"strings"
)

type Format int

const (
	FormatURF Format = iota
	FormatPWG
)

const (
	magicURF = "UNIRAST\x00"
	magicPWG = "RaS2"
)

type Info struct {
	Pages         int
	Width, Height int
}

func FormatFromContentType(ct string) (Format, bool) {
	switch strings.ToLower(strings.TrimSpace(strings.Split(ct, ";")[0])) {
	case "image/urf":
		return FormatURF, true
	case "image/pwg-raster":
		return FormatPWG, true
	}
	return 0, false
}

// Verify 只看头部几十个字节。三条校验：魔数、页数非 0、首页尺寸合理。
//
// 只需要 head，调用方传前 4KB 即可——上传是流式的，不要为了校验把整份读进内存。
func Verify(f Format, head []byte) (Info, error) {
	switch f {
	case FormatURF:
		return verifyURF(head)
	case FormatPWG:
		return verifyPWG(head)
	}
	return Info{}, fmt.Errorf("raster: 未知格式")
}

func verifyURF(b []byte) (Info, error) {
	if len(b) < 12+32 {
		return Info{}, fmt.Errorf("raster: 数据过短（%d 字节），不是完整的 URF", len(b))
	}
	if string(b[:8]) != magicURF {
		return Info{}, fmt.Errorf(`raster: 魔数不匹配，期望 UNIRAST\0，实际 %q`, preview(b))
	}
	pages := binary.BigEndian.Uint32(b[8:12])
	if pages == 0 {
		// tools/reference/render.py 的 fix_page_count 就是为这个写的：
		// Debian 版 cupsfilter 写 0，打印机据此认为文档为空。
		return Info{}, fmt.Errorf("raster: 页数字段为 0，打印机会认为文档为空")
	}
	w := binary.BigEndian.Uint32(b[24:28])
	h := binary.BigEndian.Uint32(b[28:32])
	if w == 0 || h == 0 || w >= 30000 || h >= 30000 {
		return Info{}, fmt.Errorf("raster: 首页尺寸不合理 %dx%d", w, h)
	}
	return Info{Pages: int(pages), Width: int(w), Height: int(h)}, nil
}

func verifyPWG(b []byte) (Info, error) {
	if len(b) < 4 {
		return Info{}, fmt.Errorf("raster: 数据过短")
	}
	if string(b[:4]) != magicPWG {
		return Info{}, fmt.Errorf("raster: 魔数不匹配，期望 RaS2，实际 %q", preview(b))
	}
	// PWG 的页头结构与 URF 不同，这里只认魔数。
	// 当前没有客户端在用 PWG 路径；真要用时补页数与尺寸校验。
	return Info{}, nil
}

func preview(b []byte) string {
	if len(b) > 8 {
		b = b[:8]
	}
	return strings.ToValidUTF8(string(b), "?")
}
