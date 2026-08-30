package tlsx

import (
	"crypto/ecdsa"
	"crypto/elliptic"
	"crypto/rand"
	"crypto/tls"
	"crypto/x509"
	"crypto/x509/pkix"
	"encoding/pem"
	"math/big"
	"os"
	"path/filepath"
	"testing"
	"time"
)

// writeCert 生成一张自签证书写到磁盘。
func writeCert(t *testing.T, dir, cn string) {
	t.Helper()
	key, err := ecdsa.GenerateKey(elliptic.P256(), rand.Reader)
	if err != nil {
		t.Fatal(err)
	}
	tpl := &x509.Certificate{
		SerialNumber: big.NewInt(1),
		Subject:      pkix.Name{CommonName: cn},
		NotBefore:    time.Now().Add(-time.Hour),
		NotAfter:     time.Now().Add(24 * time.Hour),
	}
	der, err := x509.CreateCertificate(rand.Reader, tpl, tpl, &key.PublicKey, key)
	if err != nil {
		t.Fatal(err)
	}
	certPEM := pem.EncodeToMemory(&pem.Block{Type: "CERTIFICATE", Bytes: der})
	kb, err := x509.MarshalECPrivateKey(key)
	if err != nil {
		t.Fatal(err)
	}
	keyPEM := pem.EncodeToMemory(&pem.Block{Type: "EC PRIVATE KEY", Bytes: kb})
	if err := os.WriteFile(filepath.Join(dir, "fullchain.pem"), certPEM, 0o644); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(dir, "privkey.pem"), keyPEM, 0o600); err != nil {
		t.Fatal(err)
	}
}

func newReloader(t *testing.T, dir string) *Reloader {
	t.Helper()
	r, err := New(filepath.Join(dir, "fullchain.pem"), filepath.Join(dir, "privkey.pem"))
	if err != nil {
		t.Fatal(err)
	}
	return r
}

func cnOf(t *testing.T, c *tls.Certificate) string {
	t.Helper()
	leaf, err := x509.ParseCertificate(c.Certificate[0])
	if err != nil {
		t.Fatal(err)
	}
	return leaf.Subject.CommonName
}

func TestReloaderServesCert(t *testing.T) {
	dir := t.TempDir()
	writeCert(t, dir, "first.example")
	r := newReloader(t, dir)
	c, err := r.GetCertificate(&tls.ClientHelloInfo{})
	if err != nil {
		t.Fatal(err)
	}
	if got := cnOf(t, c); got != "first.example" {
		t.Errorf("CN = %q", got)
	}
}

// certbot 续签后写了新文件，进程不重启也要换过来。
func TestReloaderPicksUpNewFile(t *testing.T) {
	dir := t.TempDir()
	writeCert(t, dir, "old.example")
	r := newReloader(t, dir)
	r.GetCertificate(&tls.ClientHelloInfo{})

	writeCert(t, dir, "new.example")
	// 越过「每秒最多 stat 一次」的节流，并让 mtime 比记录的更新
	base := time.Now()
	r.now = func() time.Time { return base.Add(2 * time.Second) }
	newer := time.Now().Add(time.Minute)
	os.Chtimes(filepath.Join(dir, "fullchain.pem"), newer, newer)

	c, err := r.GetCertificate(&tls.ClientHelloInfo{})
	if err != nil {
		t.Fatal(err)
	}
	if got := cnOf(t, c); got != "new.example" {
		t.Errorf("CN = %q，期望 new.example——证书没有热重载", got)
	}
}

// 续签中途文件可能只写了一半，这时要继续用旧证书，不能报错断连接。
func TestReloaderKeepsOldOnBrokenFile(t *testing.T) {
	dir := t.TempDir()
	writeCert(t, dir, "good.example")
	r := newReloader(t, dir)
	r.GetCertificate(&tls.ClientHelloInfo{})

	os.WriteFile(filepath.Join(dir, "fullchain.pem"), []byte("half-written"), 0o644)
	base := time.Now()
	r.now = func() time.Time { return base.Add(2 * time.Second) }
	newer := time.Now().Add(time.Minute)
	os.Chtimes(filepath.Join(dir, "fullchain.pem"), newer, newer)

	c, err := r.GetCertificate(&tls.ClientHelloInfo{})
	if err != nil {
		t.Fatalf("坏文件不该让握手失败：%v", err)
	}
	if got := cnOf(t, c); got != "good.example" {
		t.Errorf("没有退回旧证书，CN = %q", got)
	}
}

func TestNewFailsOnMissingFile(t *testing.T) {
	if _, err := New("/nope/fullchain.pem", "/nope/privkey.pem"); err == nil {
		t.Error("启动时证书就不存在，应当直接失败")
	}
}
