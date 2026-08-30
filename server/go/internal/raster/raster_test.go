package raster

import (
	"encoding/binary"
	"testing"
)

// urf 拼一份最小可用的 URF：魔数 + 页数 + 一个 32 字节页头。
func urf(pages uint32, w, h uint32) []byte {
	b := make([]byte, 12+32)
	copy(b, magicURF)
	binary.BigEndian.PutUint32(b[8:12], pages)
	binary.BigEndian.PutUint32(b[24:28], w)
	binary.BigEndian.PutUint32(b[28:32], h)
	return b
}

func TestVerifyAcceptsGoodURF(t *testing.T) {
	info, err := Verify(FormatURF, urf(2, 4962, 7014))
	if err != nil {
		t.Fatalf("合法 URF 被拒：%v", err)
	}
	if info.Pages != 2 || info.Width != 4962 || info.Height != 7014 {
		t.Errorf("解析结果 = %+v", info)
	}
}

// 最要命的一条：把 PDF 当 URF 传。
func TestVerifyRejectsPDFClaimingURF(t *testing.T) {
	_, err := Verify(FormatURF, []byte("%PDF-1.7\n%\xc7\xec\x8f\xa2\n1 0 obj\n<<>>"))
	if err == nil {
		t.Fatal("PDF 冒充 URF 竟然通过了——用户会收到几十张乱码纸")
	}
}

// 页数为 0 时打印机认为文档为空，什么都不打。
func TestVerifyRejectsZeroPages(t *testing.T) {
	if _, err := Verify(FormatURF, urf(0, 4962, 7014)); err == nil {
		t.Error("页数 0 应被拒")
	}
}

func TestVerifyRejectsInsaneDimensions(t *testing.T) {
	for _, c := range []struct{ w, h uint32 }{{0, 7014}, {4962, 0}, {40000, 7014}} {
		if _, err := Verify(FormatURF, urf(1, c.w, c.h)); err == nil {
			t.Errorf("尺寸 %dx%d 应被拒", c.w, c.h)
		}
	}
}

func TestVerifyRejectsTruncated(t *testing.T) {
	if _, err := Verify(FormatURF, []byte("UNIRAST\x00\x00")); err == nil {
		t.Error("截断的数据应被拒")
	}
}

func TestVerifyPWG(t *testing.T) {
	b := make([]byte, 64)
	copy(b, magicPWG)
	if _, err := Verify(FormatPWG, b); err != nil {
		t.Errorf("合法 PWG 被拒：%v", err)
	}
	if _, err := Verify(FormatPWG, []byte("RaS9xxxx")); err == nil {
		t.Error("错误魔数应被拒")
	}
}

func TestFormatFromContentType(t *testing.T) {
	for ct, want := range map[string]Format{
		"image/urf":                 FormatURF,
		"image/pwg-raster":          FormatPWG,
		"IMAGE/URF; charset=binary": FormatURF,
	} {
		if got, ok := FormatFromContentType(ct); !ok || got != want {
			t.Errorf("%q → %v ok=%v", ct, got, ok)
		}
	}
	for _, ct := range []string{"application/pdf", "image/png", "text/plain", ""} {
		if _, ok := FormatFromContentType(ct); ok {
			t.Errorf("%q 不该被接受", ct)
		}
	}
}

// 这串来自实测：HP Laser MFP 136a 的 IEEE-1284 URF 能力字段。
const caps = "CP1,IS1-4-5-19,MT1-2-3-4-5-6,RS600,V1.4,W8"

func TestParseCaps(t *testing.T) {
	p, err := ParseCaps(caps)
	if err != nil {
		t.Fatal(err)
	}
	if p.DPI != 600 {
		t.Errorf("DPI = %d，期望 600（RS600）", p.DPI)
	}
	if p.Color != "gray8" {
		t.Errorf("Color = %q，期望 gray8（W8）", p.Color)
	}
	if a4 := p.Pages["a4"]; a4.W != 4962 || a4.H != 7014 {
		t.Errorf("A4 = %dx%d，期望 4962x7014", a4.W, a4.H)
	}
}

// 打印机只报了 RS300 就该是 300，不能硬编码 600。
func TestParseCapsHonorsReportedDPI(t *testing.T) {
	p, err := ParseCaps("RS300,W8")
	if err != nil {
		t.Fatal(err)
	}
	if p.DPI != 300 {
		t.Errorf("DPI = %d，期望 300", p.DPI)
	}
	if p.Pages["a4"].W != 2481 {
		t.Errorf("300dpi 的 A4 宽 = %d，期望 2481", p.Pages["a4"].W)
	}
}

// 多个分辨率时取最高的——能力串可能写 RS300-600。
func TestParseCapsPicksHighestDPI(t *testing.T) {
	p, _ := ParseCaps("RS300-600,W8")
	if p.DPI != 600 {
		t.Errorf("DPI = %d，期望取最高的 600", p.DPI)
	}
}

func TestParseCapsRejectsGarbage(t *testing.T) {
	for _, in := range []string{"", "CP1,V1.4", "nonsense"} {
		if _, err := ParseCaps(in); err == nil {
			t.Errorf("ParseCaps(%q) 应当报错——没有 RS 字段就推不出尺寸", in)
		}
	}
}
