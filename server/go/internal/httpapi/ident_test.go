package httpapi

import (
	"encoding/json"
	"testing"

	"github.com/dayuer/stickbox/server/go/internal/profile"
)

const identBody = `{"serial":"CNB9K1P2X4","vid":"03F0","pid":"F22A",` +
	`"make":"HP","model":"HP Laser MFP 136a",` +
	`"cmd":"URF,PCL,PJL,PWGRaster","urf_caps":"CP1,RS600,V1.4,W8"}`

// 上报机型档案 → 建档 → 下发怪癖档案，一条龙。
func TestIdentBuildsPrinterAndPublishesProfile(t *testing.T) {
	h, dep := newTestAPI(t)
	appTok := login(t, h, dep, "13800008888")
	devKey := enroll(t, h, appTok, "f412fa87c9e0")

	if rr := post(h, "/api/device/f412fa87c9e0/ident", identBody, bearer(devKey)); rr.Code != 200 {
		t.Fatalf("上报 = %d %s", rr.Code, rr.Body)
	}

	p, ok, _ := dep.store.GetPrinter("CNB9K1P2X4")
	if !ok {
		t.Fatal("打印机没建档")
	}
	if p.Model != "HP Laser MFP 136a" || p.LastDev != "f412fa87c9e0" {
		t.Errorf("建档内容 = %+v", p)
	}

	body, ok := dep.pub.profiles["f412fa87c9e0"]
	if !ok {
		t.Fatal("没有下发怪癖档案")
	}
	prof, err := profile.Unmarshal(body)
	if err != nil {
		t.Fatalf("下发的档案不是合法 JSON：%v", err)
	}
	// 档案必须带序列号——设备靠它判断该不该套用（换了打印机就该忽略）
	if prof.Serial != "CNB9K1P2X4" {
		t.Errorf("档案里的 serial = %q", prof.Serial)
	}
	if prof.Src != profile.SrcDefault {
		t.Errorf("首次上报应拿到默认档案，得到 %q", prof.Src)
	}
	// 默认必须发 UEL——不发只能打第一份
	if len(prof.Hooks.JobEnd) != 1 || prof.Hooks.JobEnd[0].Data != profile.UEL {
		t.Errorf("默认档案没发 UEL：%+v", prof.Hooks.JobEnd)
	}
	// 上限是硬约束，设备可用堆只有几十 KB
	if len(body) > profile.MaxBytes {
		t.Errorf("下发了 %d 字节，超过 %d", len(body), profile.MaxBytes)
	}
}

// 没有序列号就建不了档——那是区分「换打印机」和「换桥」的唯一依据。
// 但 ident 本身仍要落盘并返回 200，否则设备会反复重试。
func TestIdentWithoutSerialStillSucceeds(t *testing.T) {
	h, dep := newTestAPI(t)
	appTok := login(t, h, dep, "13800008888")
	devKey := enroll(t, h, appTok, "f412fa87c9e0")

	rr := post(h, "/api/device/f412fa87c9e0/ident", `{"make":"HP","model":"X"}`, bearer(devKey))
	if rr.Code != 200 {
		t.Fatalf("= %d %s", rr.Code, rr.Body)
	}
	if len(dep.pub.profiles) != 0 {
		t.Error("没有序列号却下发了档案")
	}
}

// 老格式（字段在 printer_class 下面）也要认。
func TestIdentAcceptsNestedFormat(t *testing.T) {
	h, dep := newTestAPI(t)
	appTok := login(t, h, dep, "13800008888")
	devKey := enroll(t, h, appTok, "f412fa87c9e0")

	body := `{"vid":"03F0","pid":"F22A","printer_class":{"serial":"S1",` +
		`"make":"HP","model":"M","urf":"RS600,W8"}}`
	if rr := post(h, "/api/device/f412fa87c9e0/ident", body, bearer(devKey)); rr.Code != 200 {
		t.Fatalf("= %d %s", rr.Code, rr.Body)
	}
	p, ok, _ := dep.store.GetPrinter("S1")
	if !ok || p.URFCaps != "RS600,W8" {
		t.Errorf("嵌套格式没解析出来：%+v ok=%v", p, ok)
	}
}

// 换打印机：两台各自建档，档案跟着当前这台走。
func TestIdentTwoPrintersOnSameBridge(t *testing.T) {
	h, dep := newTestAPI(t)
	appTok := login(t, h, dep, "13800008888")
	devKey := enroll(t, h, appTok, "f412fa87c9e0")

	post(h, "/api/device/f412fa87c9e0/ident", identBody, bearer(devKey))
	second := `{"serial":"OTHER-1","vid":"04A9","pid":"1234","make":"Canon",` +
		`"model":"LBP","urf_caps":"RS600,W8"}`
	post(h, "/api/device/f412fa87c9e0/ident", second, bearer(devKey))

	list, err := dep.store.PrintersOfDevice("f412fa87c9e0")
	if err != nil {
		t.Fatal(err)
	}
	if len(list) != 2 {
		t.Fatalf("这个桥见过 %d 台，期望 2", len(list))
	}
	prof, _ := profile.Unmarshal(dep.pub.profiles["f412fa87c9e0"])
	if prof.Serial != "OTHER-1" {
		t.Errorf("最后下发的档案对应 %q，期望当前这台 OTHER-1", prof.Serial)
	}
}

func TestPrinterEndpointShowsProfileSource(t *testing.T) {
	h, dep := newTestAPI(t)
	appTok := login(t, h, dep, "13800008888")
	devKey := enroll(t, h, appTok, "f412fa87c9e0")
	post(h, "/api/device/f412fa87c9e0/ident", identBody, bearer(devKey))

	rr := get(h, "/api/device/f412fa87c9e0/printer", bearer(appTok))
	if rr.Code != 200 {
		t.Fatalf("= %d %s", rr.Code, rr.Body)
	}
	var out struct {
		Printer struct {
			Serial string `json:"serial"`
			Model  string `json:"model"`
		} `json:"printer"`
		Profile struct {
			Src string `json:"src"`
		} `json:"profile"`
	}
	json.Unmarshal(rr.Body.Bytes(), &out)
	if out.Printer.Serial != "CNB9K1P2X4" || out.Printer.Model != "HP Laser MFP 136a" {
		t.Errorf("printer = %+v", out.Printer)
	}
	// src 告诉用户这份配置的可信度：default 就该提示做一次测试
	if out.Profile.Src != "default" {
		t.Errorf("src = %q，期望 default", out.Profile.Src)
	}
}

func TestPrinterEndpoint404WithoutIdent(t *testing.T) {
	h, dep := newTestAPI(t)
	token := login(t, h, dep, "13800008888")
	enroll(t, h, token, "f412fa87c9e0")
	if rr := get(h, "/api/device/f412fa87c9e0/printer", bearer(token)); rr.Code != 404 {
		t.Errorf("= %d，期望 404", rr.Code)
	}
}
