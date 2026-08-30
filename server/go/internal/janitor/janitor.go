// Package janitor 清作业文件。
//
// URF 是光栅，单份 200KB~15MB，比 PDF 大一到两个数量级。不清理会很快写满盘。
// 内建而不是靠 cron 脚本——那是会被忘记的东西。
package janitor

import (
	"log/slog"
	"os"
	"path/filepath"
	"time"
)

type Store interface {
	ExpiredJobFiles(doneBefore, failBefore int64, perDev int) ([]string, error)
}

type Janitor struct {
	st      Store
	dir     string
	doneHrs int
	failHrs int
	perDev  int
	now     func() time.Time
}

func New(st Store, jobsDir string, doneHrs, failHrs, perDev int, now func() time.Time) *Janitor {
	if now == nil {
		now = time.Now
	}
	return &Janitor{st: st, dir: jobsDir, doneHrs: doneHrs,
		failHrs: failHrs, perDev: perDev, now: now}
}

// Sweep 扫一遍，返回删除的文件数。
//
// 只删已结束作业的文件。queued 永远不删——用户可能只是换了台打印机，
// 插回来就该接着打（见 spec 第 5b 节）。
func (j *Janitor) Sweep() (int, error) {
	now := j.now().Unix()
	victims, err := j.st.ExpiredJobFiles(
		now-int64(j.doneHrs)*3600,
		now-int64(j.failHrs)*3600,
		j.perDev)
	if err != nil {
		return 0, err
	}
	n := 0
	for _, jid := range victims {
		if err := os.Remove(filepath.Join(j.dir, jid+".urf")); err == nil {
			n++
		}
	}
	if n > 0 {
		slog.Info("清理作业文件", "count", n)
	}
	return n, nil
}

// Run 每小时扫一次，直到 stop 关闭。
func (j *Janitor) Run(stop <-chan struct{}) {
	t := time.NewTicker(time.Hour)
	defer t.Stop()
	for {
		select {
		case <-stop:
			return
		case <-t.C:
			if _, err := j.Sweep(); err != nil {
				slog.Error("清理失败", "err", err)
			}
		}
	}
}
