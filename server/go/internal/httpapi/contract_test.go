package httpapi

import (
	"bytes"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"strconv"
	"strings"
	"testing"
)

// 设备用 esp_http_client，**不解 chunked 响应体**。
// 所有 JSON 响应都必须带 Content-Length（SERVER-REQUIREMENTS 3.3）。
func TestAllJSONResponsesHaveContentLength(t *testing.T) {
	h, dep := newTestAPI(t)
	token := login(t, h, dep, "13800008888")

	checks := []*httptest.ResponseRecorder{
		get(h, "/api/status", nil),                                   // 401
		post(h, "/api/auth/sms", `{"phone":"12345"}`, nil),           // 400
		post(h, "/api/auth/sms", `{"phone":"13900001111"}`, nil),     // 200
		post(h, "/api/device/enroll", `{"dev":"zz"}`, bearer(token)), // 400
	}
	for i, rr := range checks {
		cl := rr.Header().Get("Content-Length")
		if cl == "" {
			t.Errorf("第 %d 个响应（%d）没有 Content-Length——设备解不了 chunked", i+1, rr.Code)
			continue
		}
		n, err := strconv.Atoi(cl)
		if err != nil || n != rr.Body.Len() {
			t.Errorf("第 %d 个响应 Content-Length=%q 与实际 %d 字节不符",
				i+1, cl, rr.Body.Len())
		}
		if rr.Header().Get("Transfer-Encoding") != "" {
			t.Errorf("第 %d 个响应用了 Transfer-Encoding", i+1)
		}
	}
}

// 任何路径的 POST 都必须读完请求体，即使要返回错误
// （SERVER-REQUIREMENTS 3.1）。不读会让 HTTP/1.1 长连接失步：
// 设备发完 7KB 卡住等响应，34 秒后 MQTT 跟着写超时，最后设备重启。
func TestErrorResponsesDrainRequestBody(t *testing.T) {
	h, dep := newTestAPI(t)
	token := login(t, h, dep, "13800008888")
	enroll(t, h, token, "f412fa87c9e0")

	big := bytes.Repeat([]byte("x"), 7<<10)

	cases := []struct {
		name string
		req  *http.Request
		hdr  map[string]string
	}{
		{"未知路径", httptest.NewRequest("POST", "/api/nope", bytes.NewReader(big)), nil},
		{"上传时 Content-Type 不对",
			httptest.NewRequest("POST", "/api/print", bytes.NewReader(big)),
			map[string]string{"Content-Type": "application/pdf",
				"X-Device": "f412fa87c9e0", "X-Printer-Serial": "PA",
				"Authorization": "Bearer " + token}},
		{"上传时未授权",
			httptest.NewRequest("POST", "/api/print", bytes.NewReader(big)),
			map[string]string{"Content-Type": "image/urf",
				"X-Device": "f412fa87c9e0", "X-Printer-Serial": "PA"}},
		{"上传的不是 URF",
			httptest.NewRequest("POST", "/api/print", bytes.NewReader(big)),
			map[string]string{"Content-Type": "image/urf",
				"X-Device": "f412fa87c9e0", "X-Printer-Serial": "PA",
				"Authorization": "Bearer " + token}},
	}
	for _, c := range cases {
		rr := do(h, c.req, c.hdr)
		left, _ := io_ReadAll(c.req.Body)
		if len(left) != 0 {
			t.Errorf("%s（%d）：请求体还剩 %d 字节没读——连接会失步",
				c.name, rr.Code, len(left))
		}
	}
}

// ident 要留历史：探针会随固件改进而变，覆盖掉就没法比对
// 「是机器变了还是我们的采集变了」（SERVER-REQUIREMENTS 6.2）。
func TestIdentKeepsHistory(t *testing.T) {
	h, dep := newTestAPI(t)
	appTok := login(t, h, dep, "13800008888")
	devKey := enroll(t, h, appTok, "f412fa87c9e0")

	for i := 0; i < 2; i++ {
		if rr := post(h, "/api/device/f412fa87c9e0/ident",
			`{"serial":"S1","vid":"03F0","pid":"F22A","urf_caps":"RS600,W8"}`,
			bearer(devKey)); rr.Code != 200 {
			t.Fatalf("第 %d 次上报 = %d %s", i+1, rr.Code, rr.Body)
		}
	}
	dir := filepath.Join(dep.cfg.IdentsDir(), "f412fa87c9e0")
	entries, err := os.ReadDir(dir)
	if err != nil {
		t.Fatal(err)
	}
	hist := 0
	for _, e := range entries {
		if strings.HasPrefix(e.Name(), "base-") {
			hist++
		}
	}
	if hist < 1 {
		t.Errorf("目录里 %v，没有按时间戳留的历史", names(entries))
	}
	if _, err := os.Stat(filepath.Join(dir, "latest.json")); err != nil {
		t.Error("latest.json 不见了")
	}
}

// 分片上传：设备的 HTTPS 载荷必须 <4KB，全量档案要拆成几趟
// （SERVER-REQUIREMENTS 1.1）。带 _part 的片单独存，不覆盖第 0 层。
func TestIdentPartsStoredSeparately(t *testing.T) {
	h, dep := newTestAPI(t)
	appTok := login(t, h, dep, "13800008888")
	devKey := enroll(t, h, appTok, "f412fa87c9e0")

	post(h, "/api/device/f412fa87c9e0/ident",
		`{"serial":"S1","vid":"03F0","pid":"F22A","urf_caps":"RS600,W8"}`, bearer(devKey))
	post(h, "/api/device/f412fa87c9e0/ident",
		`{"_part":"pjl","info_id":"HP Laser"}`, bearer(devKey))

	dir := filepath.Join(dep.cfg.IdentsDir(), "f412fa87c9e0")
	for _, f := range []string{"latest.json", "base.json", "pjl.json"} {
		if _, err := os.Stat(filepath.Join(dir, f)); err != nil {
			t.Errorf("缺少 %s", f)
		}
	}
	// 第 0 层不能被 pjl 片覆盖——建档和 render-profile 都从 latest 取
	caps, serial, ok := loadCapsForTest(dep, "f412fa87c9e0")
	if !ok || caps != "RS600,W8" || serial != "S1" {
		t.Errorf("第 0 层被覆盖了：caps=%q serial=%q ok=%v", caps, serial, ok)
	}
}

// 分片名直接进文件名，不能让它跑出目录。
func TestIdentPartNameIsSanitized(t *testing.T) {
	h, dep := newTestAPI(t)
	appTok := login(t, h, dep, "13800008888")
	devKey := enroll(t, h, appTok, "f412fa87c9e0")

	post(h, "/api/device/f412fa87c9e0/ident",
		`{"_part":"../../etc/passwd","x":1}`, bearer(devKey))

	dir := filepath.Join(dep.cfg.IdentsDir(), "f412fa87c9e0")
	entries, _ := os.ReadDir(dir)
	for _, e := range entries {
		if strings.Contains(e.Name(), "..") || strings.Contains(e.Name(), "/") {
			t.Fatalf("分片名没消毒，写出了 %q", e.Name())
		}
	}
	if len(entries) == 0 {
		t.Error("什么都没写")
	}
}

func names(es []os.DirEntry) []string {
	var out []string
	for _, e := range es {
		out = append(out, e.Name())
	}
	return out
}
