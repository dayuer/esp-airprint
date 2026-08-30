package integration

import (
	"context"
	"crypto/ecdsa"
	"crypto/elliptic"
	"crypto/rand"
	"crypto/tls"
	"crypto/x509"
	"crypto/x509/pkix"
	"encoding/binary"
	"encoding/json"
	"encoding/pem"
	"fmt"
	"io"
	"math/big"
	"net"
	"net/http"
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"

	paho "github.com/eclipse/paho.mqtt.golang"

	"github.com/dayuer/esp-airprint/server/go/internal/auth"
	"github.com/dayuer/esp-airprint/server/go/internal/broker"
	"github.com/dayuer/esp-airprint/server/go/internal/config"
	"github.com/dayuer/esp-airprint/server/go/internal/device"
	"github.com/dayuer/esp-airprint/server/go/internal/httpapi"
	"github.com/dayuer/esp-airprint/server/go/internal/registry"
	"github.com/dayuer/esp-airprint/server/go/internal/store"
	"github.com/dayuer/esp-airprint/server/go/internal/tlsx"
)

type fakeSender struct{ last string }

func (f *fakeSender) Send(ctx context.Context, phone, code string) error {
	f.last = code
	return nil
}

type env struct {
	t        *testing.T
	httpBase string
	mqttAddr string
	store    *store.Store
	sender   *fakeSender
	client   *http.Client
}

// startServer 起一套真的：内嵌 broker + HTTPS，自签证书，端口交给内核分配。
// 固定端口会让并行测试互相打架。
func startServer(t *testing.T) *env {
	t.Helper()
	dir := t.TempDir()
	certDir := filepath.Join(dir, "certs")
	if err := os.MkdirAll(certDir, 0o755); err != nil {
		t.Fatal(err)
	}
	writeSelfSigned(t, certDir)

	cfg := &config.Config{
		Root: dir, CertDir: certDir,
		JobRetainHours: 24, JobRetainPerDevice: 50,
	}
	for _, d := range []string{cfg.JobsDir(), cfg.IdentsDir()} {
		if err := os.MkdirAll(d, 0o755); err != nil {
			t.Fatal(err)
		}
	}
	st, err := store.Open(cfg.DBPath())
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { st.Close() })

	pb, err := auth.NewPhoneBox("itest-pepper",
		"000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f")
	if err != nil {
		t.Fatal(err)
	}
	sender := &fakeSender{}
	v := auth.NewVerifier(st)
	mem := broker.NewMembership(st)
	reloader, err := tlsx.New(cfg.CertPath(), cfg.KeyPath())
	if err != nil {
		t.Fatal(err)
	}

	pubShim := &publisherShim{}
	reg := registry.New(st, pubShim, device.Options{
		JobTimeout: 180 * time.Second, IdleTimeout: time.Hour,
	})
	t.Cleanup(reg.Shutdown)

	mqttAddr := freeAddr(t)
	br, err := broker.New(mqttAddr, reloader.Config(), v, mem, reg)
	if err != nil {
		t.Fatal(err)
	}
	pubShim.b = br
	if err := br.Serve(); err != nil { // 非阻塞：启动监听后立即返回
		t.Fatal(err)
	}
	t.Cleanup(func() { br.Close() })

	api := httpapi.New(cfg, st, v, pb, auth.NewSMS(st, sender, time.Now), reg, mem)
	ln, err := tls.Listen("tcp", freeAddr(t), reloader.Config())
	if err != nil {
		t.Fatal(err)
	}
	srv := &http.Server{Handler: api.Handler()}
	go srv.Serve(ln)
	t.Cleanup(func() { srv.Close() })

	return &env{
		t: t, httpBase: "https://" + ln.Addr().String(), mqttAddr: mqttAddr,
		store: st, sender: sender,
		client: &http.Client{Transport: &http.Transport{
			TLSClientConfig: &tls.Config{InsecureSkipVerify: true}, // 仅测试
		}},
	}
}

type publisherShim struct{ b *broker.Broker }

func (p *publisherShim) PublishJob(dev, job string, size int64) error {
	return p.b.PublishJob(dev, job, size)
}

func freeAddr(t *testing.T) string {
	t.Helper()
	l, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	addr := l.Addr().String()
	l.Close()
	return addr
}

func writeSelfSigned(t *testing.T, dir string) {
	t.Helper()
	key, err := ecdsa.GenerateKey(elliptic.P256(), rand.Reader)
	if err != nil {
		t.Fatal(err)
	}
	tpl := &x509.Certificate{
		SerialNumber: big.NewInt(1),
		Subject:      pkix.Name{CommonName: "localhost"},
		NotBefore:    time.Now().Add(-time.Hour),
		NotAfter:     time.Now().Add(24 * time.Hour),
		IPAddresses:  []net.IP{net.ParseIP("127.0.0.1")},
	}
	der, err := x509.CreateCertificate(rand.Reader, tpl, tpl, &key.PublicKey, key)
	if err != nil {
		t.Fatal(err)
	}
	kb, _ := x509.MarshalECPrivateKey(key)
	os.WriteFile(filepath.Join(dir, "fullchain.pem"),
		pem.EncodeToMemory(&pem.Block{Type: "CERTIFICATE", Bytes: der}), 0o644)
	os.WriteFile(filepath.Join(dir, "privkey.pem"),
		pem.EncodeToMemory(&pem.Block{Type: "EC PRIVATE KEY", Bytes: kb}), 0o600)
}

// —— HTTP 侧 ——

func (e *env) req(method, path, body string, hdr map[string]string) (int, []byte) {
	e.t.Helper()
	var r io.Reader
	if body != "" {
		r = strings.NewReader(body)
	}
	req, err := http.NewRequest(method, e.httpBase+path, r)
	if err != nil {
		e.t.Fatal(err)
	}
	for k, v := range hdr {
		req.Header.Set(k, v)
	}
	resp, err := e.client.Do(req)
	if err != nil {
		e.t.Fatal(err)
	}
	defer resp.Body.Close()
	out, _ := io.ReadAll(resp.Body)
	return resp.StatusCode, out
}

func (e *env) login(phone string) string {
	e.t.Helper()
	if code, b := e.req("POST", "/api/auth/sms", `{"phone":"`+phone+`"}`,
		map[string]string{"Content-Type": "application/json"}); code != 200 {
		e.t.Fatalf("发码 = %d %s", code, b)
	}
	code, b := e.req("POST", "/api/auth/verify",
		fmt.Sprintf(`{"phone":"%s","code":"%s","device":"itest"}`, phone, e.sender.last),
		map[string]string{"Content-Type": "application/json"})
	if code != 200 {
		e.t.Fatalf("登录 = %d %s", code, b)
	}
	var out struct {
		Token string `json:"token"`
	}
	json.Unmarshal(b, &out)
	return out.Token
}

func (e *env) enroll(token, dev string) string {
	e.t.Helper()
	code, b := e.req("POST", "/api/device/enroll", `{"dev":"`+dev+`"}`,
		map[string]string{"Content-Type": "application/json",
			"Authorization": "Bearer " + token})
	if code != 200 {
		e.t.Fatalf("enroll = %d %s", code, b)
	}
	var out struct {
		DeviceKey string `json:"device_key"`
	}
	json.Unmarshal(b, &out)
	return out.DeviceKey
}

func (e *env) upload(token, dev, serial string, body []byte) string {
	e.t.Helper()
	req, _ := http.NewRequest("POST", e.httpBase+"/api/print", strings.NewReader(string(body)))
	req.Header.Set("Content-Type", "image/urf")
	req.Header.Set("X-Device", dev)
	req.Header.Set("X-Printer-Serial", serial)
	req.Header.Set("Authorization", "Bearer "+token)
	resp, err := e.client.Do(req)
	if err != nil {
		e.t.Fatal(err)
	}
	defer resp.Body.Close()
	b, _ := io.ReadAll(resp.Body)
	if resp.StatusCode != 200 {
		e.t.Fatalf("上传 = %d %s", resp.StatusCode, b)
	}
	var out struct {
		Job string `json:"job"`
	}
	json.Unmarshal(b, &out)
	return out.Job
}

// waitJobState 轮询数据库，不用固定 sleep——那是 flaky 之源。
func (e *env) waitJobState(jid, want string, d time.Duration) {
	e.t.Helper()
	deadline := time.Now().Add(d)
	var last string
	for time.Now().Before(deadline) {
		j, ok, _ := e.store.GetJob(jid)
		if ok {
			last = j.State
			if last == want {
				return
			}
		}
		time.Sleep(10 * time.Millisecond)
	}
	e.t.Fatalf("作业 %s 状态停在 %q，期望 %q", jid, last, want)
}

// —— 设备侧（用 paho 当假固件）——

type fakeDevice struct {
	t    *testing.T
	c    paho.Client
	dev  string
	jobs chan jobSignal
}

type jobSignal struct {
	ID   string `json:"id"`
	Size int64  `json:"size"`
}

func (e *env) connectDevice(dev, key string) *fakeDevice {
	e.t.Helper()
	d, err := e.tryConnectDevice(dev, key)
	if err != nil {
		e.t.Fatalf("设备连不上：%v", err)
	}
	return d
}

func (e *env) tryConnectDevice(dev, key string) (*fakeDevice, error) {
	opts := paho.NewClientOptions().
		AddBroker("ssl://" + e.mqttAddr).
		SetClientID(dev).
		SetUsername(dev).
		SetPassword(key).
		SetTLSConfig(&tls.Config{InsecureSkipVerify: true}).
		SetConnectTimeout(5 * time.Second).
		SetAutoReconnect(false)

	fd := &fakeDevice{t: e.t, dev: dev, jobs: make(chan jobSignal, 8)}
	opts.SetDefaultPublishHandler(func(_ paho.Client, m paho.Message) {
		var js jobSignal
		if json.Unmarshal(m.Payload(), &js) == nil {
			select {
			case fd.jobs <- js:
			default:
			}
		}
	})
	c := paho.NewClient(opts)
	tok := c.Connect()
	if !tok.WaitTimeout(5*time.Second) || tok.Error() != nil {
		return nil, fmt.Errorf("连接失败：%v", tok.Error())
	}
	fd.c = c
	e.t.Cleanup(func() { c.Disconnect(100) })
	if err := fd.subscribe("printer/" + dev + "/job"); err != nil {
		return nil, err
	}
	return fd, nil
}

// subscribe 必须检查 SUBACK 的返回码：broker 拒绝订阅时回 0x80，
// 而 paho 的 token.Error() 是 nil——只看 error 会以为订阅成功了。
func (d *fakeDevice) subscribe(topic string) error {
	tok := d.c.Subscribe(topic, 1, nil)
	if !tok.WaitTimeout(5 * time.Second) {
		return fmt.Errorf("订阅 %s 超时", topic)
	}
	if err := tok.Error(); err != nil {
		return err
	}
	st, ok := tok.(*paho.SubscribeToken)
	if !ok {
		return nil
	}
	for f, code := range st.Result() {
		if code >= 0x80 {
			return fmt.Errorf("订阅 %s 被拒（SUBACK 0x%02x）", f, code)
		}
	}
	return nil
}

func (d *fakeDevice) heartbeat(state, job, serial string) {
	d.t.Helper()
	payload, _ := json.Marshal(map[string]any{
		"dev": d.dev, "job": job, "state": state, "bytes": 0, "serial": serial,
		"prn": map[string]any{"display": "Ready"},
	})
	tok := d.c.Publish("printer/"+d.dev+"/status", 1, false, payload)
	if !tok.WaitTimeout(5*time.Second) || tok.Error() != nil {
		d.t.Fatalf("心跳发送失败：%v", tok.Error())
	}
}

func (d *fakeDevice) waitJob(timeout time.Duration) jobSignal {
	d.t.Helper()
	select {
	case js := <-d.jobs:
		return js
	case <-time.After(timeout):
		d.t.Fatal("没有收到派发信令")
		return jobSignal{}
	}
}

func urfBytes(pages uint32, w, h uint32) []byte {
	b := make([]byte, 12+32+512)
	copy(b, "UNIRAST\x00")
	binary.BigEndian.PutUint32(b[8:12], pages)
	binary.BigEndian.PutUint32(b[24:28], w)
	binary.BigEndian.PutUint32(b[28:32], h)
	return b
}
