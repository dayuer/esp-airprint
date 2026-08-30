package integration

import (
	"testing"
	"time"

	"github.com/dayuer/stickbox/server/go/internal/profile"
)

// 完整链路：登录 → enroll → 上传 URF → 派发信令 → 设备取件 → 回执。
//
// 单测都过不代表串起来能用——mochi 的 Serve() 非阻塞那个 bug 就是
// 只有真跑起来才暴露的。
func TestEndToEndPrintFlow(t *testing.T) {
	e := startServer(t)

	token := e.login("13800008888")
	devKey := e.enroll(token, "f412fa87c9e0")

	dev := e.connectDevice("f412fa87c9e0", devKey)
	dev.heartbeat("ready", "", "CNB9K1P2X4")

	jid := e.upload(token, "f412fa87c9e0", "CNB9K1P2X4", urfBytes(2, 4962, 7014))

	got := dev.waitJob(5 * time.Second)
	if got.ID != jid {
		t.Fatalf("派发的作业 = %s，期望 %s", got.ID, jid)
	}

	code, body := e.req("GET", "/api/job/"+jid+"/data", "",
		map[string]string{"Authorization": "Bearer " + devKey})
	if code != 200 {
		t.Fatalf("取件 = %d %s", code, body)
	}
	if int64(len(body)) != got.Size {
		t.Errorf("取回 %d 字节，信令说 %d", len(body), got.Size)
	}

	dev.heartbeat("done", jid, "CNB9K1P2X4")
	e.waitJobState(jid, "done", 5*time.Second)
}

// 一次只派一件：设备没有本地队列，堆过去等于丢件。
func TestEndToEndDispatchesOneAtATime(t *testing.T) {
	e := startServer(t)
	token := e.login("13800008888")
	devKey := e.enroll(token, "f412fa87c9e0")
	dev := e.connectDevice("f412fa87c9e0", devKey)
	dev.heartbeat("ready", "", "PA")

	first := e.upload(token, "f412fa87c9e0", "PA", urfBytes(1, 4962, 7014))
	second := e.upload(token, "f412fa87c9e0", "PA", urfBytes(1, 4962, 7014))

	if got := dev.waitJob(5 * time.Second); got.ID != first {
		t.Fatalf("先派了 %s，期望 %s", got.ID, first)
	}
	select {
	case js := <-dev.jobs:
		t.Fatalf("第一件还没结束就派了第二件 %s", js.ID)
	case <-time.After(500 * time.Millisecond):
	}

	dev.heartbeat("done", first, "PA")
	if got := dev.waitJob(5 * time.Second); got.ID != second {
		t.Fatalf("回执后派了 %s，期望 %s", got.ID, second)
	}
}

// 换打印机后，为旧机器光栅的作业不能派给新机器。
func TestEndToEndPrinterSwap(t *testing.T) {
	e := startServer(t)
	token := e.login("13800008888")
	devKey := e.enroll(token, "f412fa87c9e0")
	dev := e.connectDevice("f412fa87c9e0", devKey)

	dev.heartbeat("ready", "", "PRINTER-A")
	jidA := e.upload(token, "f412fa87c9e0", "PRINTER-A", urfBytes(1, 4962, 7014))
	if got := dev.waitJob(5 * time.Second); got.ID != jidA {
		t.Fatalf("首次派了 %s", got.ID)
	}

	// 用户换了台打印机
	dev.heartbeat("ready", "", "PRINTER-B")
	jidB := e.upload(token, "f412fa87c9e0", "PRINTER-B", urfBytes(1, 4962, 7014))

	if got := dev.waitJob(5 * time.Second); got.ID != jidB {
		t.Fatalf("换机后派了 %s，期望 %s（A 机的作业必须留在队列里）", got.ID, jidB)
	}
	// A 机的作业留着等它插回来，不失败也不删
	e.waitJobState(jidA, "queued", 5*time.Second)
}

// ACL：设备不能订阅别人的 topic。
func TestEndToEndACLBlocksCrossDevice(t *testing.T) {
	e := startServer(t)
	token := e.login("13800008888")
	devKey := e.enroll(token, "f412fa87c9e0")
	dev := e.connectDevice("f412fa87c9e0", devKey)

	if err := dev.subscribe("printer/aaaaaaaaaaaa/job"); err == nil {
		t.Error("设备订阅了别人的 topic——ACL 没生效")
	}
}

// 认证失败必须被拒，不能是「连上了但收不到消息」那种半死状态。
func TestEndToEndRejectsBadKey(t *testing.T) {
	e := startServer(t)
	if _, err := e.tryConnectDevice("f412fa87c9e0", "bogus.key"); err == nil {
		t.Error("错误密钥竟然连上了")
	}
}

// device 角色的 username 必须等于密钥里的 dev，防止拿 A 的密钥冒充 B。
func TestEndToEndRejectsMismatchedUsername(t *testing.T) {
	e := startServer(t)
	token := e.login("13800008888")
	devKey := e.enroll(token, "f412fa87c9e0")
	if _, err := e.tryConnectDevice("aaaaaaaaaaaa", devKey); err == nil {
		t.Error("用别的 username 拿着 A 的密钥连上了")
	}
}

// 插上打印机 → 上报机型档案 → 服务端建档 → 下发怪癖档案。
//
// 那 9 个字节的 UEL 现在住在 profile 里，不在固件里——这条链路通了，
// 新机型的新怪癖才能改服务端一行文本就生效。
func TestEndToEndProfileDelivery(t *testing.T) {
	e := startServer(t)
	token := e.login("13800008888")
	devKey := e.enroll(token, "f412fa87c9e0")
	dev := e.connectDevice("f412fa87c9e0", devKey)
	dev.heartbeat("ready", "", "CNB9K1P2X4")

	body := `{"serial":"CNB9K1P2X4","vid":"03F0","pid":"F22A","make":"HP",` +
		`"model":"HP Laser MFP 136a","cmd":"URF,PCL,PJL,PWGRaster",` +
		`"urf_caps":"CP1,RS600,V1.4,W8"}`
	code, resp := e.req("POST", "/api/device/f412fa87c9e0/ident", body,
		map[string]string{"Content-Type": "application/json",
			"Authorization": "Bearer " + devKey})
	if code != 200 {
		t.Fatalf("上报 = %d %s", code, resp)
	}

	got := dev.waitProfile(5 * time.Second)
	p, err := profile.Unmarshal(got)
	if err != nil {
		t.Fatalf("下发的档案不是合法 JSON：%v", err)
	}
	if p.Serial != "CNB9K1P2X4" {
		t.Errorf("档案 serial = %q", p.Serial)
	}
	if len(p.Hooks.JobEnd) != 1 || p.Hooks.JobEnd[0].Data != profile.UEL {
		t.Errorf("默认档案没发 UEL：%+v", p.Hooks.JobEnd)
	}
	if len(got) > profile.MaxBytes {
		t.Errorf("档案 %d 字节，超过上限 %d", len(got), profile.MaxBytes)
	}
}

// 固定手机号：不发短信也能登录，测试链路不依赖短信服务商。
func TestEndToEndDevLogin(t *testing.T) {
	e := startServerWithDevLogin(t, "13800000000", "424242")
	token := e.loginWithCode("13800000000", "424242")
	if token == "" {
		t.Fatal("固定手机号登录失败")
	}
	if e.sender.last != "" {
		t.Errorf("固定号码发了真短信：%q", e.sender.last)
	}
	// 连续登录不该被 60 秒闸挡住
	if e.loginWithCode("13800000000", "424242") == "" {
		t.Error("连续登录被限流了，测试没法用")
	}
}
