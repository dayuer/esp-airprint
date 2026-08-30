package httpapi

import (
	"encoding/json"
	"testing"
)

type devicesResp struct {
	Devices []struct {
		Dev        string `json:"dev"`
		Name       string `json:"name"`
		Online     bool   `json:"online"`
		State      string `json:"state"`
		Bound      int64  `json:"bound"`
		QueuedJobs int    `json:"queued_jobs"`
		Printer    *struct {
			Serial   string `json:"serial"`
			Make     string `json:"make"`
			Model    string `json:"model"`
			Attached bool   `json:"attached"`
		} `json:"printer"`
	} `json:"devices"`
}

// 一台设备都没有时返回空数组，不是 404。
// 「没有设备」是正常状态——App 靠这个区分「新用户」和「出问题了」。
func TestDevicesEmptyIsNotNotFound(t *testing.T) {
	h, dep := newTestAPI(t)
	token := login(t, h, dep, "13800008888")

	rr := get(h, "/api/devices", bearer(token))
	if rr.Code != 200 {
		t.Fatalf("GET /api/devices = %d %s", rr.Code, rr.Body)
	}
	var out devicesResp
	if err := json.Unmarshal(rr.Body.Bytes(), &out); err != nil {
		t.Fatalf("响应不是合法 JSON：%s", rr.Body)
	}
	if out.Devices == nil {
		t.Error("devices 必须是 []，不能是 null——App 会对它做 .map")
	}
	if len(out.Devices) != 0 {
		t.Errorf("新账号不该有设备，拿到 %d 台", len(out.Devices))
	}
}

// enroll 之后设备要出现在列表里，带上 enroll 时给的名字。
//
// 这条是整个端点存在的理由：dev 只在配网那一刻从 SoftAP 读到过一次，
// 之后 App 只能靠这个列表把它找回来。
func TestDevicesListsEnrolled(t *testing.T) {
	h, dep := newTestAPI(t)
	token := login(t, h, dep, "13800008888")

	post(h, "/api/device/enroll", `{"dev":"f412fa87c9e0","name":"工位打印机"}`, bearer(token))
	post(h, "/api/device/enroll", `{"dev":"aabbccddeeff","name":"仓库备用机"}`, bearer(token))

	rr := get(h, "/api/devices", bearer(token))
	var out devicesResp
	json.Unmarshal(rr.Body.Bytes(), &out)
	if len(out.Devices) != 2 {
		t.Fatalf("应有 2 台，拿到 %d：%s", len(out.Devices), rr.Body)
	}
	byDev := map[string]string{}
	for _, d := range out.Devices {
		byDev[d.Dev] = d.Name
		if d.Online {
			t.Errorf("%s 没连过 MQTT，不该是在线", d.Dev)
		}
		if d.State != "offline" {
			t.Errorf("%s state=%q，期望 offline", d.Dev, d.State)
		}
		if d.Printer != nil {
			t.Errorf("%s 没插打印机时 printer 必须是 null", d.Dev)
		}
		if d.Bound == 0 {
			t.Errorf("%s 没有绑定时间", d.Dev)
		}
	}
	if byDev["f412fa87c9e0"] != "工位打印机" || byDev["aabbccddeeff"] != "仓库备用机" {
		t.Errorf("名字对不上：%v", byDev)
	}
}

// 只能看到自己的设备。
func TestDevicesIsolatedPerUser(t *testing.T) {
	h, dep := newTestAPI(t)
	a := login(t, h, dep, "13800008888")
	b := login(t, h, dep, "13900009999")

	post(h, "/api/device/enroll", `{"dev":"f412fa87c9e0","name":"A 的"}`, bearer(a))

	rr := get(h, "/api/devices", bearer(b))
	var out devicesResp
	json.Unmarshal(rr.Body.Bytes(), &out)
	if len(out.Devices) != 0 {
		t.Errorf("B 不该看到 A 的设备：%s", rr.Body)
	}
}

// 重新 enroll（也就是重置）之后，列表里仍然只有一台，名字是新的。
func TestDevicesAfterReEnroll(t *testing.T) {
	h, dep := newTestAPI(t)
	token := login(t, h, dep, "13800008888")

	post(h, "/api/device/enroll", `{"dev":"f412fa87c9e0","name":"旧名字"}`, bearer(token))
	post(h, "/api/device/enroll", `{"dev":"f412fa87c9e0","name":"新名字"}`, bearer(token))

	rr := get(h, "/api/devices", bearer(token))
	var out devicesResp
	json.Unmarshal(rr.Body.Bytes(), &out)
	if len(out.Devices) != 1 {
		t.Fatalf("重置后仍应只有 1 台，拿到 %d：%s", len(out.Devices), rr.Body)
	}
	// 旧密钥被吊销、新的生效，取的必须是那条还有效的。
	if out.Devices[0].Name != "新名字" {
		t.Errorf("名字 = %q，期望「新名字」", out.Devices[0].Name)
	}
}

// 解绑之后从列表里消失。
func TestDevicesAfterUnbind(t *testing.T) {
	h, dep := newTestAPI(t)
	token := login(t, h, dep, "13800008888")

	post(h, "/api/device/enroll", `{"dev":"f412fa87c9e0","name":"x"}`, bearer(token))
	post(h, "/api/device/f412fa87c9e0/unbind", `{}`, bearer(token))

	rr := get(h, "/api/devices", bearer(token))
	var out devicesResp
	json.Unmarshal(rr.Body.Bytes(), &out)
	if len(out.Devices) != 0 {
		t.Errorf("解绑后不该还在列表里：%s", rr.Body)
	}
}

// 这个端点不需要 X-Device——它就是用来得知有哪些 dev 的。
func TestDevicesNeedsNoXDevice(t *testing.T) {
	h, dep := newTestAPI(t)
	token := login(t, h, dep, "13800008888")
	post(h, "/api/device/enroll", `{"dev":"f412fa87c9e0","name":"x"}`, bearer(token))

	if rr := get(h, "/api/devices", bearer(token)); rr.Code != 200 {
		t.Errorf("不带 X-Device 就该能用，拿到 %d", rr.Code)
	}
}

// device 角色不能调——它是 app 专用的。
func TestDevicesRejectsDeviceRole(t *testing.T) {
	h, dep := newTestAPI(t)
	token := login(t, h, dep, "13800008888")
	rr := post(h, "/api/device/enroll", `{"dev":"f412fa87c9e0","name":"x"}`, bearer(token))
	var en struct {
		DeviceKey string `json:"device_key"`
	}
	json.Unmarshal(rr.Body.Bytes(), &en)

	if rr := get(h, "/api/devices", bearer(en.DeviceKey)); rr.Code != 401 {
		t.Errorf("device 密钥调 /api/devices = %d，期望 401", rr.Code)
	}
}
