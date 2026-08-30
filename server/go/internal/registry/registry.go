// Package registry 管 devid → actor 的映射与 actor 的生死。
//
// 一台设备一个 goroutine。actor 的状态全部私有，registry 只负责创建、
// 路由消息、在它退出后摘除，以及 panic 时摘掉它。
package registry

import (
	"log/slog"
	"sync"

	"github.com/dayuer/esp-airprint/server/go/internal/device"
	"github.com/dayuer/esp-airprint/server/go/internal/store"
)

type Registry struct {
	st   *store.Store
	pub  device.Publisher
	opts device.Options

	mu    sync.RWMutex
	items map[string]*device.Actor
	stop  chan struct{}
	wg    sync.WaitGroup
}

func New(st *store.Store, pub device.Publisher, opts device.Options) *Registry {
	return &Registry{
		st: st, pub: pub, opts: opts,
		items: map[string]*device.Actor{},
		stop:  make(chan struct{}),
	}
}

// Send 把消息投给该设备的 actor，没有就先建一个。
func (r *Registry) Send(dev string, m device.Msg) {
	if a := r.getOrCreate(dev); a != nil {
		a.Send(m)
	}
}

func (r *Registry) getOrCreate(dev string) *device.Actor {
	r.mu.RLock()
	a, ok := r.items[dev]
	r.mu.RUnlock()
	if ok {
		return a
	}

	r.mu.Lock()
	defer r.mu.Unlock()
	if a, ok := r.items[dev]; ok { // 并发下别人先建好了
		return a
	}
	select {
	case <-r.stop:
		return nil
	default:
	}
	a = device.New(dev, r.st, r.pub, r.opts)
	r.items[dev] = a
	r.wg.Add(1)
	go r.run(dev, a)
	return a
}

// run 跑一个 actor，并在它退出或 panic 时把它摘掉。
//
// 不在这里自动重建：重建会立刻再收到同样的消息、再 panic 一次，成为忙循环。
// 摘掉之后，下一条消息自然会建一个干净的 actor——那时状态是全新的。
// inflight 作业由启动恢复和 180 秒超时兜底，不会永久卡住。
func (r *Registry) run(dev string, a *device.Actor) {
	defer r.wg.Done()
	defer func() {
		if v := recover(); v != nil {
			slog.Error("actor panic，已摘除", "dev", dev, "panic", v)
		}
		r.mu.Lock()
		if cur, ok := r.items[dev]; ok && cur == a {
			delete(r.items, dev)
		}
		r.mu.Unlock()
	}()
	a.Run(r.stop)
}

func (r *Registry) Online() []string {
	r.mu.RLock()
	defer r.mu.RUnlock()
	out := make([]string, 0, len(r.items))
	for d := range r.items {
		out = append(out, d)
	}
	return out
}

// Actor 取一个在线 actor，用于读它的当前状态（serial、面板状态）。
// 不在线返回 nil。
func (r *Registry) Actor(dev string) *device.Actor {
	r.mu.RLock()
	defer r.mu.RUnlock()
	return r.items[dev]
}

func (r *Registry) Shutdown() {
	r.mu.Lock()
	select {
	case <-r.stop:
	default:
		close(r.stop)
	}
	r.mu.Unlock()
	r.wg.Wait()
}
