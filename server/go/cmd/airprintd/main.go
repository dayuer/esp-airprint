package main

import (
	"context"
	"errors"
	"flag"
	"log/slog"
	"net/http"
	"os"
	"os/signal"
	"syscall"
	"time"

	"github.com/dayuer/esp-airprint/server/go/internal/auth"
	"github.com/dayuer/esp-airprint/server/go/internal/broker"
	"github.com/dayuer/esp-airprint/server/go/internal/config"
	"github.com/dayuer/esp-airprint/server/go/internal/device"
	"github.com/dayuer/esp-airprint/server/go/internal/httpapi"
	"github.com/dayuer/esp-airprint/server/go/internal/janitor"
	"github.com/dayuer/esp-airprint/server/go/internal/registry"
	"github.com/dayuer/esp-airprint/server/go/internal/sms"
	"github.com/dayuer/esp-airprint/server/go/internal/store"
	"github.com/dayuer/esp-airprint/server/go/internal/tlsx"
	"github.com/dayuer/esp-airprint/server/go/internal/version"
)

func main() {
	confPath := flag.String("conf", envOr("AIRPRINT_CONF", "/opt/airprint/config.json"),
		"配置文件路径")
	flag.Parse()

	slog.SetDefault(slog.New(slog.NewTextHandler(os.Stderr, nil)))

	cfg, err := config.Load(*confPath)
	if err != nil {
		die("读配置失败", err)
	}
	// 子命令走另一条路，不起服务
	if args := flag.Args(); len(args) > 0 {
		if err := runCLI(cfg, args); err != nil {
			die("命令失败", err)
		}
		return
	}
	if err := serve(cfg); err != nil {
		die("服务退出", err)
	}
}

func serve(cfg *config.Config) error {
	for _, d := range []string{cfg.JobsDir(), cfg.IdentsDir()} {
		if err := os.MkdirAll(d, 0o755); err != nil {
			return err
		}
	}
	st, err := store.Open(cfg.DBPath())
	if err != nil {
		return err
	}
	defer st.Close()

	// 进程上次退出时悬空的作业退回队列，否则永远卡在 downloading。
	if n, err := st.RecoverOnBoot(); err != nil {
		return err
	} else if n > 0 {
		slog.Info("恢复悬空作业", "count", n)
	}

	pb, err := auth.NewPhoneBox(cfg.PhonePepper, cfg.PhoneKey)
	if err != nil {
		return err
	}
	sender, err := newSender(cfg)
	if err != nil {
		return err
	}
	v := auth.NewVerifier(st)
	mem := broker.NewMembership(st)
	smsSvc := auth.NewSMS(st, sender, time.Now)
	if cfg.DevLogin.Enabled() {
		smsSvc.SetDevLogin(pb.HMAC(cfg.DevLogin.Phone), cfg.DevLogin.Code)
		// 每次启动都喊一遍。后门最危险的形态是「配上去之后没人记得」。
		slog.Warn("⚠ 固定手机号登录已启用——这是登录后门，生产环境必须移除",
			"phone", cfg.DevLogin.Phone)
	}

	reloader, err := tlsx.New(cfg.CertPath(), cfg.KeyPath())
	if err != nil {
		return err
	}

	// registry 需要 Publisher，broker 需要 Router——互相依赖，
	// 用一个延迟绑定的壳打破环。
	pubShim := &publisherShim{}
	reg := registry.New(st, pubShim, device.Options{
		JobTimeout: 180 * time.Second, IdleTimeout: 5 * time.Minute,
	})
	defer reg.Shutdown()

	br, err := broker.New(cfg.MQTTAddr, reloader.Config(), v, mem, reg)
	if err != nil {
		return err
	}
	pubShim.set(br)

	api := httpapi.New(cfg, st, v, pb, smsSvc, reg, mem, br)
	srv := &http.Server{
		Addr:              cfg.HTTPAddr,
		Handler:           api.Handler(),
		TLSConfig:         reloader.Config(),
		ReadHeaderTimeout: 10 * time.Second,
		// 不设 WriteTimeout：取件是流式大文件，慢设备会被拦腰砍断。
		IdleTimeout: 120 * time.Second,
	}

	stop := make(chan struct{})
	jan := janitor.New(st, cfg.JobsDir(), cfg.JobRetainHours, 72,
		cfg.JobRetainPerDevice, time.Now)
	go jan.Run(stop)

	// mochi 的 Serve() 是非阻塞的：启动监听后立即返回 nil。
	// 当成「服务退出」会让进程刚起来就结束——这个 bug 是冒烟测试抓到的，
	// 单测抓不到，因为没有哪个包会去起真的监听。
	if err := br.Serve(); err != nil {
		return err
	}
	slog.Info("MQTT/TLS 监听", "addr", cfg.MQTTAddr)

	errc := make(chan error, 1)
	go func() {
		slog.Info("HTTPS 监听", "addr", cfg.HTTPAddr, "version", version.String())
		err := srv.ListenAndServeTLS("", "") // 证书由 GetCertificate 回调提供
		if errors.Is(err, http.ErrServerClosed) {
			err = nil
		}
		errc <- err
	}()

	sig := make(chan os.Signal, 1)
	signal.Notify(sig, syscall.SIGINT, syscall.SIGTERM)
	select {
	case err := <-errc:
		close(stop)
		br.Close()
		return err
	case <-sig:
		slog.Info("收到退出信号，优雅关闭")
		close(stop)
		ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
		defer cancel()
		srv.Shutdown(ctx)
		br.Close()
		return nil
	}
}

// publisherShim 打破 registry 与 broker 的循环依赖：
// registry 构造时 broker 还不存在，构造完再填进来。
type publisherShim struct{ b *broker.Broker }

func (p *publisherShim) set(b *broker.Broker) { p.b = b }

func (p *publisherShim) PublishJob(dev, job string, size int64) error {
	if p.b == nil {
		return errors.New("broker 尚未就绪")
	}
	return p.b.PublishJob(dev, job, size)
}

func newSender(cfg *config.Config) (auth.Sender, error) {
	if cfg.SMS.Provider == "aliyun" && cfg.SMS.AccessKeyID != "" {
		return sms.NewAliyun(sms.AliyunConfig{
			AccessKeyID:     cfg.SMS.AccessKeyID,
			AccessKeySecret: cfg.SMS.AccessKeySecret,
			SignName:        cfg.SMS.SignName,
			TemplateCode:    cfg.SMS.TemplateCode,
		})
	}
	slog.Warn("短信未配置，验证码只打日志——生产环境必须配 sms.*")
	return sms.Logger{}, nil
}

func envOr(k, def string) string {
	if v := os.Getenv(k); v != "" {
		return v
	}
	return def
}

func die(msg string, err error) {
	slog.Error(msg, "err", err)
	os.Exit(1)
}
