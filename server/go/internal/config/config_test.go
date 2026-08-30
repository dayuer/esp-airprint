package config

import (
	"os"
	"path/filepath"
	"testing"
)

func write(t *testing.T, body string) string {
	t.Helper()
	p := filepath.Join(t.TempDir(), "config.json")
	if err := os.WriteFile(p, []byte(body), 0o600); err != nil {
		t.Fatal(err)
	}
	return p
}

const minimal = `{"cert_dir":"/etc/le/live/x","phone_pepper":"p","phone_key":"k"}`

func TestLoadAppliesDefaults(t *testing.T) {
	c, err := Load(write(t, minimal))
	if err != nil {
		t.Fatal(err)
	}
	if c.Root != "/opt/stickbox" {
		t.Errorf("Root = %q，期望默认 /opt/stickbox", c.Root)
	}
	if c.HTTPAddr != ":9443" {
		t.Errorf("HTTPAddr = %q，期望默认 :9443", c.HTTPAddr)
	}
	if c.MQTTAddr != ":8883" {
		t.Errorf("MQTTAddr = %q，期望默认 :8883", c.MQTTAddr)
	}
	if c.JobRetainHours != 24 {
		t.Errorf("JobRetainHours = %d，期望默认 24", c.JobRetainHours)
	}
	if c.JobRetainPerDevice != 50 {
		t.Errorf("JobRetainPerDevice = %d，期望默认 50", c.JobRetainPerDevice)
	}
}

func TestLoadRejectsMissingRequired(t *testing.T) {
	cases := map[string]string{
		"缺 cert_dir":     `{"phone_pepper":"p","phone_key":"k"}`,
		"缺 phone_pepper": `{"cert_dir":"/c","phone_key":"k"}`,
		"缺 phone_key":    `{"cert_dir":"/c","phone_pepper":"p"}`,
	}
	for name, body := range cases {
		if _, err := Load(write(t, body)); err == nil {
			t.Errorf("%s 时应当报错", name)
		}
	}
}

func TestDerivedPaths(t *testing.T) {
	c, err := Load(write(t, `{"cert_dir":"/c","root":"/srv/ap","phone_pepper":"p","phone_key":"k"}`))
	if err != nil {
		t.Fatal(err)
	}
	if c.JobsDir() != "/srv/ap/jobs" {
		t.Errorf("JobsDir = %q", c.JobsDir())
	}
	if c.IdentsDir() != "/srv/ap/idents" {
		t.Errorf("IdentsDir = %q", c.IdentsDir())
	}
	if c.DBPath() != "/srv/ap/jobs.db" {
		t.Errorf("DBPath = %q", c.DBPath())
	}
	if c.CertPath() != "/c/fullchain.pem" {
		t.Errorf("CertPath = %q", c.CertPath())
	}
	if c.KeyPath() != "/c/privkey.pem" {
		t.Errorf("KeyPath = %q", c.KeyPath())
	}
}

func TestLoadReportsMissingFile(t *testing.T) {
	if _, err := Load("/nope/config.json"); err == nil {
		t.Error("配置文件不存在时应当报错")
	}
}

func TestDevLoginValidation(t *testing.T) {
	base := `"cert_dir":"/c","phone_pepper":"p","phone_key":"k"`

	c, err := Load(write(t, `{`+base+`,"dev_login":{"phone":"13800000000","code":"123456"}}`))
	if err != nil {
		t.Fatalf("合法的 dev_login 被拒：%v", err)
	}
	if !c.DevLogin.Enabled() {
		t.Error("Enabled() 应为 true")
	}

	if _, err := Load(write(t, `{`+base+`,"dev_login":{"phone":"13800000000","code":"123"}}`)); err == nil {
		t.Error("验证码不是 6 位应当报错")
	}

	// 配了短信服务商就说明是生产环境，此时还留着后门是配置事故
	if _, err := Load(write(t, `{`+base+`,"dev_login":{"phone":"13800000000","code":"123456"},`+
		`"sms":{"provider":"aliyun","access_key_id":"x"}}`)); err == nil {
		t.Error("dev_login 与 sms 同时配置应当拒绝启动")
	}

	c, _ = Load(write(t, `{`+base+`}`))
	if c.DevLogin.Enabled() {
		t.Error("没配时 Enabled() 应为 false")
	}
}
