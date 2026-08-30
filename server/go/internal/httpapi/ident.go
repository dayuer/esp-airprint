package httpapi

import (
	"encoding/json"
	"log/slog"

	"github.com/dayuer/stickbox/server/go/internal/profile"
	"github.com/dayuer/stickbox/server/go/internal/store"
)

// identDoc 是设备上报的机型档案里我们关心的那部分。
//
// 全量档案有 4~16KB（第 0 层描述符 + 第 1 层 PJL 探针），这里只挑出
// 建档和下发 profile 需要的字段，其余原样落盘。字段名以
// docs/API-cloud-print.md 第 4.3 节为准。
type identDoc struct {
	Serial  string `json:"serial"`
	VID     string `json:"vid"`
	PID     string `json:"pid"`
	Make    string `json:"make"`
	Model   string `json:"model"`
	CMD     string `json:"cmd"`
	URFCaps string `json:"urf_caps"`

	// 老格式的兜底：早期固件把这些放在 printer_class 下面。
	PrinterClass struct {
		Serial  string `json:"serial"`
		Make    string `json:"make"`
		Model   string `json:"model"`
		CMD     string `json:"cmd"`
		URFCaps string `json:"urf"`
	} `json:"printer_class"`
}

func parseIdent(raw []byte) (store.Printer, bool) {
	var d identDoc
	if err := json.Unmarshal(raw, &d); err != nil {
		return store.Printer{}, false
	}
	pick := func(a, b string) string {
		if a != "" {
			return a
		}
		return b
	}
	p := store.Printer{
		Serial:  pick(d.Serial, d.PrinterClass.Serial),
		VID:     d.VID,
		PID:     d.PID,
		Make:    pick(d.Make, d.PrinterClass.Make),
		Model:   pick(d.Model, d.PrinterClass.Model),
		CMD:     pick(d.CMD, d.PrinterClass.CMD),
		URFCaps: pick(d.URFCaps, d.PrinterClass.URFCaps),
	}
	// 序列号是主键。没有它就建不了档——也区分不了
	// 「同一个桥换了打印机」和「同一台打印机换了桥」。
	return p, p.Serial != ""
}

// onIdent 在设备上报机型档案后建档并下发怪癖档案。
//
// 失败不影响 ident 上报本身返回 200：档案已经落盘了，下发失败下次重连
// 还会重来（profile 是 retain 消息）。让上报失败反而会让设备反复重试。
func (a *API) onIdent(dev string, raw []byte) {
	p, ok := parseIdent(raw)
	if !ok {
		slog.Warn("ident 里没有打印机序列号，跳过建档", "dev", dev)
		return
	}
	p.LastDev = dev
	if err := a.store.UpsertPrinter(p); err != nil {
		slog.Error("打印机建档失败", "dev", dev, "serial", p.Serial, "err", err)
		return
	}

	prof := profile.Lookup(a.store, p.Serial, p.VID, p.PID, p.Model)
	// 下发前把关：不能让畸形档案传到设备上才发现。
	if err := profile.Validate(prof); err != nil {
		slog.Error("查到的档案不合法，改发默认档案", "serial", p.Serial, "err", err)
		prof = profile.Default(p.Serial)
	}
	body, err := profile.Marshal(prof)
	if err != nil {
		slog.Error("档案编码失败", "serial", p.Serial, "err", err)
		return
	}
	if a.pub == nil {
		return
	}
	if err := a.pub.PublishProfile(dev, body); err != nil {
		slog.Error("档案下发失败", "dev", dev, "err", err)
		return
	}
	slog.Info("已下发怪癖档案", "dev", dev, "serial", p.Serial,
		"src", prof.Src, "bytes", len(body))
}
