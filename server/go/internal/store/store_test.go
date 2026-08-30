package store

import (
	"path/filepath"
	"testing"
)

func open(t *testing.T) *Store {
	t.Helper()
	s, err := Open(filepath.Join(t.TempDir(), "jobs.db"))
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { s.Close() })
	return s
}

func TestOpenIsIdempotent(t *testing.T) {
	p := filepath.Join(t.TempDir(), "jobs.db")
	for i := 0; i < 2; i++ {
		s, err := Open(p)
		if err != nil {
			t.Fatalf("第 %d 次 Open: %v", i+1, err)
		}
		s.Close()
	}
}

// 进程重启时正在传的作业必须退回队列。
// Python 版只处理了 no-device 一种，downloading 会永远卡住。
func TestRecoverOnBootRequeuesStuckJobs(t *testing.T) {
	s := open(t)
	for _, j := range []Job{
		{ID: "a", Dev: "d1", State: "downloading"},
		{ID: "b", Dev: "d1", State: "no-device"},
		{ID: "c", Dev: "d1", State: "rendering"},
		{ID: "d", Dev: "d1", State: "done"},
		{ID: "e", Dev: "d1", State: "queued"},
	} {
		if err := s.InsertJob(j); err != nil {
			t.Fatal(err)
		}
	}
	n, err := s.RecoverOnBoot()
	if err != nil {
		t.Fatal(err)
	}
	if n != 3 {
		t.Errorf("恢复了 %d 件，期望 3（a/b/c）", n)
	}
	for _, id := range []string{"a", "b", "c", "e"} {
		j, ok, _ := s.GetJob(id)
		if !ok || j.State != "queued" {
			t.Errorf("作业 %s 状态 = %q，期望 queued", id, j.State)
		}
	}
	if j, _, _ := s.GetJob("d"); j.State != "done" {
		t.Errorf("已完成的作业被误改成 %q", j.State)
	}
}

// 队列顺序必须按 created 先进先出，且只取该设备、该打印机的。
func TestNextQueuedIsFIFOAndPerDevice(t *testing.T) {
	s := open(t)
	s.InsertJob(Job{ID: "j2", Dev: "d1", Serial: "PA", State: "queued", Created: 200})
	s.InsertJob(Job{ID: "j1", Dev: "d1", Serial: "PA", State: "queued", Created: 100})
	s.InsertJob(Job{ID: "x1", Dev: "d2", Serial: "PA", State: "queued", Created: 50})
	s.InsertJob(Job{ID: "r1", Dev: "d1", Serial: "PA", State: "downloading", Created: 10})

	j, ok, err := s.NextQueued("d1", "PA")
	if err != nil || !ok {
		t.Fatalf("NextQueued: ok=%v err=%v", ok, err)
	}
	if j.ID != "j1" {
		t.Errorf("取到 %s，期望最早入队的 j1", j.ID)
	}
}

// 用户换打印机后，为旧机器光栅的作业绝不能派给新机器——
// URF 是按特定 dpi 和像素尺寸生成的，派错了就是一沓废纸，
// 而且服务端不解析文档、设备不认识格式，没有任何环节会发现。
func TestNextQueuedFiltersByPrinter(t *testing.T) {
	s := open(t)
	s.InsertJob(Job{ID: "old", Dev: "d1", Serial: "PA", State: "queued", Created: 1})
	s.InsertJob(Job{ID: "new", Dev: "d1", Serial: "PB", State: "queued", Created: 2})

	j, ok, _ := s.NextQueued("d1", "PB")
	if !ok || j.ID != "new" {
		t.Fatalf("取到 %+v，期望只拿到 PB 的作业", j)
	}
	if j, ok, _ := s.GetJob("old"); !ok || j.State != StateQueued {
		t.Errorf("旧打印机的作业被动了：state=%q", j.State)
	}
}

func TestNextQueuedEmpty(t *testing.T) {
	s := open(t)
	if _, ok, err := s.NextQueued("nobody", "PA"); ok || err != nil {
		t.Errorf("空队列应返回 ok=false，得到 ok=%v err=%v", ok, err)
	}
}

func TestQueuedCountByPrinter(t *testing.T) {
	s := open(t)
	s.InsertJob(Job{ID: "a", Dev: "d1", Serial: "PA", State: "queued", Created: 1})
	s.InsertJob(Job{ID: "b", Dev: "d1", Serial: "PA", State: "queued", Created: 2})
	s.InsertJob(Job{ID: "c", Dev: "d1", Serial: "PB", State: "queued", Created: 3})
	s.InsertJob(Job{ID: "d", Dev: "d1", Serial: "PB", State: "done", Created: 4})

	m, err := s.QueuedCountByPrinter("d1")
	if err != nil {
		t.Fatal(err)
	}
	if m["PA"] != 2 || m["PB"] != 1 {
		t.Errorf("统计 = %+v，期望 PA:2 PB:1", m)
	}
}

func TestSetJobStateRecordsError(t *testing.T) {
	s := open(t)
	s.InsertJob(Job{ID: "j", Dev: "d", Serial: "PA", State: "downloading"})
	if err := s.SetJobState("j", "failed", 0, "USB 写入超时"); err != nil {
		t.Fatal(err)
	}
	j, _, _ := s.GetJob("j")
	if j.State != "failed" || j.Err != "USB 写入超时" {
		t.Errorf("state=%q err=%q", j.State, j.Err)
	}
	if j.Updated == 0 {
		t.Error("updated 未被刷新")
	}
}

func TestJobsForDeviceNewestFirst(t *testing.T) {
	s := open(t)
	s.InsertJob(Job{ID: "old", Dev: "d", Serial: "PA", State: "done", Created: 1})
	s.InsertJob(Job{ID: "new", Dev: "d", Serial: "PA", State: "done", Created: 9})
	s.InsertJob(Job{ID: "other", Dev: "zzz", Serial: "PA", State: "done", Created: 5})
	js, err := s.JobsForDevice("d", 15)
	if err != nil {
		t.Fatal(err)
	}
	if len(js) != 2 || js[0].ID != "new" {
		t.Fatalf("得到 %+v，期望 [new old]", js)
	}
}

func TestExpiredJobFilesByAge(t *testing.T) {
	s := open(t)
	s.InsertJob(Job{ID: "olddone", Dev: "d", Serial: "P", State: StateDone, Created: 1, Updated: 100})
	s.InsertJob(Job{ID: "newdone", Dev: "d", Serial: "P", State: StateDone, Created: 2, Updated: 900})
	s.InsertJob(Job{ID: "oldfail", Dev: "d", Serial: "P", State: StateFailed, Created: 3, Updated: 100})
	s.InsertJob(Job{ID: "queued", Dev: "d", Serial: "P", State: StateQueued, Created: 4, Updated: 1})

	got, err := s.ExpiredJobFiles(500, 50, 100) // done 阈值 500，failed 阈值 50
	if err != nil {
		t.Fatal(err)
	}
	set := map[string]bool{}
	for _, id := range got {
		set[id] = true
	}
	if !set["olddone"] {
		t.Error("过期的 done 没被选中")
	}
	if set["newdone"] {
		t.Error("未过期的 done 被误选")
	}
	if set["oldfail"] {
		t.Error("failed 的阈值更宽松，不该选中")
	}
	// 排队中的作业永远不删——用户插回那台打印机就该接着打
	if set["queued"] {
		t.Error("排队中的作业被选中删除")
	}
}

func TestExpiredJobFilesPerDeviceCap(t *testing.T) {
	s := open(t)
	for i := 0; i < 5; i++ {
		s.InsertJob(Job{ID: string(rune('a' + i)), Dev: "d", Serial: "P",
			State: StateDone, Created: int64(i), Updated: int64(1000 + i)})
	}
	got, err := s.ExpiredJobFiles(0, 0, 3) // 时长阈值设 0（都不过期），只看每设备上限
	if err != nil {
		t.Fatal(err)
	}
	if len(got) != 2 {
		t.Fatalf("选中 %v，期望删最旧的两件", got)
	}
	set := map[string]bool{got[0]: true, got[1]: true}
	if !set["a"] || !set["b"] {
		t.Errorf("选中 %v，期望是最旧的 a 和 b", got)
	}
}
