package device

import (
	"testing"
	"time"

	"github.com/dayuer/esp-airprint/server/go/internal/store"
)

type published struct {
	dev, job string
	size     int64
}

type fakePub struct{ sent []published }

func (f *fakePub) PublishJob(dev, job string, size int64) error {
	f.sent = append(f.sent, published{dev, job, size})
	return nil
}

var fakeNow = time.Unix(1_000_000, 0)

func newActor(t *testing.T) (*Actor, *store.Store, *fakePub, chan time.Time) {
	t.Helper()
	st, err := store.Open(t.TempDir() + "/jobs.db")
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { st.Close() })
	pub := &fakePub{}
	tick := make(chan time.Time)
	a := New("d1", st, pub, Options{
		Ticker:      tick,
		JobTimeout:  180 * time.Second,
		IdleTimeout: 5 * time.Minute,
		Now:         func() time.Time { return fakeNow },
	})
	// 默认认为插着打印机 PA，否则什么都不会派。
	a.handle(Msg{Kind: KindHeartbeat, Serial: "PA"})
	return a, st, pub, tick
}

func queue(t *testing.T, st *store.Store, id, serial string, created int64, size int64) {
	t.Helper()
	if err := st.InsertJob(store.Job{ID: id, Dev: "d1", Serial: serial, Size: size,
		State: store.StateQueued, Created: created}); err != nil {
		t.Fatal(err)
	}
}

func TestDispatchesOneJobOnWake(t *testing.T) {
	a, st, pub, _ := newActor(t)
	queue(t, st, "j1", "PA", 1, 4096)
	queue(t, st, "j2", "PA", 2, 8192)

	a.handle(Msg{Kind: KindWake})

	if len(pub.sent) != 1 {
		t.Fatalf("派发了 %d 件，必须只派一件——设备没有本地队列", len(pub.sent))
	}
	if pub.sent[0].job != "j1" || pub.sent[0].size != 4096 {
		t.Errorf("派发了 %+v，期望 j1/4096", pub.sent[0])
	}
}

func TestSecondWakeDoesNotRedispatch(t *testing.T) {
	a, st, pub, _ := newActor(t)
	queue(t, st, "j1", "PA", 1, 0)
	queue(t, st, "j2", "PA", 2, 0)

	a.handle(Msg{Kind: KindWake})
	a.handle(Msg{Kind: KindWake})
	a.handle(Msg{Kind: KindHeartbeat, Serial: "PA"})

	if len(pub.sent) != 1 {
		t.Fatalf("重复派发 %d 次——inflight 判断失效", len(pub.sent))
	}
}

func TestDoneTriggersNextJob(t *testing.T) {
	a, st, pub, _ := newActor(t)
	queue(t, st, "j1", "PA", 1, 0)
	queue(t, st, "j2", "PA", 2, 0)

	a.handle(Msg{Kind: KindWake})
	a.handle(Msg{Kind: KindJobDone, JobID: "j1", Bytes: 4096})

	if len(pub.sent) != 2 || pub.sent[1].job != "j2" {
		t.Fatalf("回执后未续派下一件，sent=%+v", pub.sent)
	}
	j, _, _ := st.GetJob("j1")
	if j.State != store.StateDone || j.Bytes != 4096 {
		t.Errorf("j1 落库 state=%q bytes=%d", j.State, j.Bytes)
	}
}

func TestFailedAlsoTriggersNextJob(t *testing.T) {
	a, st, pub, _ := newActor(t)
	queue(t, st, "j1", "PA", 1, 0)
	queue(t, st, "j2", "PA", 2, 0)

	a.handle(Msg{Kind: KindWake})
	a.handle(Msg{Kind: KindJobFailed, JobID: "j1", Err: "USB 超时"})

	if len(pub.sent) != 2 {
		t.Fatalf("失败后队列停摆，sent=%+v", pub.sent)
	}
	if j, _, _ := st.GetJob("j1"); j.State != store.StateFailed || j.Err != "USB 超时" {
		t.Errorf("j1 state=%q err=%q", j.State, j.Err)
	}
}

// 180 秒无进展退回队列重传，语义与 Python 版一致。
func TestTimeoutRequeues(t *testing.T) {
	a, st, pub, _ := newActor(t)
	queue(t, st, "j1", "PA", 1, 0)

	a.handle(Msg{Kind: KindWake})
	a.now = func() time.Time { return fakeNow.Add(181 * time.Second) }
	a.handle(Msg{Kind: KindTick})

	if len(pub.sent) != 2 {
		t.Errorf("退回后应立即重派，sent=%d", len(pub.sent))
	}
}

// 设备正在取件时不能被超时误杀。
func TestDownloadingExtendsDeadline(t *testing.T) {
	a, st, pub, _ := newActor(t)
	queue(t, st, "j1", "PA", 1, 0)

	a.handle(Msg{Kind: KindWake})
	a.now = func() time.Time { return fakeNow.Add(170 * time.Second) }
	a.handle(Msg{Kind: KindDownloading, JobID: "j1"})
	a.now = func() time.Time { return fakeNow.Add(300 * time.Second) }
	a.handle(Msg{Kind: KindTick})

	if j, _, _ := st.GetJob("j1"); j.State != store.StateDownloading {
		t.Errorf("续命失败，j1 state=%q", j.State)
	}
	if len(pub.sent) != 1 {
		t.Errorf("不应重派，sent=%d", len(pub.sent))
	}
}

func TestEmptyQueueIsQuiet(t *testing.T) {
	a, _, pub, _ := newActor(t)
	a.handle(Msg{Kind: KindWake})
	if len(pub.sent) != 0 {
		t.Errorf("空队列不该发任何信令，sent=%d", len(pub.sent))
	}
}

// 换打印机：正在传的那件必须退回队列，且不能派给新打印机。
func TestPrinterSwapRequeuesInflightAndFiltersQueue(t *testing.T) {
	a, st, pub, _ := newActor(t)
	queue(t, st, "a1", "PA", 1, 0)
	queue(t, st, "b1", "PB", 2, 0)

	a.handle(Msg{Kind: KindWake})
	if len(pub.sent) != 1 || pub.sent[0].job != "a1" {
		t.Fatalf("首次应派 PA 的作业，sent=%+v", pub.sent)
	}

	// 用户拔掉 PA 换上 PB
	a.handle(Msg{Kind: KindHeartbeat, Serial: "PB"})

	if len(pub.sent) != 2 || pub.sent[1].job != "b1" {
		t.Fatalf("换机后应派 PB 的作业，sent=%+v", pub.sent)
	}
	// PA 的作业等它插回来，不能失败也不能删
	if j, ok, _ := st.GetJob("a1"); !ok || j.State != store.StateQueued {
		t.Errorf("PA 的作业丢了：ok=%v state=%q", ok, j.State)
	}
}

// 没插打印机时什么都不派。
func TestNoPrinterNoDispatch(t *testing.T) {
	a, st, pub, _ := newActor(t)
	queue(t, st, "a1", "PA", 1, 0)
	a.handle(Msg{Kind: KindHeartbeat, Serial: ""})
	a.handle(Msg{Kind: KindWake})
	if len(pub.sent) != 0 {
		t.Errorf("没插打印机不该派活，sent=%d", len(pub.sent))
	}
}

// 队列的真相在 sqlite：actor 退出后作业必须原样还在。
func TestIdleExitLeavesQueueIntact(t *testing.T) {
	a, st, _, _ := newActor(t)
	queue(t, st, "j1", "PA", 1, 0)
	a.handle(Msg{Kind: KindWake})

	a.now = func() time.Time { return fakeNow.Add(10 * time.Minute) }
	if !a.idleExpired() {
		t.Fatal("超过 IdleTimeout 未判定为离线")
	}
	a.shutdown()

	if _, ok, _ := st.GetJob("j1"); !ok {
		t.Error("actor 退出把作业带走了")
	}
}

// mailbox 满了要丢弃而不是阻塞——阻塞会把问题扩散到 MQTT 回调和 HTTP handler。
func TestSendDoesNotBlockWhenFull(t *testing.T) {
	a, _, _, _ := newActor(t)
	for i := 0; i < 16; i++ {
		if !a.Send(Msg{Kind: KindWake}) {
			t.Fatalf("第 %d 条就满了，缓冲应为 16", i+1)
		}
	}
	done := make(chan bool, 1)
	go func() { done <- a.Send(Msg{Kind: KindWake}) }()
	select {
	case ok := <-done:
		if ok {
			t.Error("满了却报告投递成功")
		}
	case <-time.After(time.Second):
		t.Fatal("Send 阻塞了")
	}
}
