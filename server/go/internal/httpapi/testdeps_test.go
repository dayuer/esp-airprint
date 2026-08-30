package httpapi

import (
	"bytes"
	"context"
	"encoding/binary"
	"encoding/json"
	"net/http"
	"net/http/httptest"
	"os"
	"strings"
	"testing"
	"time"

	"github.com/dayuer/esp-airprint/server/go/internal/auth"
	"github.com/dayuer/esp-airprint/server/go/internal/config"
	"github.com/dayuer/esp-airprint/server/go/internal/device"
	"github.com/dayuer/esp-airprint/server/go/internal/registry"
	"github.com/dayuer/esp-airprint/server/go/internal/store"
)

type fakeSender struct{ last string }

func (f *fakeSender) Send(ctx context.Context, phone, code string) error {
	f.last = code
	return nil
}

// recPub 记下下发过的档案，测试据此断言。
type recPub struct{ profiles map[string][]byte }

func (p *recPub) PublishJob(dev, job string, size int64) error { return nil }

func (p *recPub) PublishProfile(dev string, body []byte) error {
	if p.profiles == nil {
		p.profiles = map[string][]byte{}
	}
	p.profiles[dev] = body
	return nil
}

type nopMem struct{}

func (nopMem) Invalidate(string) {}

type testDeps struct {
	store  *store.Store
	phone  *auth.PhoneBox
	sender *fakeSender
	reg    *registry.Registry
	v      *auth.Verifier
	cfg    *config.Config
	pub    *recPub
	clock  time.Time
}

func newTestAPI(t *testing.T) (http.Handler, *testDeps) {
	t.Helper()
	dir := t.TempDir()
	st, err := store.Open(dir + "/jobs.db")
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { st.Close() })

	pb, err := auth.NewPhoneBox("test-pepper",
		"000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f")
	if err != nil {
		t.Fatal(err)
	}
	dep := &testDeps{store: st, phone: pb, sender: &fakeSender{},
		clock: time.Unix(1_000_000, 0)}
	sms := auth.NewSMS(st, dep.sender, func() time.Time { return dep.clock })
	dep.pub = &recPub{}
	reg := registry.New(st, dep.pub, device.Options{
		JobTimeout: 180 * time.Second, IdleTimeout: time.Hour,
	})
	t.Cleanup(reg.Shutdown)
	dep.reg = reg
	dep.v = auth.NewVerifier(st)

	cfg := &config.Config{Root: dir, JobRetainHours: 24, JobRetainPerDevice: 50}
	for _, d := range []string{cfg.JobsDir(), cfg.IdentsDir()} {
		if err := os.MkdirAll(d, 0o755); err != nil {
			t.Fatal(err)
		}
	}
	dep.cfg = cfg
	return New(cfg, st, dep.v, pb, sms, reg, nopMem{}, dep.pub).Handler(), dep
}

// —— 请求辅助 ——

func bearer(tok string) map[string]string {
	return map[string]string{"Authorization": "Bearer " + tok}
}

func do(h http.Handler, req *http.Request, hdr map[string]string) *httptest.ResponseRecorder {
	for k, v := range hdr {
		req.Header.Set(k, v)
	}
	rr := httptest.NewRecorder()
	h.ServeHTTP(rr, req)
	return rr
}

func post(h http.Handler, path, body string, hdr map[string]string) *httptest.ResponseRecorder {
	req := httptest.NewRequest("POST", path, strings.NewReader(body))
	req.Header.Set("Content-Type", "application/json")
	return do(h, req, hdr)
}

func get(h http.Handler, path string, hdr map[string]string) *httptest.ResponseRecorder {
	return do(h, httptest.NewRequest("GET", path, nil), hdr)
}

func upload(h http.Handler, token, dev, serial, ct string, body []byte) *httptest.ResponseRecorder {
	req := httptest.NewRequest("POST", "/api/print", bytes.NewReader(body))
	return do(h, req, map[string]string{
		"Content-Type":     ct,
		"X-Device":         dev,
		"X-Printer-Serial": serial,
		"Authorization":    "Bearer " + token,
	})
}

// urfBytes 拼一份最小可用的 URF。
func urfBytes(pages uint32, w, h uint32) []byte {
	b := make([]byte, 12+32+512)
	copy(b, "UNIRAST\x00")
	binary.BigEndian.PutUint32(b[8:12], pages)
	binary.BigEndian.PutUint32(b[24:28], w)
	binary.BigEndian.PutUint32(b[28:32], h)
	return b
}

// login 走完整的发码 → 校验流程，返回 session token。
func login(t *testing.T, h http.Handler, dep *testDeps, phone string) string {
	t.Helper()
	if rr := post(h, "/api/auth/sms", `{"phone":"`+phone+`"}`, nil); rr.Code != 200 {
		t.Fatalf("发码失败：%d %s", rr.Code, rr.Body)
	}
	rr := post(h, "/api/auth/verify",
		`{"phone":"`+phone+`","code":"`+dep.sender.last+`","device":"test"}`, nil)
	var out struct {
		Token string `json:"token"`
	}
	json.Unmarshal(rr.Body.Bytes(), &out)
	if out.Token == "" {
		t.Fatalf("登录失败：%d %s", rr.Code, rr.Body)
	}
	return out.Token
}

// enroll 绑一台设备，返回 device 密钥。
func enroll(t *testing.T, h http.Handler, token, dev string) string {
	t.Helper()
	rr := post(h, "/api/device/enroll", `{"dev":"`+dev+`"}`, bearer(token))
	if rr.Code != 200 {
		t.Fatalf("enroll 失败：%d %s", rr.Code, rr.Body)
	}
	var out struct {
		DeviceKey string `json:"device_key"`
	}
	json.Unmarshal(rr.Body.Bytes(), &out)
	return out.DeviceKey
}
