// Package device 是每台设备的派发状态机。
//
// 全部状态私有于一个 goroutine，所以没有锁。Python 版靠 _pub 去重字典、
// "now-updated<180" 判忙、以及 pump() 开临时线程调用自己来模拟这件事——
// 那些在这里不是被重写，是不再需要。
//
// 硬规则：
//  1. 作业队列的真相永远在 store，actor 只缓存 inflight 一件。
//  2. 一次只派一件。设备可用堆几十 KB 且没有本地队列，堆过去等于丢件。
package device

import (
	"log/slog"
	"time"

	"github.com/dayuer/stickbox/server/go/internal/store"
)

// Publisher 是 actor 对 MQTT 的窄依赖。
type Publisher interface {
	PublishJob(dev, jobID string, size int64) error
}

// Store 是 actor 对持久层的窄依赖。
type Store interface {
	NextQueued(dev, serial string) (store.Job, bool, error)
	SetJobState(id, state string, bytes int64, errMsg string) error
}

type Options struct {
	// Ticker 驱动超时检查。nil 时 Run 自建 1 秒 ticker；测试里手动喂。
	Ticker      <-chan time.Time
	JobTimeout  time.Duration // 传输无进展多久退回队列，默认 180s
	IdleTimeout time.Duration // 多久没心跳就退出，默认 120s
	Now         func() time.Time
}

type Actor struct {
	dev string
	st  Store
	pub Publisher

	// —— 以下字段只有 actor 自己的 goroutine 碰，无锁 ——
	inflight string
	deadline time.Time
	lastSeen time.Time
	serial   string // 当前插着的打印机；空串 = 没插
	printer  []byte

	box    chan Msg
	ticker <-chan time.Time
	done   chan struct{}

	jobTimeout  time.Duration
	idleTimeout time.Duration
	now         func() time.Time

	// snapshot 是给外部读的只读快照。actor 自己写，HTTP 侧读。
	snap *snapshot
}

func New(dev string, st Store, pub Publisher, o Options) *Actor {
	if o.Now == nil {
		o.Now = time.Now
	}
	if o.JobTimeout == 0 {
		o.JobTimeout = 180 * time.Second
	}
	if o.IdleTimeout == 0 {
		// 设备保证状态一变 2 秒内推、没变也 30 秒一拍，连丢四拍才判离线。
		o.IdleTimeout = 120 * time.Second
	}
	return &Actor{
		dev: dev, st: st, pub: pub,
		box:         make(chan Msg, 16),
		ticker:      o.Ticker,
		done:        make(chan struct{}),
		jobTimeout:  o.JobTimeout,
		idleTimeout: o.IdleTimeout,
		now:         o.Now,
		lastSeen:    o.Now(),
		snap:        newSnapshot(),
	}
}

func (a *Actor) Dev() string           { return a.dev }
func (a *Actor) Done() <-chan struct{} { return a.done }

// Send 投递消息。满了就丢并记日志——mailbox 堆积说明 actor 卡住了，
// 阻塞调用方（MQTT 回调或 HTTP handler）只会把问题扩散出去。
func (a *Actor) Send(m Msg) bool {
	select {
	case a.box <- m:
		return true
	default:
		slog.Warn("actor mailbox 满，丢弃消息", "dev", a.dev, "kind", m.Kind)
		return false
	}
}

// Run 阻塞运行直到离线或 stop。返回即表示该 actor 已死，registry 负责摘除。
func (a *Actor) Run(stop <-chan struct{}) {
	tick := a.ticker
	if tick == nil {
		t := time.NewTicker(time.Second)
		defer t.Stop()
		tick = t.C
	}
	defer a.shutdown()
	for {
		select {
		case <-stop:
			return
		case m := <-a.box:
			a.handle(m)
		case <-tick:
			if a.idleExpired() {
				slog.Info("设备离线，actor 退出", "dev", a.dev)
				return
			}
			a.handle(Msg{Kind: KindTick})
		}
	}
}

func (a *Actor) shutdown() {
	select {
	case <-a.done:
	default:
		close(a.done)
	}
}

func (a *Actor) idleExpired() bool {
	return a.now().Sub(a.lastSeen) > a.idleTimeout
}

// handle 是全部业务逻辑。单独拆出来是为了在测试里同步调用，不用起 goroutine。
func (a *Actor) handle(m Msg) {
	switch m.Kind {
	case KindHeartbeat:
		a.lastSeen = a.now()
		if m.Printer != nil {
			a.printer = m.Printer
		}
		a.setSerial(m.Serial)

	case KindDownloading:
		a.lastSeen = a.now()
		if m.JobID == a.inflight {
			a.deadline = a.now().Add(a.jobTimeout) // 取件有进展，续命
		}

	case KindJobDone, KindJobFailed:
		a.lastSeen = a.now()
		state, errMsg := store.StateDone, ""
		if m.Kind == KindJobFailed {
			state, errMsg = store.StateFailed, m.Err
		}
		if err := a.st.SetJobState(m.JobID, state, m.Bytes, errMsg); err != nil {
			slog.Error("回执落库失败", "dev", a.dev, "job", m.JobID, "err", err)
		}
		if m.JobID == a.inflight {
			a.inflight = ""
		}

	case KindTick:
		if a.inflight != "" && a.now().After(a.deadline) {
			slog.Warn("传输超时，退回队列", "dev", a.dev, "job", a.inflight)
			if err := a.st.SetJobState(a.inflight, store.StateQueued, 0, ""); err != nil {
				slog.Error("退回队列失败", "job", a.inflight, "err", err)
			}
			a.inflight = ""
		}

	case KindWake:
		// 只是叫醒，状态更新在下面统一做
	}
	a.dispatchOne()
	a.snap.set(a.serial, a.printer)
}

// setSerial 处理换打印机。
//
// 正在传的那件是为旧打印机光栅的，必须退回队列——URF 按特定 dpi 和像素尺寸
// 生成，派给另一台就是一沓废纸，而服务端不解析文档、设备不认识格式，
// 没有任何环节会发现。
//
// 旧打印机的作业留在队列里不动，等它插回来。这是「离线不丢件」的自然延伸，
// 只是「不在线」的粒度从桥细化到了打印机。
func (a *Actor) setSerial(serial string) {
	if serial == a.serial {
		return
	}
	slog.Info("打印机变更", "dev", a.dev, "from", a.serial, "to", serial)
	a.serial = serial
	if a.inflight != "" {
		if err := a.st.SetJobState(a.inflight, store.StateQueued, 0, ""); err != nil {
			slog.Error("换打印机时退回作业失败", "job", a.inflight, "err", err)
		}
		a.inflight = ""
	}
}

// dispatchOne 是「一次只派一件」的全部实现。
func (a *Actor) dispatchOne() {
	if a.inflight != "" {
		return
	}
	if a.serial == "" {
		return // 没插打印机，没什么可派的
	}
	j, ok, err := a.st.NextQueued(a.dev, a.serial)
	if err != nil {
		slog.Error("取队首失败", "dev", a.dev, "err", err)
		return
	}
	if !ok {
		return
	}
	if err := a.pub.PublishJob(a.dev, j.ID, j.Size); err != nil {
		slog.Error("派发失败", "dev", a.dev, "job", j.ID, "err", err)
		return // 作业留在 queued，下次 tick 重试
	}
	if err := a.st.SetJobState(j.ID, store.StateDownloading, 0, ""); err != nil {
		slog.Error("标记派发失败", "job", j.ID, "err", err)
	}
	a.inflight = j.ID
	a.deadline = a.now().Add(a.jobTimeout)
	slog.Info("派发", "dev", a.dev, "job", j.ID, "size", j.Size)
}
