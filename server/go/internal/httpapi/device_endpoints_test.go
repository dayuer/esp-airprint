package httpapi

import (
	"encoding/json"
	"os"
	"path/filepath"
	"testing"
	"time"
)

func TestEnrollIssuesDeviceKey(t *testing.T) {
	h, dep := newTestAPI(t)
	token := login(t, h, dep, "13800008888")

	rr := post(h, "/api/device/enroll", `{"dev":"f412fa87c9e0","name":"工位"}`, bearer(token))
	if rr.Code != 200 {
		t.Fatalf("enroll = %d %s", rr.Code, rr.Body)
	}
	var out struct {
		DeviceKey string `json:"device_key"`
		Reset     bool   `json:"reset"`
	}
	json.Unmarshal(rr.Body.Bytes(), &out)
	if out.DeviceKey == "" || out.Reset {
		t.Fatalf("首次 enroll 响应异常：%s", rr.Body)
	}
	id, err := dep.v.Verify(out.DeviceKey)
	if err != nil || id.Dev != "f412fa87c9e0" {
		t.Errorf("签发的密钥不可用：%+v err=%v", id, err)
	}
}

func TestEnrollRejectsBadDev(t *testing.T) {
	h, dep := newTestAPI(t)
	token := login(t, h, dep, "13800008888")
	for _, dev := range []string{"", "XYZ", "F412FA87C9E0", "f412fa87c9e"} {
		rr := post(h, "/api/device/enroll", `{"dev":"`+dev+`"}`, bearer(token))
		if rr.Code != 400 {
			t.Errorf("dev=%q → %d，期望 400", dev, rr.Code)
		}
	}
}

// 抢绑防护：别人的设备不能被绑走，否则是条现成的 DoS。
func TestEnrollRejectsOtherUsersDevice(t *testing.T) {
	h, dep := newTestAPI(t)
	tokenA := login(t, h, dep, "13800008888")
	enroll(t, h, tokenA, "f412fa87c9e0")

	dep.clock = dep.clock.Add(2 * time.Minute)
	tokenB := login(t, h, dep, "13900009999")
	rr := post(h, "/api/device/enroll", `{"dev":"f412fa87c9e0"}`, bearer(tokenB))
	if rr.Code != 409 {
		t.Errorf("抢绑 = %d，期望 409", rr.Code)
	}
}

// 重复 enroll 自己的设备 = 重置：旧密钥吊销，新密钥生效。
func TestEnrollTwiceIsReset(t *testing.T) {
	h, dep := newTestAPI(t)
	token := login(t, h, dep, "13800008888")
	first := enroll(t, h, token, "f412fa87c9e0")
	dep.v.Verify(first) // 先进缓存，确保吊销时缓存也被清

	rr := post(h, "/api/device/enroll", `{"dev":"f412fa87c9e0"}`, bearer(token))
	var out struct {
		DeviceKey string `json:"device_key"`
		Reset     bool   `json:"reset"`
	}
	json.Unmarshal(rr.Body.Bytes(), &out)
	if !out.Reset {
		t.Error("重复 enroll 应标记为 reset")
	}
	if out.DeviceKey == first {
		t.Fatal("重置后密钥没变")
	}
	if _, err := dep.v.Verify(first); err == nil {
		t.Error("旧设备密钥仍可用——重置没吊销它")
	}
	if _, err := dep.v.Verify(out.DeviceKey); err != nil {
		t.Errorf("新密钥不可用：%v", err)
	}
}

func TestUnbindReleasesDevice(t *testing.T) {
	h, dep := newTestAPI(t)
	token := login(t, h, dep, "13800008888")
	enroll(t, h, token, "f412fa87c9e0")

	if rr := post(h, "/api/device/f412fa87c9e0/unbind", `{}`, bearer(token)); rr.Code != 200 {
		t.Fatalf("解绑 = %d %s", rr.Code, rr.Body)
	}
	if _, ok, _ := dep.store.OwnerOfDevice("f412fa87c9e0"); ok {
		t.Error("解绑后设备仍有归属")
	}
	// 幂等
	if rr := post(h, "/api/device/f412fa87c9e0/unbind", `{}`, bearer(token)); rr.Code != 200 {
		t.Error("重复解绑应当幂等")
	}
	// 解绑后别人可以绑
	dep.clock = dep.clock.Add(2 * time.Minute)
	other := login(t, h, dep, "13900009999")
	if rr := post(h, "/api/device/enroll", `{"dev":"f412fa87c9e0"}`, bearer(other)); rr.Code != 200 {
		t.Errorf("解绑后新用户 enroll = %d", rr.Code)
	}
}

func writeIdent(t *testing.T, dep *testDeps, dev, body string) {
	t.Helper()
	dir := filepath.Join(dep.cfg.IdentsDir(), dev)
	if err := os.MkdirAll(dir, 0o755); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(dir, "latest.json"), []byte(body), 0o644); err != nil {
		t.Fatal(err)
	}
}

func TestRenderProfileFromIdent(t *testing.T) {
	h, dep := newTestAPI(t)
	token := login(t, h, dep, "13800008888")
	enroll(t, h, token, "f412fa87c9e0")
	writeIdent(t, dep, "f412fa87c9e0",
		`{"urf_caps":"CP1,RS600,V1.4,W8","serial":"CNB9K1P2X4"}`)

	rr := get(h, "/api/device/f412fa87c9e0/render-profile", bearer(token))
	if rr.Code != 200 {
		t.Fatalf("render-profile = %d %s", rr.Code, rr.Body)
	}
	var out struct {
		Serial string `json:"serial"`
		DPI    int    `json:"dpi"`
		Color  string `json:"color"`
		Pages  map[string]struct {
			W int `json:"w_px"`
			H int `json:"h_px"`
		} `json:"pages"`
	}
	json.Unmarshal(rr.Body.Bytes(), &out)
	if out.DPI != 600 || out.Color != "gray8" {
		t.Errorf("dpi=%d color=%q", out.DPI, out.Color)
	}
	// App 光栅前记下它，上传时用 X-Printer-Serial 带回来
	if out.Serial != "CNB9K1P2X4" {
		t.Errorf("serial = %q", out.Serial)
	}
	if a4 := out.Pages["a4"]; a4.W != 4962 || a4.H != 7014 {
		t.Errorf("A4 = %dx%d", a4.W, a4.H)
	}
}

// 没上报过 ident 就返回 404，App 不能用默认值蒙——尺寸蒙错就是废纸。
func TestRenderProfile404WithoutIdent(t *testing.T) {
	h, dep := newTestAPI(t)
	token := login(t, h, dep, "13800008888")
	enroll(t, h, token, "f412fa87c9e0")
	if rr := get(h, "/api/device/f412fa87c9e0/render-profile", bearer(token)); rr.Code != 404 {
		t.Errorf("= %d，期望 404", rr.Code)
	}
}

func TestRenderProfileRejectsForeignDevice(t *testing.T) {
	h, dep := newTestAPI(t)
	token := login(t, h, dep, "13800008888")
	if rr := get(h, "/api/device/aaaaaaaaaaaa/render-profile", bearer(token)); rr.Code != 403 {
		t.Errorf("= %d，期望 403", rr.Code)
	}
}

// 用户看不到排队数就会以为打印失败了。
func TestPrintersListsQueuedPerPrinter(t *testing.T) {
	h, dep := newTestAPI(t)
	token := login(t, h, dep, "13800008888")
	enroll(t, h, token, "f412fa87c9e0")
	upload(h, token, "f412fa87c9e0", "PRINTER-A", "image/urf", urfBytes(1, 4962, 7014))
	upload(h, token, "f412fa87c9e0", "PRINTER-B", "image/urf", urfBytes(1, 4962, 7014))
	upload(h, token, "f412fa87c9e0", "PRINTER-B", "image/urf", urfBytes(1, 4962, 7014))

	rr := get(h, "/api/device/f412fa87c9e0/printers", bearer(token))
	if rr.Code != 200 {
		t.Fatalf("= %d %s", rr.Code, rr.Body)
	}
	var out struct {
		Printers []struct {
			Serial     string `json:"serial"`
			QueuedJobs int    `json:"queued_jobs"`
		} `json:"printers"`
	}
	json.Unmarshal(rr.Body.Bytes(), &out)
	got := map[string]int{}
	for _, p := range out.Printers {
		got[p.Serial] = p.QueuedJobs
	}
	if got["PRINTER-A"] != 1 || got["PRINTER-B"] != 2 {
		t.Errorf("排队统计 = %+v，期望 A:1 B:2", got)
	}
}

func TestStatusScopedToOwnDevice(t *testing.T) {
	h, dep := newTestAPI(t)
	token := login(t, h, dep, "13800008888")
	enroll(t, h, token, "f412fa87c9e0")

	rr := get(h, "/api/status", map[string]string{
		"Authorization": "Bearer " + token, "X-Device": "f412fa87c9e0"})
	if rr.Code != 200 {
		t.Fatalf("= %d %s", rr.Code, rr.Body)
	}
	rr = get(h, "/api/status", map[string]string{
		"Authorization": "Bearer " + token, "X-Device": "aaaaaaaaaaaa"})
	if rr.Code != 403 {
		t.Errorf("查别人的设备 = %d，期望 403", rr.Code)
	}
}
