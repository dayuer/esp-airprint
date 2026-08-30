package httpapi

import (
	"encoding/json"
	"testing"
	"time"
)

func TestPrintAcceptsURFAndQueues(t *testing.T) {
	h, dep := newTestAPI(t)
	token := login(t, h, dep, "13800008888")
	enroll(t, h, token, "f412fa87c9e0")

	rr := upload(h, token, "f412fa87c9e0", "PA", "image/urf", urfBytes(2, 4962, 7014))
	if rr.Code != 200 {
		t.Fatalf("上传 = %d %s", rr.Code, rr.Body)
	}
	var out struct {
		Job   string `json:"job"`
		Size  int64  `json:"size"`
		Pages int    `json:"pages"`
		State string `json:"state"`
	}
	json.Unmarshal(rr.Body.Bytes(), &out)
	if out.State != "queued" {
		t.Errorf("state = %q，期望 queued（服务端不渲染，没有 rendering 态）", out.State)
	}
	if out.Pages != 2 {
		t.Errorf("pages = %d，期望 2——App 靠它核对自己的编码器", out.Pages)
	}
	j, ok, _ := dep.store.GetJob(out.Job)
	if !ok || j.Serial != "PA" {
		t.Errorf("作业没绑打印机：%+v", j)
	}
	if j.Size != out.Size || j.Size == 0 {
		t.Errorf("落库大小 %d 与响应 %d 不符", j.Size, out.Size)
	}
}

// 最要命的一条：把 PDF 当 URF 传，必须在入口挡住。
func TestPrintRejectsPDFClaimingURF(t *testing.T) {
	h, dep := newTestAPI(t)
	token := login(t, h, dep, "13800008888")
	enroll(t, h, token, "f412fa87c9e0")

	rr := upload(h, token, "f412fa87c9e0", "PA", "image/urf",
		[]byte("%PDF-1.7\nrest of the pdf goes here and here"))
	if rr.Code != 400 {
		t.Fatalf("PDF 冒充 URF = %d，期望 400——用户会收到几十张乱码纸", rr.Code)
	}
}

// 页数 0 打印机认为文档为空，什么都不打。
func TestPrintRejectsZeroPages(t *testing.T) {
	h, dep := newTestAPI(t)
	token := login(t, h, dep, "13800008888")
	enroll(t, h, token, "f412fa87c9e0")
	if rr := upload(h, token, "f412fa87c9e0", "PA", "image/urf",
		urfBytes(0, 4962, 7014)); rr.Code != 400 {
		t.Errorf("页数 0 = %d，期望 400", rr.Code)
	}
}

func TestPrintRejectsWrongContentType(t *testing.T) {
	h, dep := newTestAPI(t)
	token := login(t, h, dep, "13800008888")
	enroll(t, h, token, "f412fa87c9e0")
	if rr := upload(h, token, "f412fa87c9e0", "PA", "application/pdf",
		urfBytes(1, 4962, 7014)); rr.Code != 415 {
		t.Errorf("PDF 的 Content-Type = %d，期望 415", rr.Code)
	}
}

func TestPrintRequiresPrinterSerial(t *testing.T) {
	h, dep := newTestAPI(t)
	token := login(t, h, dep, "13800008888")
	enroll(t, h, token, "f412fa87c9e0")
	if rr := upload(h, token, "f412fa87c9e0", "", "image/urf",
		urfBytes(1, 4962, 7014)); rr.Code != 400 {
		t.Errorf("缺 X-Printer-Serial = %d，期望 400", rr.Code)
	}
}

// 不是自己名下的设备，不能往里投作业。
func TestPrintRejectsForeignDevice(t *testing.T) {
	h, dep := newTestAPI(t)
	token := login(t, h, dep, "13800008888")
	if rr := upload(h, token, "aaaaaaaaaaaa", "PA", "image/urf",
		urfBytes(1, 4962, 7014)); rr.Code != 403 {
		t.Errorf("投给别人的设备 = %d，期望 403", rr.Code)
	}
}

// 校验不通过时不能留下垃圾文件。
func TestPrintRejectionLeavesNoFile(t *testing.T) {
	h, dep := newTestAPI(t)
	token := login(t, h, dep, "13800008888")
	enroll(t, h, token, "f412fa87c9e0")
	upload(h, token, "f412fa87c9e0", "PA", "image/urf", []byte("%PDF-1.7 not urf at all"))

	entries, err := osReadDir(dep.cfg.JobsDir())
	if err != nil {
		t.Fatal(err)
	}
	if len(entries) != 0 {
		t.Errorf("拒绝后残留了 %d 个文件", len(entries))
	}
}

// 取件必须校验作业归属——v1 完全没查，任何人能下载任何作业。
func TestJobDataChecksOwnership(t *testing.T) {
	h, dep := newTestAPI(t)
	appTok := login(t, h, dep, "13800008888")
	devKey := enroll(t, h, appTok, "f412fa87c9e0")

	rr := upload(h, appTok, "f412fa87c9e0", "PA", "image/urf", urfBytes(1, 4962, 7014))
	var up struct {
		Job string `json:"job"`
	}
	json.Unmarshal(rr.Body.Bytes(), &up)

	got := get(h, "/api/job/"+up.Job+"/data", map[string]string{
		"Authorization": "Bearer " + devKey})
	if got.Code != 200 {
		t.Fatalf("正主取件 = %d %s", got.Code, got.Body)
	}
	if got.Body.Len() == 0 {
		t.Error("取回来是空的")
	}
	if j, _, _ := dep.store.GetJob(up.Job); j.State != "downloading" {
		t.Errorf("取件后 state = %q", j.State)
	}

	// 换一台设备来取同一件
	dep.clock = dep.clock.Add(2 * time.Minute)
	otherApp := login(t, h, dep, "13900009999")
	otherKey := enroll(t, h, otherApp, "aaaaaaaaaaaa")
	got = get(h, "/api/job/"+up.Job+"/data", map[string]string{
		"Authorization": "Bearer " + otherKey})
	if got.Code != 403 {
		t.Errorf("别人的设备取件 = %d，期望 403", got.Code)
	}
}

func TestJobData404ForUnknownJob(t *testing.T) {
	h, dep := newTestAPI(t)
	appTok := login(t, h, dep, "13800008888")
	devKey := enroll(t, h, appTok, "f412fa87c9e0")
	if rr := get(h, "/api/job/deadbeef1234/data", bearer(devKey)); rr.Code != 404 {
		t.Errorf("= %d，期望 404", rr.Code)
	}
}

func TestIdentStoredAndScoped(t *testing.T) {
	h, dep := newTestAPI(t)
	appTok := login(t, h, dep, "13800008888")
	devKey := enroll(t, h, appTok, "f412fa87c9e0")

	body := `{"urf_caps":"CP1,RS600,W8","serial":"CNB9K1P2X4"}`
	if rr := post(h, "/api/device/f412fa87c9e0/ident", body, bearer(devKey)); rr.Code != 200 {
		t.Fatalf("上报 = %d %s", rr.Code, rr.Body)
	}
	if caps, serial, ok := loadCapsForTest(dep, "f412fa87c9e0"); !ok ||
		caps != "CP1,RS600,W8" || serial != "CNB9K1P2X4" {
		t.Errorf("落盘后读回 caps=%q serial=%q ok=%v", caps, serial, ok)
	}
	// 只能报自己的
	if rr := post(h, "/api/device/aaaaaaaaaaaa/ident", body, bearer(devKey)); rr.Code != 403 {
		t.Errorf("替别人上报 = %d，期望 403", rr.Code)
	}
	if rr := post(h, "/api/device/f412fa87c9e0/ident", "not json", bearer(devKey)); rr.Code != 400 {
		t.Errorf("畸形 JSON = %d，期望 400", rr.Code)
	}
}
