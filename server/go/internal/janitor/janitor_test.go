package janitor

import (
	"os"
	"path/filepath"
	"testing"
	"time"

	"github.com/dayuer/esp-airprint/server/go/internal/store"
)

func setup(t *testing.T) (*store.Store, string) {
	t.Helper()
	dir := t.TempDir()
	jobs := filepath.Join(dir, "jobs")
	if err := os.MkdirAll(jobs, 0o755); err != nil {
		t.Fatal(err)
	}
	st, err := store.Open(filepath.Join(dir, "jobs.db"))
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { st.Close() })
	return st, jobs
}

func touch(t *testing.T, dir, jid string) {
	t.Helper()
	if err := os.WriteFile(filepath.Join(dir, jid+".urf"), []byte("x"), 0o644); err != nil {
		t.Fatal(err)
	}
}

func exists(dir, jid string) bool {
	_, err := os.Stat(filepath.Join(dir, jid+".urf"))
	return err == nil
}

var now = time.Unix(1_000_000, 0)

func clock() time.Time { return now }

func TestSweepDeletesOldDoneJobs(t *testing.T) {
	st, dir := setup(t)
	old := now.Add(-25 * time.Hour).Unix()
	fresh := now.Add(-1 * time.Hour).Unix()

	st.InsertJob(store.Job{ID: "old", Dev: "d", Serial: "P",
		State: store.StateDone, Created: old, Updated: old})
	st.InsertJob(store.Job{ID: "new", Dev: "d", Serial: "P",
		State: store.StateDone, Created: fresh, Updated: fresh})
	touch(t, dir, "old")
	touch(t, dir, "new")

	n, err := New(st, dir, 24, 72, 50, clock).Sweep()
	if err != nil {
		t.Fatal(err)
	}
	if n != 1 {
		t.Errorf("删了 %d 个，期望 1", n)
	}
	if exists(dir, "old") {
		t.Error("24 小时前完成的作业文件没删")
	}
	if !exists(dir, "new") {
		t.Error("刚完成的作业文件被误删")
	}
}

// failed 留久一点——要留给排查。
func TestSweepKeepsFailedLonger(t *testing.T) {
	st, dir := setup(t)
	ts := now.Add(-48 * time.Hour).Unix()
	st.InsertJob(store.Job{ID: "f", Dev: "d", Serial: "P",
		State: store.StateFailed, Created: ts, Updated: ts})
	touch(t, dir, "f")

	New(st, dir, 24, 72, 50, clock).Sweep()
	if !exists(dir, "f") {
		t.Error("48 小时的失败作业不该删（保留 72 小时）")
	}
}

// 排队中的作业永远不删——用户可能只是换了台打印机，插回来就该接着打。
func TestSweepNeverDeletesQueued(t *testing.T) {
	st, dir := setup(t)
	ts := now.Add(-1000 * time.Hour).Unix()
	st.InsertJob(store.Job{ID: "q", Dev: "d", Serial: "P",
		State: store.StateQueued, Created: ts, Updated: ts})
	touch(t, dir, "q")

	New(st, dir, 24, 72, 50, clock).Sweep()
	if !exists(dir, "q") {
		t.Error("排队中的作业被删了——用户插回那台打印机就打不出来了")
	}
}

func TestSweepEnforcesPerDeviceCap(t *testing.T) {
	st, dir := setup(t)
	for i := 0; i < 5; i++ {
		id := string(rune('a' + i))
		ts := now.Add(-time.Duration(i) * time.Minute).Unix()
		st.InsertJob(store.Job{ID: id, Dev: "d", Serial: "P",
			State: store.StateDone, Created: ts, Updated: ts})
		touch(t, dir, id)
	}
	// 时长阈值设成很久以前（都不过期），只看每设备上限 3
	New(st, dir, 1000, 1000, 3, clock).Sweep()

	kept := 0
	for i := 0; i < 5; i++ {
		if exists(dir, string(rune('a'+i))) {
			kept++
		}
	}
	if kept != 3 {
		t.Errorf("留下了 %d 个文件，期望 3", kept)
	}
	if !exists(dir, "a") {
		t.Error("删的应该是最旧的，最新的 a 不该被删")
	}
}

// 文件已经不在了不该报错——数据库记录会比文件活得久。
func TestSweepToleratesMissingFile(t *testing.T) {
	st, dir := setup(t)
	ts := now.Add(-25 * time.Hour).Unix()
	st.InsertJob(store.Job{ID: "gone", Dev: "d", Serial: "P",
		State: store.StateDone, Created: ts, Updated: ts})
	n, err := New(st, dir, 24, 72, 50, clock).Sweep()
	if err != nil {
		t.Fatalf("文件不存在不该报错：%v", err)
	}
	if n != 0 {
		t.Errorf("删了 %d 个不存在的文件", n)
	}
}
