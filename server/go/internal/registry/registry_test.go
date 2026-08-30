package registry

import (
	"sync"
	"testing"
	"time"

	"github.com/dayuer/esp-airprint/server/go/internal/device"
	"github.com/dayuer/esp-airprint/server/go/internal/store"
)

type nopPub struct {
	mu sync.Mutex
	n  int
}

func (p *nopPub) PublishJob(dev, job string, size int64) error {
	p.mu.Lock()
	p.n++
	p.mu.Unlock()
	return nil
}

func newReg(t *testing.T, idle time.Duration) (*Registry, *store.Store, *nopPub) {
	t.Helper()
	st, err := store.Open(t.TempDir() + "/jobs.db")
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { st.Close() })
	pub := &nopPub{}
	r := New(st, pub, device.Options{
		JobTimeout:  180 * time.Second,
		IdleTimeout: idle,
	})
	t.Cleanup(r.Shutdown)
	return r, st, pub
}

// 第一条消息就该把 actor 建起来。
func TestSendCreatesActor(t *testing.T) {
	r, _, _ := newReg(t, time.Hour)
	r.Send("d1", device.Msg{Kind: device.KindHeartbeat, Serial: "PA"})
	if got := r.Online(); len(got) != 1 || got[0] != "d1" {
		t.Fatalf("在线列表 = %v，期望 [d1]", got)
	}
}

// 离线后 actor 自己退出并被摘除，不能泄漏 goroutine。
func TestActorExitsAndIsRemoved(t *testing.T) {
	r, _, _ := newReg(t, 50*time.Millisecond)
	r.Send("d1", device.Msg{Kind: device.KindHeartbeat, Serial: "PA"})
	deadline := time.Now().Add(5 * time.Second)
	for time.Now().Before(deadline) {
		if len(r.Online()) == 0 {
			return
		}
		time.Sleep(20 * time.Millisecond)
	}
	t.Fatal("超过 IdleTimeout 后 actor 仍在 registry 里")
}

// 摘除后再来消息要能重建，且队列还在。
func TestActorRebuildsAfterExit(t *testing.T) {
	r, st, _ := newReg(t, 50*time.Millisecond)
	st.InsertJob(store.Job{ID: "j1", Dev: "d1", Serial: "PA", State: store.StateQueued})

	r.Send("d1", device.Msg{Kind: device.KindHeartbeat, Serial: "PA"})
	deadline := time.Now().Add(5 * time.Second)
	for len(r.Online()) > 0 && time.Now().Before(deadline) {
		time.Sleep(20 * time.Millisecond)
	}
	if _, ok, _ := st.GetJob("j1"); !ok {
		t.Fatal("actor 退出把作业带走了")
	}
	r.Send("d1", device.Msg{Kind: device.KindHeartbeat, Serial: "PA"})
	if len(r.Online()) != 1 {
		t.Fatal("actor 没能重建")
	}
}

func TestConcurrentSendCreatesOneActor(t *testing.T) {
	r, _, _ := newReg(t, time.Hour)
	var wg sync.WaitGroup
	for i := 0; i < 50; i++ {
		wg.Add(1)
		go func() {
			defer wg.Done()
			r.Send("d1", device.Msg{Kind: device.KindHeartbeat, Serial: "PA"})
		}()
	}
	wg.Wait()
	if len(r.Online()) != 1 {
		t.Errorf("并发建了 %d 个 actor，期望 1 个", len(r.Online()))
	}
}

func TestShutdownIsIdempotent(t *testing.T) {
	r, _, _ := newReg(t, time.Hour)
	r.Send("d1", device.Msg{Kind: device.KindHeartbeat, Serial: "PA"})
	r.Shutdown()
	r.Shutdown() // 第二次不能 panic
	if len(r.Online()) != 0 {
		t.Errorf("关闭后仍有 %d 个 actor", len(r.Online()))
	}
}
