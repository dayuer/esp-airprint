package raster

import (
	"errors"
	"strconv"
	"strings"
)

type PageSize struct {
	W int `json:"w_px"`
	H int `json:"h_px"`
}

// Profile 是下发给 App 的光栅参数。机型知识仍收敛在服务端，
// 只是形态从「拿 PPD 渲染」变成「把参数下发给客户端渲染」。
type Profile struct {
	Caps   string              `json:"urf_caps"`
	DPI    int                 `json:"dpi"`
	Color  string              `json:"color"`
	Format string              `json:"format"`
	Pages  map[string]PageSize `json:"pages"`
}

// 纸张物理尺寸（英寸）。乘以 dpi 得到像素。
var sheets = map[string][2]float64{
	"a4":     {8.27, 11.69},
	"letter": {8.5, 11.0},
}

// ParseCaps 从 IEEE-1284 的 URF 能力串解析光栅参数。
// 例：CP1,IS1-4-5-19,MT1-2-3-4-5-6,RS600,V1.4,W8
//
// 只认真正影响光栅的两个字段：RS（分辨率）和 W/SRGB（色彩）。
// 其余字段（CP 份数、IS 进纸盒、MT 介质类型）与光栅无关，忽略。
func ParseCaps(caps string) (Profile, error) {
	p := Profile{Caps: caps, Format: "urf", Color: "gray8",
		Pages: map[string]PageSize{}}
	for _, f := range strings.Split(caps, ",") {
		f = strings.TrimSpace(f)
		switch {
		case strings.HasPrefix(f, "RS"):
			// RS600 或 RS300-600，取最高的
			for _, v := range strings.Split(f[2:], "-") {
				if n, err := strconv.Atoi(v); err == nil && n > p.DPI {
					p.DPI = n
				}
			}
		case f == "SRGB24":
			p.Color = "srgb24"
		case f == "W8":
			p.Color = "gray8"
		}
	}
	if p.DPI <= 0 {
		return Profile{}, errors.New("raster: 能力串里没有 RS 字段，推不出光栅尺寸")
	}
	for name, wh := range sheets {
		p.Pages[name] = PageSize{
			W: int(wh[0] * float64(p.DPI)),
			H: int(wh[1] * float64(p.DPI)),
		}
	}
	return p, nil
}
