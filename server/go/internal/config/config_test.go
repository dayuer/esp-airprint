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
	if c.Root != "/opt/airprint" {
		t.Errorf("Root = %q，期望默认 /opt/airprint", c.Root)
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
