// Package tlsx 让证书续签不需要重启进程。
//
// 旧做法是 certbot 的 deploy hook 重启服务，钩子一丢就是
// 「证书续了但服务还拿着旧的」。这里按 mtime 检测，钩子降级为可选优化。
package tlsx

import (
	"crypto/tls"
	"log/slog"
	"os"
	"sync"
	"time"
)

type Reloader struct {
	certFile, keyFile string

	mu      sync.RWMutex
	cert    *tls.Certificate
	mtime   time.Time
	checked time.Time
	now     func() time.Time
}

// New 立即加载一次。启动时证书就不对的话直接失败——
// 起来了但没有证书，比起不来更难排查。
func New(certFile, keyFile string) (*Reloader, error) {
	r := &Reloader{certFile: certFile, keyFile: keyFile, now: time.Now}
	if err := r.load(); err != nil {
		return nil, err
	}
	return r, nil
}

// GetCertificate 挂到 tls.Config.GetCertificate。
// 每秒最多 stat 一次——握手是热路径，不能每次都碰磁盘。
func (r *Reloader) GetCertificate(*tls.ClientHelloInfo) (*tls.Certificate, error) {
	r.mu.RLock()
	cert, checked, mtime := r.cert, r.checked, r.mtime
	r.mu.RUnlock()

	if r.now().Sub(checked) < time.Second {
		return cert, nil
	}
	fi, err := os.Stat(r.certFile)
	if err != nil {
		r.mu.Lock()
		r.checked = r.now()
		r.mu.Unlock()
		return cert, nil // 文件暂时读不到，继续用旧证书
	}
	if !fi.ModTime().After(mtime) {
		r.mu.Lock()
		r.checked = r.now()
		r.mu.Unlock()
		return cert, nil
	}
	// 加载失败就继续用旧的：续签中途文件可能只写了一半，
	// 这时断掉所有握手比用一张还没过期的旧证书糟得多。
	if err := r.load(); err != nil {
		slog.Warn("证书重载失败，继续使用旧证书", "err", err)
		r.mu.Lock()
		r.checked = r.now()
		r.mu.Unlock()
		return cert, nil
	}
	slog.Info("证书已热重载", "file", r.certFile)
	r.mu.RLock()
	defer r.mu.RUnlock()
	return r.cert, nil
}

func (r *Reloader) load() error {
	c, err := tls.LoadX509KeyPair(r.certFile, r.keyFile)
	if err != nil {
		return err
	}
	fi, err := os.Stat(r.certFile)
	if err != nil {
		return err
	}
	r.mu.Lock()
	r.cert, r.mtime, r.checked = &c, fi.ModTime(), r.now()
	r.mu.Unlock()
	return nil
}

// Config 返回一份挂好回调的 TLS 配置，MQTT 和 HTTPS 共用。
func (r *Reloader) Config() *tls.Config {
	return &tls.Config{
		GetCertificate: r.GetCertificate,
		MinVersion:     tls.VersionTLS12,
	}
}
