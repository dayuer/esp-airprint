# 云打印服务端 Go 重写 实现计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 把 `server/bin/jobsrv.py` 重写为单个 Go 二进制 `stickboxd`，内嵌 MQTT broker，
每设备一密钥，纯 API 无网页。

**Architecture:** 单进程同时监听 `:8883`（内嵌 mochi-mqtt over TLS）和 `:9443`（HTTPS JSON API），
共用一份证书并热重载。每台在线设备一个 goroutine（actor）持有该设备的派发状态机，
作业队列的真相始终在 sqlite。**服务端不渲染**——App 上传已光栅的 URF，
服务端只校验魔数/页数/尺寸，然后排队转发。部署物是一个 Go 二进制加一个 sqlite。

**Tech Stack:** Go 1.26 / mochi-mqtt v2（内嵌 broker）/ modernc.org/sqlite（纯 Go，无 cgo）/
golang.org/x/crypto/argon2 / 标准库 net/http、crypto/tls。**无 CUPS、无 Python、无外部服务。**

**Spec:** `docs/superpowers/specs/2026-08-30-go-print-server-design.md`

---

## 阅读顺序（实现前必读）

1. 本计划配套的 spec（上面那份），尤其第 5 节「设备 actor」的两条硬规则
2. `docs/HANDOFF-cloud-print.md` 第 1 节（职责划分）和第 4 节（服务端现状）
3. `server/bin/jobsrv.py` —— 被替换的对象，行为语义以它为准

**领域约束（违反会导致设备侧故障，不是风格问题）：**

- 文档**绝不走 MQTT**。设备可用堆 70~120KB，作业 200KB~15MB，MQTT 消息必须整包进内存。
- **一次只派一件**。设备没有本地队列，一次性堆过去等于丢件。
- MQTT 信令只传几十字节。
- **上传必须做入口校验**。服务端不解析文档，设备也不认识格式——一份标称 URF 的
  PDF 会被原样送进打印机，用户收到几十张乱码纸。魔数、页数字段、首页尺寸三条都要查。

---

## 文件结构

全部新代码在 `server/go/` 下，Python 渲染脚本原地不动。

| 文件 | 职责 |
|---|---|
| `server/go/go.mod` | 模块 `github.com/dayuer/stickbox/server/go` |
| `server/go/internal/config/config.go` | 读 `config.json`，字段校验与默认值 |
| `server/go/internal/store/store.go` | sqlite 打开、建表、迁移、启动恢复 |
| `server/go/internal/store/job.go` | `jobs` 表读写 |
| `server/go/internal/store/key.go` | `devices` 表（密钥）读写 |
| `server/go/internal/auth/secret.go` | 密钥生成、argon2id 编码/校验 |
| `server/go/internal/auth/verifier.go` | 令牌解析、校验缓存 |
| `server/go/internal/auth/acl.go` | 角色 × topic 的 ACL 判定 |
| `server/go/internal/device/actor.go` | 设备状态机（本项目核心） |
| `server/go/internal/device/msg.go` | actor 的消息类型 |
| `server/go/internal/registry/registry.go` | devid → actor 分片表、生命周期、panic 恢复 |
| `server/go/internal/raster/verify.go` | URF / PWG-Raster 入口校验（魔数、页数、尺寸） |
| `server/go/internal/raster/profile.go` | 从 ident 的 URF 能力串解析出光栅参数 |
| `server/go/internal/janitor/janitor.go` | 作业文件清理（按时长和每设备件数） |
| `server/go/internal/tlsx/reloader.go` | 证书按 mtime 热重载 |
| `server/go/internal/broker/broker.go` | 内嵌 broker 装配 |
| `server/go/internal/broker/hooks.go` | auth / ACL / OnPublished 钩子 |
| `server/go/internal/httpapi/api.go` | 路由与鉴权中间件 |
| `server/go/internal/httpapi/print.go` | `POST /api/print`（含入口校验） |
| `server/go/internal/httpapi/profile.go` | `GET /api/device/{id}/render-profile` |
| `server/go/internal/httpapi/data.go` | `GET /api/job/{id}/data` |
| `server/go/internal/httpapi/ident.go` | `POST /api/device/{id}/ident` |
| `server/go/internal/httpapi/status.go` | `GET /api/status` |
| `server/go/cmd/stickboxd/main.go` | 装配、优雅退出 |
| `server/go/cmd/stickboxd/device_cmd.go` | `device add/list/revoke` 子命令 |
| `server/go/integration_test.go` | 端到端：真 broker + 真 HTTP + 假设备 |

拆分原则：`device` 包不 import HTTP 和 MQTT 的任何东西，只依赖两个窄接口。
这是它能被单测的前提，评审时按这条检查。

---

## Task 1: 模块骨架

**Files:**
- Create: `server/go/go.mod`
- Create: `server/go/internal/version/version.go`
- Create: `server/go/internal/version/version_test.go`

- [ ] **Step 1: 初始化模块**

```bash
cd server/go 2>/dev/null || mkdir -p server/go
cd server/go
go mod init github.com/dayuer/stickbox/server/go
go mod edit -go=1.26
```

- [ ] **Step 2: 写一个会失败的测试**

`server/go/internal/version/version_test.go`:

```go
package version

import "testing"

func TestStringNotEmpty(t *testing.T) {
	if String() == "" {
		t.Fatal("version.String() 返回空串")
	}
}
```

- [ ] **Step 3: 运行测试确认失败**

Run: `cd server/go && go test ./internal/version/`
Expected: FAIL，`undefined: String`

- [ ] **Step 4: 最小实现**

`server/go/internal/version/version.go`:

```go
// Package version 只做一件事：让二进制能报出自己是哪个版本。
package version

// Version 由构建时 -ldflags 覆盖，默认值用于开发构建。
var Version = "dev"

func String() string { return Version }
```

- [ ] **Step 5: 运行测试确认通过**

Run: `cd server/go && go test ./internal/version/`
Expected: `ok`

- [ ] **Step 6: 提交**

```bash
git add server/go
git commit -m "chore(server): Go 模块骨架"
```

---

## Task 2: config 包

**Files:**
- Create: `server/go/internal/config/config.go`
- Create: `server/go/internal/config/config_test.go`
- Modify: `server/config.example.json`

- [ ] **Step 1: 写失败的测试**

`server/go/internal/config/config_test.go`:

```go
package config

import (
	"os"
	"path/filepath"
	"testing"
)

func write(t *testing.T, body string) string {
	t.Helper()
	p := filepath.Join(t.TempDir(), "config.json")
	if err := os.WriteFile(p, []byte(body), 0o600); err != nil {
		t.Fatal(err)
	}
	return p
}

func TestLoadAppliesDefaults(t *testing.T) {
	c, err := Load(write(t, `{"cert_dir":"/etc/le/live/x"}`))
	if err != nil {
		t.Fatal(err)
	}
	if c.Root != "/opt/stickbox" {
		t.Errorf("Root = %q，期望默认 /opt/stickbox", c.Root)
	}
	if c.HTTPAddr != ":9443" {
		t.Errorf("HTTPAddr = %q，期望默认 :9443", c.HTTPAddr)
	}
	if c.MQTTAddr != ":8883" {
		t.Errorf("MQTTAddr = %q，期望默认 :8883", c.MQTTAddr)
	}
	if c.JobRetainHours != 24 {
		t.Errorf("JobRetainHours = %d，期望默认 24", c.JobRetainHours)
	}
	if c.JobRetainPerDevice != 50 {
		t.Errorf("JobRetainPerDevice = %d，期望默认 50", c.JobRetainPerDevice)
	}
}

func TestLoadRejectsMissingCertDir(t *testing.T) {
	if _, err := Load(write(t, `{}`)); err == nil {
		t.Fatal("cert_dir 缺失时应当报错")
	}
}

func TestDerivedPaths(t *testing.T) {
	c, err := Load(write(t, `{"cert_dir":"/c","root":"/srv/ap"}`))
	if err != nil {
		t.Fatal(err)
	}
	if c.JobsDir() != "/srv/ap/jobs" {
		t.Errorf("JobsDir = %q", c.JobsDir())
	}
	if c.DBPath() != "/srv/ap/jobs.db" {
		t.Errorf("DBPath = %q", c.DBPath())
	}
	if c.CertPath() != "/c/fullchain.pem" {
		t.Errorf("CertPath = %q", c.CertPath())
	}
}
```

- [ ] **Step 2: 运行确认失败**

Run: `cd server/go && go test ./internal/config/`
Expected: FAIL，`undefined: Load`

- [ ] **Step 3: 实现**

`server/go/internal/config/config.go`:

```go
// Package config 读取 config.json。
//
// 口令类字段不再出现在这里——设备密钥进了 sqlite，见 internal/store。
package config

import (
	"encoding/json"
	"errors"
	"os"
	"path/filepath"
)

type Config struct {
	Root          string `json:"root"`
	CertDir       string `json:"cert_dir"`
	HTTPAddr      string `json:"http_addr"`
	MQTTAddr      string `json:"mqtt_addr"`
	JobRetainHours     int `json:"job_retain_hours"`
	JobRetainPerDevice int `json:"job_retain_per_device"`
}

func Load(path string) (*Config, error) {
	raw, err := os.ReadFile(path)
	if err != nil {
		return nil, err
	}
	var c Config
	if err := json.Unmarshal(raw, &c); err != nil {
		return nil, err
	}
	if c.CertDir == "" {
		return nil, errors.New("config: cert_dir 必填")
	}
	if c.Root == "" {
		c.Root = "/opt/stickbox"
	}
	if c.HTTPAddr == "" {
		c.HTTPAddr = ":9443"
	}
	if c.MQTTAddr == "" {
		c.MQTTAddr = ":8883"
	}
	// URF 是光栅，单份 200KB~15MB，不清理很快写满盘。
	if c.JobRetainHours < 1 {
		c.JobRetainHours = 24
	}
	if c.JobRetainPerDevice < 1 {
		c.JobRetainPerDevice = 50
	}
	return &c, nil
}

func (c *Config) JobsDir() string   { return filepath.Join(c.Root, "jobs") }
func (c *Config) IdentsDir() string { return filepath.Join(c.Root, "idents") }
func (c *Config) DBPath() string    { return filepath.Join(c.Root, "jobs.db") }
func (c *Config) CertPath() string  { return filepath.Join(c.CertDir, "fullchain.pem") }
func (c *Config) KeyPath() string   { return filepath.Join(c.CertDir, "privkey.pem") }
```

- [ ] **Step 4: 运行确认通过**

Run: `cd server/go && go test ./internal/config/`
Expected: `ok`

- [ ] **Step 5: 更新示例配置**

`server/config.example.json` 已更新为：

```json
{
  "root": "/opt/stickbox",
  "cert_dir": "/etc/letsencrypt/live/your.host",
  "http_addr": ":9443",
  "mqtt_addr": ":8883",
  "job_retain_hours": 24,
  "job_retain_per_device": 50
}
```

`mqtt_user` / `mqtt_pass` 删除：broker 已内嵌，认证走每设备密钥。
渲染相关字段一个都没有：服务端不渲染。

- [ ] **Step 6: 提交**

```bash
git add server/go/internal/config server/config.example.json
git commit -m "feat(server): config 包，口令字段退场"
```

---

## Task 3: store 包 — 建表与启动恢复

**Files:**
- Create: `server/go/internal/store/store.go`
- Create: `server/go/internal/store/store_test.go`

- [ ] **Step 1: 拉依赖**

```bash
cd server/go && go get modernc.org/sqlite@latest
```

用 `modernc.org/sqlite` 而不是 `mattn/go-sqlite3`：纯 Go，不需要 cgo，
交叉编译和静态部署都省事。驱动名是 `"sqlite"`（不是 `"sqlite3"`）。

- [ ] **Step 2: 写失败的测试**

`server/go/internal/store/store_test.go`:

```go
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

// 进程重启时正在传/正在渲染的作业必须退回队列。
// 旧的 Python 版只处理 no-device 一种，downloading 会永远卡住。
func TestRecoverOnBootRequeuesStuckJobs(t *testing.T) {
	s := open(t)
	for _, j := range []Job{
		{ID: "a", Dev: "d1", State: "downloading"},
		{ID: "b", Dev: "d1", State: "no-device"},
		{ID: "c", Dev: "d1", State: "downloading"},
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
```

- [ ] **Step 3: 运行确认失败**

Run: `cd server/go && go test ./internal/store/`
Expected: FAIL，`undefined: Open`

- [ ] **Step 4: 实现**

`server/go/internal/store/store.go`:

```go
// Package store 是作业与密钥的唯一真相来源。
//
// 关键约束：设备 actor 只在内存里缓存「正在传的那一件」，其余全部在这里。
// actor 退出、进程重启，队列都必须毫发无损。
package store

import (
	"database/sql"
	"sync"

	_ "modernc.org/sqlite"
)

type Store struct {
	db  *sql.DB
	wmu sync.Mutex // 写串行化：百台量级写入量极低，一把锁足够，省掉 SQLITE_BUSY 重试
}

const schema = `
CREATE TABLE IF NOT EXISTS jobs(
  id      TEXT PRIMARY KEY,
  dev     TEXT NOT NULL DEFAULT '',
  name    TEXT NOT NULL DEFAULT '',
  size    INTEGER NOT NULL DEFAULT 0,
  state   TEXT NOT NULL DEFAULT 'queued',
  bytes   INTEGER NOT NULL DEFAULT 0,
  err     TEXT NOT NULL DEFAULT '',
  serial  TEXT NOT NULL DEFAULT '',
  created INTEGER NOT NULL DEFAULT 0,
  updated INTEGER NOT NULL DEFAULT 0
);
CREATE INDEX IF NOT EXISTS jobs_dev_state ON jobs(dev, serial, state, created);

CREATE TABLE IF NOT EXISTS devices(
  key_id    TEXT PRIMARY KEY,
  dev       TEXT NOT NULL,
  role      TEXT NOT NULL,
  name      TEXT NOT NULL DEFAULT '',
  key_hash  TEXT NOT NULL,
  created   INTEGER NOT NULL DEFAULT 0,
  last_seen INTEGER NOT NULL DEFAULT 0,
  disabled  INTEGER NOT NULL DEFAULT 0
);
CREATE INDEX IF NOT EXISTS devices_dev ON devices(dev);
`

func Open(path string) (*Store, error) {
	db, err := sql.Open("sqlite", path+"?_pragma=busy_timeout(5000)&_pragma=journal_mode(WAL)&_pragma=foreign_keys(1)")
	if err != nil {
		return nil, err
	}
	if _, err := db.Exec(schema); err != nil {
		db.Close()
		return nil, err
	}
	// 旧库升级：Python 版没有这两列。已存在时报错，忽略即可。
	db.Exec(`ALTER TABLE jobs ADD COLUMN err TEXT NOT NULL DEFAULT ''`)
	db.Exec(`ALTER TABLE jobs ADD COLUMN serial TEXT NOT NULL DEFAULT ''`)
	return &Store{db: db}, nil
}

func (s *Store) Close() error { return s.db.Close() }

// RecoverOnBoot 把进程上次退出时悬空的作业退回队列，返回处理条数。
//
// downloading：设备正在取，进程一死没人给它续超时，永远不会被救。
// rendering / no-device：Python 版的历史状态，一并归位。
func (s *Store) RecoverOnBoot() (int, error) {
	s.wmu.Lock()
	defer s.wmu.Unlock()
	r, err := s.db.Exec(
		`UPDATE jobs SET state='queued', bytes=0 WHERE state IN ('downloading','rendering','no-device')`)
	if err != nil {
		return 0, err
	}
	n, err := r.RowsAffected()
	return int(n), err
}
```

- [ ] **Step 5: 运行——此时仍会失败**

Run: `cd server/go && go test ./internal/store/`
Expected: FAIL，`undefined: Job` / `InsertJob` / `GetJob` —— 下一个 Task 补上。
这是刻意的：表结构与作业读写分两次提交，各自可独立评审。

- [ ] **Step 6: 暂不提交**，直接进 Task 4；两者一起提交。

---

## Task 4: store 包 — jobs 表读写

**Files:**
- Create: `server/go/internal/store/job.go`
- Modify: `server/go/internal/store/store_test.go`（追加测试）

- [ ] **Step 1: 追加失败的测试**

在 `server/go/internal/store/store_test.go` 末尾追加：

```go
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
	// PA 的作业必须原样留着，等那台打印机插回来
	if j, ok, _ := s.GetJob("old"); !ok || j.State != StateQueued {
		t.Errorf("旧打印机的作业被动了：state=%q", j.State)
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

func TestNextQueuedEmpty(t *testing.T) {
	s := open(t)
	if _, ok, err := s.NextQueued("nobody", "PA"); ok || err != nil {
		t.Errorf("空队列应返回 ok=false，得到 ok=%v err=%v", ok, err)
	}
}

func TestSetJobStateRecordsError(t *testing.T) {
	s := open(t)
	s.InsertJob(Job{ID: "j", Dev: "d", State: "downloading"})
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
```

- [ ] **Step 2: 运行确认失败**

Run: `cd server/go && go test ./internal/store/`
Expected: FAIL，`undefined: Job`

- [ ] **Step 3: 实现**

`server/go/internal/store/job.go`:

```go
package store

import "time"

type Job struct {
	ID    string
	Dev   string
	Name  string
	Size  int64
	State string
	Bytes int64
	Err   string
	// Serial 是这份光栅为哪台打印机生成的。
	// 派发前必须校验它等于设备当前插着的那台——URF 按特定 dpi 和像素尺寸
	// 光栅，派给另一台就是一沓废纸，而且没有任何环节会发现。
	Serial  string
	Created int64
	Updated int64
}

// 作业状态机：queued → downloading → done / failed
// 没有 rendering——服务端不渲染，上传校验通过即入队。
const (
	StateQueued      = "queued"
	StateDownloading = "downloading"
	StateDone        = "done"
	StateFailed      = "failed"
)

func now() int64 { return time.Now().Unix() }

func (s *Store) InsertJob(j Job) error {
	if j.Created == 0 {
		j.Created = now()
	}
	if j.Updated == 0 {
		j.Updated = j.Created
	}
	s.wmu.Lock()
	defer s.wmu.Unlock()
	_, err := s.db.Exec(
		`INSERT INTO jobs(id,dev,name,size,state,bytes,err,serial,created,updated)
		 VALUES(?,?,?,?,?,?,?,?,?,?)`,
		j.ID, j.Dev, j.Name, j.Size, j.State, j.Bytes, j.Err, j.Serial,
		j.Created, j.Updated)
	return err
}

func (s *Store) SetJobState(id, state string, bytes int64, errMsg string) error {
	s.wmu.Lock()
	defer s.wmu.Unlock()
	_, err := s.db.Exec(
		`UPDATE jobs SET state=?, bytes=?, err=?, updated=? WHERE id=?`,
		state, bytes, errMsg, now(), id)
	return err
}

func (s *Store) SetJobSize(id string, size int64) error {
	s.wmu.Lock()
	defer s.wmu.Unlock()
	_, err := s.db.Exec(`UPDATE jobs SET size=?, updated=? WHERE id=?`, size, now(), id)
	return err
}

// NextQueued 返回该设备上、为当前这台打印机排的最早一件。
//
// serial 过滤是必需的：用户换打印机后，为旧机器光栅的作业绝不能派给新机器。
// 那些作业留在队列里等旧机器插回来，不失败也不删。
//
// 不做「取出并标记」的原子操作：标记由 actor 在派发成功后自己做，
// 而 actor 是该设备唯一的写入者，不存在竞争。
func (s *Store) NextQueued(dev, serial string) (Job, bool, error) {
	row := s.db.QueryRow(
		`SELECT id,dev,name,size,state,bytes,err,serial,created,updated
		 FROM jobs WHERE dev=? AND serial=? AND state=? ORDER BY created LIMIT 1`,
		dev, serial, StateQueued)
	return scanJob(row)
}

// QueuedCountByPrinter 统计该桥上每台打印机各有多少件在排队，
// 给 GET /api/device/{dev}/printers 的 queued_jobs 用。
// 用户看不到这个数就会以为打印失败了。
func (s *Store) QueuedCountByPrinter(dev string) (map[string]int, error) {
	rows, err := s.db.Query(
		`SELECT serial, COUNT(*) FROM jobs WHERE dev=? AND state=? GROUP BY serial`,
		dev, StateQueued)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	out := map[string]int{}
	for rows.Next() {
		var ser string
		var n int
		if err := rows.Scan(&ser, &n); err != nil {
			return nil, err
		}
		out[ser] = n
	}
	return out, rows.Err()
}

func (s *Store) GetJob(id string) (Job, bool, error) {
	row := s.db.QueryRow(
		`SELECT id,dev,name,size,state,bytes,err,serial,created,updated
		 FROM jobs WHERE id=?`, id)
	return scanJob(row)
}

func (s *Store) JobsForDevice(dev string, limit int) ([]Job, error) {
	rows, err := s.db.Query(
		`SELECT id,dev,name,size,state,bytes,err,serial,created,updated
		 FROM jobs WHERE dev=? ORDER BY created DESC LIMIT ?`, dev, limit)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	var out []Job
	for rows.Next() {
		var j Job
		if err := rows.Scan(&j.ID, &j.Dev, &j.Name, &j.Size, &j.State,
			&j.Bytes, &j.Err, &j.Serial, &j.Created, &j.Updated); err != nil {
			return nil, err
		}
		out = append(out, j)
	}
	return out, rows.Err()
}

type scanner interface{ Scan(...any) error }

func scanJob(r scanner) (Job, bool, error) {
	var j Job
	err := r.Scan(&j.ID, &j.Dev, &j.Name, &j.Size, &j.State,
		&j.Bytes, &j.Err, &j.Serial, &j.Created, &j.Updated)
	if err != nil {
		if err.Error() == "sql: no rows in result set" {
			return Job{}, false, nil
		}
		return Job{}, false, err
	}
	return j, true, nil
}
```

- [ ] **Step 4: 运行确认通过**

Run: `cd server/go && go test ./internal/store/ -v`
Expected: PASS，含 `TestRecoverOnBootRequeuesStuckJobs`

- [ ] **Step 5: 提交**

```bash
git add server/go/internal/store server/go/go.mod server/go/go.sum
git commit -m "feat(server): store 表结构与作业读写，启动时恢复悬空作业"
```

---

## Task 5: auth 包 — 密钥生成与 argon2id

**Files:**
- Create: `server/go/internal/auth/secret.go`
- Create: `server/go/internal/auth/secret_test.go`

密钥明文格式 `{key_id}.{secret}`：校验时按 `key_id` 直接定位到唯一一行，
**不遍历该设备的所有密钥逐个 argon2**——那会让开销随密钥数线性增长。

- [ ] **Step 1: 拉依赖**

```bash
cd server/go && go get golang.org/x/crypto/argon2@latest
```

- [ ] **Step 2: 写失败的测试**

`server/go/internal/auth/secret_test.go`:

```go
package auth

import (
	"strings"
	"testing"
)

func TestNewTokenFormat(t *testing.T) {
	keyID, secret, token := NewToken()
	if len(keyID) != 12 {
		t.Errorf("keyID 长度 %d，期望 12", len(keyID))
	}
	if len(secret) < 32 {
		t.Errorf("secret 长度 %d，太短", len(secret))
	}
	if token != keyID+"."+secret {
		t.Errorf("token = %q，期望 keyID.secret", token)
	}
	if _, _, tok2 := NewToken(); tok2 == token {
		t.Error("两次生成的令牌相同——随机源有问题")
	}
}

func TestSplitToken(t *testing.T) {
	id, sec, err := SplitToken("abc123.deadbeef")
	if err != nil || id != "abc123" || sec != "deadbeef" {
		t.Errorf("id=%q sec=%q err=%v", id, sec, err)
	}
	for _, bad := range []string{"", "nodot", ".onlysecret", "onlyid.", "a.b.c"} {
		if _, _, err := SplitToken(bad); err == nil {
			t.Errorf("SplitToken(%q) 应当报错", bad)
		}
	}
}

func TestHashVerifyRoundTrip(t *testing.T) {
	h, err := HashSecret("s3cret-value")
	if err != nil {
		t.Fatal(err)
	}
	if !strings.HasPrefix(h, "$argon2id$") {
		t.Errorf("哈希格式 = %q，期望 argon2id 编码串", h)
	}
	if !VerifySecret("s3cret-value", h) {
		t.Error("正确密钥校验失败")
	}
	if VerifySecret("wrong", h) {
		t.Error("错误密钥竟然通过")
	}
	if VerifySecret("s3cret-value", "$argon2id$garbage") {
		t.Error("畸形哈希串竟然通过")
	}
}

func TestHashSaltsAreDistinct(t *testing.T) {
	a, _ := HashSecret("same")
	b, _ := HashSecret("same")
	if a == b {
		t.Error("同一密钥两次哈希结果相同——盐没随机")
	}
}
```

- [ ] **Step 3: 运行确认失败**

Run: `cd server/go && go test ./internal/auth/`
Expected: FAIL，`undefined: NewToken`

- [ ] **Step 4: 实现**

`server/go/internal/auth/secret.go`:

```go
// Package auth 管设备密钥的生成、校验与 ACL 判定。
package auth

import (
	"crypto/rand"
	"crypto/subtle"
	"encoding/base64"
	"encoding/hex"
	"errors"
	"fmt"
	"strings"

	"golang.org/x/crypto/argon2"
)

// argon2id 参数。刻意选得慢——这是防爆破的全部意义。
// 热路径不受影响：校验结果有缓存，见 verifier.go。
const (
	argonTime    = 1
	argonMemory  = 64 * 1024 // 64 MB
	argonThreads = 4
	argonKeyLen  = 32
	saltLen      = 16
)

var ErrBadToken = errors.New("auth: 令牌格式非法")

// NewToken 生成一把新密钥，返回 (keyID, secret, 完整令牌)。
// 明文只在签发时出现一次，不落盘。
func NewToken() (keyID, secret, token string) {
	idb := make([]byte, 6)
	sb := make([]byte, 24)
	if _, err := rand.Read(idb); err != nil {
		panic(err) // crypto/rand 失败时继续运行没有意义
	}
	if _, err := rand.Read(sb); err != nil {
		panic(err)
	}
	keyID = hex.EncodeToString(idb)                                  // 12 字符
	secret = base64.RawURLEncoding.EncodeToString(sb)                // 32 字符
	return keyID, secret, keyID + "." + secret
}

func SplitToken(token string) (keyID, secret string, err error) {
	i := strings.IndexByte(token, '.')
	if i <= 0 || i == len(token)-1 {
		return "", "", ErrBadToken
	}
	keyID, secret = token[:i], token[i+1:]
	if strings.ContainsRune(secret, '.') {
		return "", "", ErrBadToken
	}
	return keyID, secret, nil
}

func HashSecret(secret string) (string, error) {
	salt := make([]byte, saltLen)
	if _, err := rand.Read(salt); err != nil {
		return "", err
	}
	sum := argon2.IDKey([]byte(secret), salt, argonTime, argonMemory, argonThreads, argonKeyLen)
	return fmt.Sprintf("$argon2id$v=%d$m=%d,t=%d,p=%d$%s$%s",
		argon2.Version, argonMemory, argonTime, argonThreads,
		base64.RawStdEncoding.EncodeToString(salt),
		base64.RawStdEncoding.EncodeToString(sum)), nil
}

func VerifySecret(secret, encoded string) bool {
	parts := strings.Split(encoded, "$")
	// ["", "argon2id", "v=19", "m=..,t=..,p=..", salt, hash]
	if len(parts) != 6 || parts[1] != "argon2id" {
		return false
	}
	var version int
	if _, err := fmt.Sscanf(parts[2], "v=%d", &version); err != nil || version != argon2.Version {
		return false
	}
	var m uint32
	var tt, p int
	if _, err := fmt.Sscanf(parts[3], "m=%d,t=%d,p=%d", &m, &tt, &p); err != nil {
		return false
	}
	salt, err := base64.RawStdEncoding.DecodeString(parts[4])
	if err != nil {
		return false
	}
	want, err := base64.RawStdEncoding.DecodeString(parts[5])
	if err != nil {
		return false
	}
	got := argon2.IDKey([]byte(secret), salt, uint32(tt), m, uint8(p), uint32(len(want)))
	return subtle.ConstantTimeCompare(got, want) == 1
}
```

- [ ] **Step 5: 运行确认通过**

Run: `cd server/go && go test ./internal/auth/ -v`
Expected: PASS（5 个用例）

- [ ] **Step 6: 提交**

```bash
git add server/go/internal/auth server/go/go.mod server/go/go.sum
git commit -m "feat(server): 设备密钥生成与 argon2id 校验"
```

---

## Task 6: auth 包 — 校验缓存与 ACL

**Files:**
- Create: `server/go/internal/auth/verifier.go`
- Create: `server/go/internal/auth/acl.go`
- Create: `server/go/internal/auth/verifier_test.go`
- Create: `server/go/internal/auth/acl_test.go`

- [ ] **Step 1: 写失败的测试（缓存）**

`server/go/internal/auth/verifier_test.go`:

```go
package auth

import (
	"testing"
	"time"

	"github.com/dayuer/stickbox/server/go/internal/store"
)

type fakeKeys struct {
	keys  map[string]store.Key
	reads int
}

func (f *fakeKeys) KeyByID(id string) (store.Key, bool, error) {
	f.reads++
	k, ok := f.keys[id]
	return k, ok, nil
}

func newFixture(t *testing.T) (*Verifier, *fakeKeys, string) {
	t.Helper()
	keyID, secret, token := NewToken()
	h, err := HashSecret(secret)
	if err != nil {
		t.Fatal(err)
	}
	f := &fakeKeys{keys: map[string]store.Key{
		keyID: {KeyID: keyID, Dev: "dev1", Role: string(RoleDevice), Hash: h},
	}}
	return NewVerifier(f), f, token
}

func TestVerifyReturnsIdentity(t *testing.T) {
	v, _, token := newFixture(t)
	id, err := v.Verify(token)
	if err != nil {
		t.Fatal(err)
	}
	if id.Dev != "dev1" || id.Role != RoleDevice {
		t.Errorf("身份 = %+v", id)
	}
}

// argon2 单次几十毫秒，取件路径每次都算会毁掉低延迟目标。
func TestVerifyUsesCacheOnSecondCall(t *testing.T) {
	v, f, token := newFixture(t)
	if _, err := v.Verify(token); err != nil {
		t.Fatal(err)
	}
	before := f.reads
	start := time.Now()
	if _, err := v.Verify(token); err != nil {
		t.Fatal(err)
	}
	if f.reads != before {
		t.Errorf("第二次校验又查了库（reads %d → %d），缓存没生效", before, f.reads)
	}
	if d := time.Since(start); d > 5*time.Millisecond {
		t.Errorf("缓存命中耗时 %v，说明仍在跑 argon2", d)
	}
}

func TestVerifyRejects(t *testing.T) {
	v, f, token := newFixture(t)
	keyID, _, _ := SplitToken(token)

	if _, err := v.Verify(keyID + ".wrongsecret"); err == nil {
		t.Error("错误密钥应被拒")
	}
	if _, err := v.Verify("nonexistent.secret"); err == nil {
		t.Error("不存在的 key_id 应被拒")
	}
	if _, err := v.Verify("malformed"); err == nil {
		t.Error("畸形令牌应被拒")
	}

	k := f.keys[keyID]
	k.Disabled = true
	f.keys[keyID] = k
	v.Invalidate(keyID)
	if _, err := v.Verify(token); err == nil {
		t.Error("已吊销的密钥应被拒")
	}
}

func TestInvalidateDropsCache(t *testing.T) {
	v, f, token := newFixture(t)
	v.Verify(token)
	v.Invalidate(mustKeyID(t, token))
	before := f.reads
	v.Verify(token)
	if f.reads == before {
		t.Error("Invalidate 后应重新查库")
	}
}

func mustKeyID(t *testing.T, token string) string {
	t.Helper()
	id, _, err := SplitToken(token)
	if err != nil {
		t.Fatal(err)
	}
	return id
}
```

- [ ] **Step 2: 写失败的测试（ACL）**

`server/go/internal/auth/acl_test.go`:

```go
package auth

import "testing"

// 这张矩阵是安全边界，改动必须先改这里。
func TestACLMatrix(t *testing.T) {
	dev := Identity{Dev: "aaa", Role: RoleDevice}
	app := Identity{Dev: "aaa", Role: RoleApp}

	cases := []struct {
		name  string
		id    Identity
		topic string
		write bool
		want  bool
	}{
		{"设备发状态", dev, "printer/aaa/status", true, true},
		{"设备发档案", dev, "printer/aaa/ident", true, true},
		{"设备收作业", dev, "printer/aaa/job", false, true},
		{"设备收命令", dev, "printer/aaa/cmd", false, true},
		{"设备不能自派作业", dev, "printer/aaa/job", true, false},
		{"设备不能碰别人", dev, "printer/bbb/status", true, false},
		{"设备不能订阅别人", dev, "printer/bbb/status", false, false},
		{"设备不能用通配符", dev, "printer/+/status", false, false},

		{"App 订阅状态", app, "printer/aaa/status", false, true},
		{"App 不能伪造状态", app, "printer/aaa/status", true, false},
		{"App 不能派作业", app, "printer/aaa/job", true, false},
		{"App 不能收作业内容", app, "printer/aaa/job", false, false},
		{"App 不能碰别人", app, "printer/bbb/status", false, false},

		{"越界前缀", dev, "printer/aaa", false, false},
		{"多余层级", dev, "printer/aaa/status/x", false, false},
		{"完全无关的 topic", dev, "$SYS/broker/uptime", false, false},
	}
	for _, c := range cases {
		if got := ACLAllowed(c.id, c.topic, c.write); got != c.want {
			t.Errorf("%s: ACLAllowed(%+v, %q, write=%v) = %v，期望 %v",
				c.name, c.id, c.topic, c.write, got, c.want)
		}
	}
}
```

- [ ] **Step 3: 运行确认失败**

Run: `cd server/go && go test ./internal/auth/`
Expected: FAIL，`undefined: NewVerifier` / `undefined: ACLAllowed`

- [ ] **Step 4: 实现 verifier**

`server/go/internal/auth/verifier.go`:

```go
package auth

import (
	"crypto/sha256"
	"crypto/subtle"
	"errors"
	"sync"
	"time"

	"github.com/dayuer/stickbox/server/go/internal/store"
)

type Role string

const (
	RoleDevice Role = "device"
	RoleApp    Role = "app"
)

type Identity struct {
	Dev   string
	Role  Role
	KeyID string
}

var ErrDenied = errors.New("auth: 拒绝")

// KeyStore 是 auth 对 store 的窄依赖，测试里用假实现替换。
type KeyStore interface {
	KeyByID(keyID string) (store.Key, bool, error)
}

const cacheTTL = time.Hour

type cacheEntry struct {
	sum [32]byte // sha256(secret)，缓存命中时只做常数时间比较
	id  Identity
	exp time.Time
}

type Verifier struct {
	keys KeyStore
	mu   sync.RWMutex
	c    map[string]cacheEntry
	now  func() time.Time
}

func NewVerifier(keys KeyStore) *Verifier {
	return &Verifier{keys: keys, c: map[string]cacheEntry{}, now: time.Now}
}

// Verify 校验 "{key_id}.{secret}" 形式的令牌。
//
// 首次走 argon2id（几十毫秒，故意的）；之后缓存 sha256，
// 让取件这种高频路径退化成一次哈希比较。
func (v *Verifier) Verify(token string) (Identity, error) {
	keyID, secret, err := SplitToken(token)
	if err != nil {
		return Identity{}, ErrDenied
	}
	sum := sha256.Sum256([]byte(secret))

	v.mu.RLock()
	e, ok := v.c[keyID]
	v.mu.RUnlock()
	if ok && v.now().Before(e.exp) {
		if subtle.ConstantTimeCompare(sum[:], e.sum[:]) == 1 {
			return e.id, nil
		}
		return Identity{}, ErrDenied
	}

	k, found, err := v.keys.KeyByID(keyID)
	if err != nil {
		return Identity{}, err
	}
	if !found || k.Disabled || !VerifySecret(secret, k.Hash) {
		return Identity{}, ErrDenied
	}
	id := Identity{Dev: k.Dev, Role: Role(k.Role), KeyID: k.KeyID}

	v.mu.Lock()
	v.c[keyID] = cacheEntry{sum: sum, id: id, exp: v.now().Add(cacheTTL)}
	v.mu.Unlock()
	return id, nil
}

// Invalidate 在吊销密钥后立即调用，否则缓存会让它再活最多一小时。
func (v *Verifier) Invalidate(keyID string) {
	v.mu.Lock()
	delete(v.c, keyID)
	v.mu.Unlock()
}
```

- [ ] **Step 5: 实现 ACL**

`server/go/internal/auth/acl.go`:

```go
package auth

import "strings"

// ACLAllowed 判定某身份能否读/写某 topic。
//
// topic 一律是 printer/{dev}/{leaf} 三段。不做通配符展开——
// 设备和 App 都不需要跨设备订阅，把口子焊死比事后审计便宜。
func ACLAllowed(id Identity, topic string, write bool) bool {
	parts := strings.Split(topic, "/")
	if len(parts) != 3 || parts[0] != "printer" || parts[1] != id.Dev {
		return false
	}
	leaf := parts[2]
	switch id.Role {
	case RoleDevice:
		if write {
			return leaf == "status" || leaf == "ident"
		}
		return leaf == "job" || leaf == "cmd"
	case RoleApp:
		// App 只旁听。能写 status 就能伪造打印机状态，能读 job 就能截胡作业信令。
		return !write && leaf == "status"
	}
	return false
}
```

- [ ] **Step 6: 补 store 的 Key 类型让测试能编译**

`server/go/internal/store/key.go`:

```go
package store

type Key struct {
	KeyID    string
	Dev      string
	Role     string
	Name     string
	Hash     string
	Created  int64
	LastSeen int64
	Disabled bool
}

func (s *Store) InsertKey(k Key) error {
	if k.Created == 0 {
		k.Created = now()
	}
	s.wmu.Lock()
	defer s.wmu.Unlock()
	_, err := s.db.Exec(
		`INSERT INTO devices(key_id,dev,role,name,key_hash,created,last_seen,disabled)
		 VALUES(?,?,?,?,?,?,0,0)`,
		k.KeyID, k.Dev, k.Role, k.Name, k.Hash, k.Created)
	return err
}

func (s *Store) KeyByID(keyID string) (Key, bool, error) {
	var k Key
	var disabled int
	err := s.db.QueryRow(
		`SELECT key_id,dev,role,name,key_hash,created,last_seen,disabled
		 FROM devices WHERE key_id=?`, keyID).
		Scan(&k.KeyID, &k.Dev, &k.Role, &k.Name, &k.Hash, &k.Created, &k.LastSeen, &disabled)
	if err != nil {
		if err.Error() == "sql: no rows in result set" {
			return Key{}, false, nil
		}
		return Key{}, false, err
	}
	k.Disabled = disabled != 0
	return k, true, nil
}

func (s *Store) ListKeys() ([]Key, error) {
	rows, err := s.db.Query(
		`SELECT key_id,dev,role,name,key_hash,created,last_seen,disabled
		 FROM devices ORDER BY dev, created`)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	var out []Key
	for rows.Next() {
		var k Key
		var disabled int
		if err := rows.Scan(&k.KeyID, &k.Dev, &k.Role, &k.Name, &k.Hash,
			&k.Created, &k.LastSeen, &disabled); err != nil {
			return nil, err
		}
		k.Disabled = disabled != 0
		out = append(out, k)
	}
	return out, rows.Err()
}

func (s *Store) DisableKey(keyID string) error {
	s.wmu.Lock()
	defer s.wmu.Unlock()
	_, err := s.db.Exec(`UPDATE devices SET disabled=1 WHERE key_id=?`, keyID)
	return err
}

func (s *Store) TouchKey(keyID string) error {
	s.wmu.Lock()
	defer s.wmu.Unlock()
	_, err := s.db.Exec(`UPDATE devices SET last_seen=? WHERE key_id=?`, now(), keyID)
	return err
}
```

- [ ] **Step 7: 运行确认通过**

Run: `cd server/go && go test ./internal/auth/ ./internal/store/ -v`
Expected: PASS，ACL 矩阵 17 条全绿

- [ ] **Step 8: 提交**

```bash
git add server/go/internal/auth server/go/internal/store/key.go
git commit -m "feat(server): 校验缓存与角色 ACL——App 密钥不能伪造打印机状态"
```

---

## Task 7: device 包 — 消息类型与 actor 骨架

**Files:**
- Create: `server/go/internal/device/msg.go`
- Create: `server/go/internal/device/actor.go`
- Create: `server/go/internal/device/actor_test.go`

这是本项目的核心。`device` 包**不 import** HTTP、MQTT 的任何东西，
只依赖两个窄接口。评审时按这条检查。

- [ ] **Step 1: 定义消息类型**

`server/go/internal/device/msg.go`:

```go
package device

type Kind int

const (
	KindHeartbeat   Kind = iota // 设备心跳（含打印机面板状态）
	KindJobDone                 // 作业完成回执
	KindJobFailed               // 作业失败回执
	KindDownloading             // 设备开始取件，用于续超时
	KindWake                    // 有新作业入队 / 巡检唤醒
	KindTick                    // 时间推进，测试里由外部驱动
)

type Msg struct {
	Kind  Kind
	JobID string
	Bytes int64
	Err   string
	// Serial 是心跳里上报的、当前插着的打印机序列号。拔掉时是空串。
	// 用户换打印机是常态，actor 靠它决定派哪些作业。
	Serial string
	// Printer 是设备上报的打印机面板状态原文，原样存下给 /api/status 用。
	Printer []byte
}
```

- [ ] **Step 2: 写失败的测试**

`server/go/internal/device/actor_test.go`:

```go
package device

import (
	"testing"
	"time"

	"github.com/dayuer/stickbox/server/go/internal/store"
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

var fakeNow = time.Unix(1_000_000, 0)

func TestDispatchesOneJobOnWake(t *testing.T) {
	a, st, pub, _ := newActor(t)
	st.InsertJob(store.Job{ID: "j1", Dev: "d1", Serial: "PA", Size: 4096, State: store.StateQueued, Created: 1})
	st.InsertJob(store.Job{ID: "j2", Dev: "d1", Serial: "PA", Size: 8192, State: store.StateQueued, Created: 2})

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
	st.InsertJob(store.Job{ID: "j1", Dev: "d1", Serial: "PA", State: store.StateQueued, Created: 1})
	st.InsertJob(store.Job{ID: "j2", Dev: "d1", Serial: "PA", State: store.StateQueued, Created: 2})

	a.handle(Msg{Kind: KindWake})
	a.handle(Msg{Kind: KindWake})
	a.handle(Msg{Kind: KindHeartbeat})

	if len(pub.sent) != 1 {
		t.Fatalf("重复派发 %d 次——inflight 判断失效", len(pub.sent))
	}
}

func TestDoneTriggersNextJob(t *testing.T) {
	a, st, pub, _ := newActor(t)
	st.InsertJob(store.Job{ID: "j1", Dev: "d1", Serial: "PA", State: store.StateQueued, Created: 1})
	st.InsertJob(store.Job{ID: "j2", Dev: "d1", Serial: "PA", State: store.StateQueued, Created: 2})

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
	st.InsertJob(store.Job{ID: "j1", Dev: "d1", Serial: "PA", State: store.StateQueued, Created: 1})
	st.InsertJob(store.Job{ID: "j2", Dev: "d1", Serial: "PA", State: store.StateQueued, Created: 2})

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
	st.InsertJob(store.Job{ID: "j1", Dev: "d1", Serial: "PA", State: store.StateQueued, Created: 1})

	a.handle(Msg{Kind: KindWake})
	a.now = func() time.Time { return fakeNow.Add(181 * time.Second) }
	a.handle(Msg{Kind: KindTick})

	if j, _, _ := st.GetJob("j1"); j.State != store.StateQueued {
		t.Errorf("超时后 j1 state=%q，期望退回 queued", j.State)
	}
	if len(pub.sent) != 2 {
		t.Errorf("退回后应立即重派，sent=%d", len(pub.sent))
	}
}

// 设备正在取件时不能被超时误杀。
func TestDownloadingExtendsDeadline(t *testing.T) {
	a, st, pub, _ := newActor(t)
	st.InsertJob(store.Job{ID: "j1", Dev: "d1", Serial: "PA", State: store.StateQueued, Created: 1})

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
	st.InsertJob(store.Job{ID: "a1", Dev: "d1", Serial: "PA", State: store.StateQueued, Created: 1})
	st.InsertJob(store.Job{ID: "b1", Dev: "d1", Serial: "PB", State: store.StateQueued, Created: 2})

	a.handle(Msg{Kind: KindWake})
	if len(pub.sent) != 1 || pub.sent[0].job != "a1" {
		t.Fatalf("首次应派 PA 的作业，sent=%+v", pub.sent)
	}

	// 用户拔掉 PA 换上 PB
	a.handle(Msg{Kind: KindHeartbeat, Serial: "PB"})

	if j, _, _ := st.GetJob("a1"); j.State != store.StateQueued {
		t.Errorf("换机后正在传的 a1 应退回队列，实际 state=%q", j.State)
	}
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
	st.InsertJob(store.Job{ID: "a1", Dev: "d1", Serial: "PA", State: store.StateQueued, Created: 1})
	a.handle(Msg{Kind: KindHeartbeat, Serial: ""})
	a.handle(Msg{Kind: KindWake})
	if len(pub.sent) != 0 {
		t.Errorf("没插打印机不该派活，sent=%d", len(pub.sent))
	}
}

// 队列的真相在 sqlite：actor 退出后作业必须原样还在。
func TestIdleExitLeavesQueueIntact(t *testing.T) {
	a, st, _, _ := newActor(t)
	st.InsertJob(store.Job{ID: "j1", Dev: "d1", Serial: "PA", State: store.StateQueued, Created: 1})
	a.handle(Msg{Kind: KindWake})

	a.now = func() time.Time { return fakeNow.Add(10 * time.Minute) }
	if !a.idleExpired() {
		t.Fatal("超过 IdleTimeout 未判定为离线")
	}
	a.shutdown()

	if j, _, _ := st.GetJob("j1"); j.State != store.StateQueued {
		t.Errorf("actor 退出后 j1 state=%q，作业丢了", j.State)
	}
}
```

- [ ] **Step 3: 运行确认失败**

Run: `cd server/go && go test ./internal/device/`
Expected: FAIL，`undefined: New` / `undefined: Actor`

- [ ] **Step 4: 实现 actor**

`server/go/internal/device/actor.go`:

```go
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
	IdleTimeout time.Duration // 多久没心跳就退出，默认 5min
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
}

func New(dev string, st Store, pub Publisher, o Options) *Actor {
	if o.Now == nil {
		o.Now = time.Now
	}
	if o.JobTimeout == 0 {
		o.JobTimeout = 180 * time.Second
	}
	if o.IdleTimeout == 0 {
		o.IdleTimeout = 5 * time.Minute
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
	}
}

func (a *Actor) Dev() string          { return a.dev }
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

// Printer 返回最近一次心跳里的打印机面板状态原文，给 /api/status 用。
func (a *Actor) Printer() []byte { return a.printer }

// Serial 返回当前插着的打印机序列号，给 /api/device/{dev}/printers 用。
func (a *Actor) Serial() string { return a.serial }
```

- [ ] **Step 5: 运行确认通过**

Run: `cd server/go && go test ./internal/device/ -v`
Expected: PASS，8 个用例

- [ ] **Step 6: 竞态检查**

Run: `cd server/go && go test -race ./internal/device/`
Expected: PASS，无 race 报告

- [ ] **Step 7: 提交**

```bash
git add server/go/internal/device
git commit -m "feat(server): 设备 actor——一次只派一件，无锁串行状态机"
```

---

## Task 8: store — 用户、手机号、验证码三张表

**Files:**
- Create: `server/go/internal/store/user.go`
- Create: `server/go/internal/store/user_test.go`
- Modify: `server/go/internal/store/store.go`（schema 追加）
- Modify: `server/go/internal/store/key.go`（`Key` 加 `UserID`）

手机号有三种形态，各管各的用途，**不要混**：`phone_hmac` 登录查询、
`phone_tail` 界面展示、`phone_enc` 客服核验与导出。完整号码**单独一张表**——
登录和 ACL 是高频路径，个人信息不进热表。

- [ ] **Step 1: 追加 schema**

在 `server/go/internal/store/store.go` 的 `schema` 常量末尾追加：

```sql
CREATE TABLE IF NOT EXISTS users(
  id         TEXT PRIMARY KEY,
  phone_hmac TEXT NOT NULL UNIQUE,
  phone_tail TEXT NOT NULL DEFAULT '',
  created    INTEGER NOT NULL DEFAULT 0,
  last_login INTEGER NOT NULL DEFAULT 0,
  disabled   INTEGER NOT NULL DEFAULT 0
);

-- 完整号码单独成表，热路径永远不碰它
CREATE TABLE IF NOT EXISTS user_phones(
  user_id   TEXT PRIMARY KEY,
  phone_enc BLOB NOT NULL,
  updated   INTEGER NOT NULL DEFAULT 0
);

CREATE TABLE IF NOT EXISTS sms_codes(
  phone_hmac TEXT PRIMARY KEY,
  code_hash  TEXT NOT NULL,
  expires    INTEGER NOT NULL DEFAULT 0,
  attempts   INTEGER NOT NULL DEFAULT 0,
  sent_at    INTEGER NOT NULL DEFAULT 0,
  day_count  INTEGER NOT NULL DEFAULT 0,
  day_start  INTEGER NOT NULL DEFAULT 0
);
```

同时给 `devices` 表加一列（旧库升级用 `ALTER`，已存在时报错忽略）：

```go
	db.Exec(`ALTER TABLE devices ADD COLUMN user_id TEXT NOT NULL DEFAULT ''`)
```

- [ ] **Step 2: 写失败的测试**

`server/go/internal/store/user_test.go`:

```go
package store

import "testing"

func TestUpsertUserIsIdempotent(t *testing.T) {
	s := open(t)
	u1, created, err := s.UpsertUser("HMAC-A", "8888")
	if err != nil || !created {
		t.Fatalf("首次应创建：created=%v err=%v", created, err)
	}
	u2, created, err := s.UpsertUser("HMAC-A", "8888")
	if err != nil || created {
		t.Fatalf("第二次不该再创建：created=%v err=%v", created, err)
	}
	if u1.ID != u2.ID {
		t.Errorf("同一号码拿到了两个 user id：%s vs %s", u1.ID, u2.ID)
	}
	if u2.LastLogin == 0 {
		t.Error("last_login 未刷新")
	}
}

func TestUserByHMACNotFound(t *testing.T) {
	s := open(t)
	if _, ok, err := s.UserByHMAC("nope"); ok || err != nil {
		t.Errorf("ok=%v err=%v，期望 ok=false", ok, err)
	}
}

// 完整号码必须在单独的表里，users 表本身不含它。
func TestPhoneStoredSeparately(t *testing.T) {
	s := open(t)
	u, _, _ := s.UpsertUser("HMAC-A", "8888")
	if err := s.PutPhone(u.ID, []byte("ciphertext-bytes")); err != nil {
		t.Fatal(err)
	}
	enc, ok, err := s.GetPhone(u.ID)
	if err != nil || !ok || string(enc) != "ciphertext-bytes" {
		t.Fatalf("取回 %q ok=%v err=%v", enc, ok, err)
	}
	// users 表里不该出现密文
	var n int
	s.db.QueryRow(`SELECT COUNT(*) FROM pragma_table_info('users') WHERE name='phone_enc'`).Scan(&n)
	if n != 0 {
		t.Error("users 表里出现了 phone_enc 列——个人信息进了热表")
	}
}

// 注销账号必须把三张表都清干净，但不碰按序列号存的适配档案。
func TestDeleteUserRemovesEverything(t *testing.T) {
	s := open(t)
	u, _, _ := s.UpsertUser("HMAC-A", "8888")
	s.PutPhone(u.ID, []byte("x"))
	s.InsertKey(Key{KeyID: "k1", Dev: "d1", Role: "app", UserID: u.ID, Hash: "h"})
	s.InsertJob(Job{ID: "j1", Dev: "d1", Serial: "PA", State: StateQueued})

	files, err := s.DeleteUser(u.ID)
	if err != nil {
		t.Fatal(err)
	}
	if len(files) != 1 || files[0] != "j1" {
		t.Errorf("返回待删文件 %v，期望 [j1]", files)
	}
	if _, ok, _ := s.UserByHMAC("HMAC-A"); ok {
		t.Error("users 行还在")
	}
	if _, ok, _ := s.GetPhone(u.ID); ok {
		t.Error("user_phones 行还在")
	}
	if _, ok, _ := s.KeyByID("k1"); ok {
		t.Error("session 密钥还在")
	}
	if _, ok, _ := s.GetJob("j1"); ok {
		t.Error("作业记录还在")
	}
}

func TestDevicesOfUser(t *testing.T) {
	s := open(t)
	s.InsertKey(Key{KeyID: "k1", Dev: "d1", Role: "device", UserID: "u1", Hash: "h"})
	s.InsertKey(Key{KeyID: "k2", Dev: "d2", Role: "device", UserID: "u1", Hash: "h"})
	s.InsertKey(Key{KeyID: "k3", Dev: "d3", Role: "device", UserID: "u2", Hash: "h"})
	s.InsertKey(Key{KeyID: "k4", Dev: "d1", Role: "device", UserID: "u1", Hash: "h"})

	devs, err := s.DevicesOfUser("u1")
	if err != nil {
		t.Fatal(err)
	}
	if len(devs) != 2 {
		t.Fatalf("得到 %v，期望去重后的 [d1 d2]", devs)
	}
}

// 抢绑防护的基础：查一台设备当前属于谁。
func TestOwnerOfDevice(t *testing.T) {
	s := open(t)
	s.InsertKey(Key{KeyID: "k1", Dev: "d1", Role: "device", UserID: "u1", Hash: "h"})
	if owner, ok, _ := s.OwnerOfDevice("d1"); !ok || owner != "u1" {
		t.Errorf("owner=%q ok=%v，期望 u1", owner, ok)
	}
	if _, ok, _ := s.OwnerOfDevice("unbound"); ok {
		t.Error("未绑定的设备不该有 owner")
	}
}
```

- [ ] **Step 3: 运行确认失败**

Run: `cd server/go && go test ./internal/store/`
Expected: FAIL，`undefined: UpsertUser`

- [ ] **Step 4: 实现**

`server/go/internal/store/user.go`:

```go
package store

import "crypto/rand"

type User struct {
	ID        string
	PhoneHMAC string
	PhoneTail string
	Created   int64
	LastLogin int64
	Disabled  bool
}

func newID() string {
	b := make([]byte, 16)
	if _, err := rand.Read(b); err != nil {
		panic(err) // crypto/rand 失败时继续运行没有意义
	}
	const hex = "0123456789abcdef"
	out := make([]byte, 32)
	for i, v := range b {
		out[i*2], out[i*2+1] = hex[v>>4], hex[v&0x0f]
	}
	return string(out)
}

// UpsertUser 按手机号 HMAC 找用户，没有就创建。created 表示是否新建。
// 登录路径只碰 HMAC，全程不解密完整号码。
func (s *Store) UpsertUser(phoneHMAC, tail string) (User, bool, error) {
	if u, ok, err := s.UserByHMAC(phoneHMAC); err != nil {
		return User{}, false, err
	} else if ok {
		s.wmu.Lock()
		_, err = s.db.Exec(`UPDATE users SET last_login=? WHERE id=?`, now(), u.ID)
		s.wmu.Unlock()
		u.LastLogin = now()
		return u, false, err
	}
	u := User{ID: newID(), PhoneHMAC: phoneHMAC, PhoneTail: tail,
		Created: now(), LastLogin: now()}
	s.wmu.Lock()
	_, err := s.db.Exec(
		`INSERT INTO users(id,phone_hmac,phone_tail,created,last_login,disabled)
		 VALUES(?,?,?,?,?,0)`, u.ID, u.PhoneHMAC, u.PhoneTail, u.Created, u.LastLogin)
	s.wmu.Unlock()
	return u, true, err
}

func (s *Store) UserByHMAC(phoneHMAC string) (User, bool, error) {
	var u User
	var disabled int
	err := s.db.QueryRow(
		`SELECT id,phone_hmac,phone_tail,created,last_login,disabled
		 FROM users WHERE phone_hmac=?`, phoneHMAC).
		Scan(&u.ID, &u.PhoneHMAC, &u.PhoneTail, &u.Created, &u.LastLogin, &disabled)
	if err != nil {
		if err.Error() == "sql: no rows in result set" {
			return User{}, false, nil
		}
		return User{}, false, err
	}
	u.Disabled = disabled != 0
	return u, true, nil
}

// PutPhone / GetPhone 存取加密后的完整号码。调用方负责加解密，
// store 只当它是一段不透明字节——这样密钥永远不进 store 包。
func (s *Store) PutPhone(userID string, enc []byte) error {
	s.wmu.Lock()
	defer s.wmu.Unlock()
	_, err := s.db.Exec(
		`INSERT INTO user_phones(user_id,phone_enc,updated) VALUES(?,?,?)
		 ON CONFLICT(user_id) DO UPDATE SET phone_enc=excluded.phone_enc,
		                                    updated=excluded.updated`,
		userID, enc, now())
	return err
}

func (s *Store) GetPhone(userID string) ([]byte, bool, error) {
	var enc []byte
	err := s.db.QueryRow(`SELECT phone_enc FROM user_phones WHERE user_id=?`, userID).Scan(&enc)
	if err != nil {
		if err.Error() == "sql: no rows in result set" {
			return nil, false, nil
		}
		return nil, false, err
	}
	return enc, true, nil
}

// DeleteUser 执行注销：清空该用户的一切个人数据，返回需要删除的作业文件 id。
//
// 刻意不碰 printers / profiles / test_runs——那些按打印机序列号存，
// 绑的是硬件不是人，里面没有能关联到自然人的字段。
func (s *Store) DeleteUser(userID string) ([]string, error) {
	devs, err := s.DevicesOfUser(userID)
	if err != nil {
		return nil, err
	}
	var jobIDs []string
	for _, d := range devs {
		rows, err := s.db.Query(`SELECT id FROM jobs WHERE dev=?`, d)
		if err != nil {
			return nil, err
		}
		for rows.Next() {
			var id string
			if err := rows.Scan(&id); err != nil {
				rows.Close()
				return nil, err
			}
			jobIDs = append(jobIDs, id)
		}
		rows.Close()
	}

	s.wmu.Lock()
	defer s.wmu.Unlock()
	tx, err := s.db.Begin()
	if err != nil {
		return nil, err
	}
	defer tx.Rollback()
	for _, d := range devs {
		if _, err := tx.Exec(`DELETE FROM jobs WHERE dev=?`, d); err != nil {
			return nil, err
		}
	}
	if _, err := tx.Exec(`DELETE FROM devices WHERE user_id=?`, userID); err != nil {
		return nil, err
	}
	if _, err := tx.Exec(`DELETE FROM user_phones WHERE user_id=?`, userID); err != nil {
		return nil, err
	}
	if _, err := tx.Exec(`DELETE FROM users WHERE id=?`, userID); err != nil {
		return nil, err
	}
	return jobIDs, tx.Commit()
}

// DevicesOfUser 返回该用户名下所有 dev，去重。ACL 判定要用它。
func (s *Store) DevicesOfUser(userID string) ([]string, error) {
	rows, err := s.db.Query(
		`SELECT DISTINCT dev FROM devices WHERE user_id=? AND disabled=0`, userID)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	var out []string
	for rows.Next() {
		var d string
		if err := rows.Scan(&d); err != nil {
			return nil, err
		}
		out = append(out, d)
	}
	return out, rows.Err()
}

// OwnerOfDevice 是抢绑防护的基础：这台设备当前属于谁。
func (s *Store) OwnerOfDevice(dev string) (string, bool, error) {
	var userID string
	err := s.db.QueryRow(
		`SELECT user_id FROM devices WHERE dev=? AND role='device' AND disabled=0
		 AND user_id<>'' LIMIT 1`, dev).Scan(&userID)
	if err != nil {
		if err.Error() == "sql: no rows in result set" {
			return "", false, nil
		}
		return "", false, err
	}
	return userID, true, nil
}
```

- [ ] **Step 5: `Key` 结构加 `UserID`**

`server/go/internal/store/key.go` 的 `Key` 加一个字段，并改三处 SQL：

```go
type Key struct {
	KeyID    string
	Dev      string
	Role     string
	UserID   string
	Name     string
	Hash     string
	Created  int64
	LastSeen int64
	Disabled bool
}
```

`InsertKey` 的 SQL 改为：

```go
	_, err := s.db.Exec(
		`INSERT INTO devices(key_id,dev,role,user_id,name,key_hash,created,last_seen,disabled)
		 VALUES(?,?,?,?,?,?,?,0,0)`,
		k.KeyID, k.Dev, k.Role, k.UserID, k.Name, k.Hash, k.Created)
```

`KeyByID` 和 `ListKeys` 的 SELECT 列表同样加 `user_id`，`Scan` 里加 `&k.UserID`
（位置与列顺序一致）。

- [ ] **Step 6: 运行确认通过**

Run: `cd server/go && go test ./internal/store/ -v`
Expected: PASS

- [ ] **Step 7: 提交**

```bash
git add server/go/internal/store
git commit -m "feat(server): 用户、手机号、验证码三张表；完整号码单独成表"
```

---

## Task 9: auth — 手机号处理与验证码防刷

**Files:**
- Create: `server/go/internal/auth/phone.go`
- Create: `server/go/internal/auth/sms.go`
- Create: `server/go/internal/auth/phone_test.go`
- Create: `server/go/internal/auth/sms_test.go`

**短信每条都是真金白银，被刷等于烧钱加骚扰他人。** 四道闸一个都不能少。

- [ ] **Step 1: 写失败的测试（手机号）**

`server/go/internal/auth/phone_test.go`:

```go
package auth

import "testing"

func newPhoneBox(t *testing.T) *PhoneBox {
	t.Helper()
	// 32 字节 key 的 hex
	pb, err := NewPhoneBox("pepper-value",
		"000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f")
	if err != nil {
		t.Fatal(err)
	}
	return pb
}

func TestHMACIsStableAndPeppered(t *testing.T) {
	pb := newPhoneBox(t)
	a := pb.HMAC("13800008888")
	if a != pb.HMAC("13800008888") {
		t.Error("同一号码两次 HMAC 不一致")
	}
	if a == pb.HMAC("13800008889") {
		t.Error("不同号码 HMAC 相同")
	}
	other, _ := NewPhoneBox("different-pepper",
		"000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f")
	if a == other.HMAC("13800008888") {
		t.Error("换了 pepper 结果不变——pepper 没参与运算")
	}
}

func TestTail(t *testing.T) {
	pb := newPhoneBox(t)
	if got := pb.Tail("13800008888"); got != "8888" {
		t.Errorf("Tail = %q，期望 8888", got)
	}
	if got := pb.Tail("123"); got != "123" {
		t.Errorf("短号码不该 panic，得到 %q", got)
	}
}

func TestSealOpenRoundTrip(t *testing.T) {
	pb := newPhoneBox(t)
	enc, err := pb.Seal("13800008888")
	if err != nil {
		t.Fatal(err)
	}
	if string(enc) == "13800008888" {
		t.Fatal("密文等于明文——没有加密")
	}
	got, err := pb.Open(enc)
	if err != nil || got != "13800008888" {
		t.Fatalf("解出 %q err=%v", got, err)
	}
}

func TestSealUsesRandomNonce(t *testing.T) {
	pb := newPhoneBox(t)
	a, _ := pb.Seal("13800008888")
	b, _ := pb.Seal("13800008888")
	if string(a) == string(b) {
		t.Error("同一号码两次加密结果相同——nonce 没随机")
	}
}

func TestOpenRejectsTamperedCiphertext(t *testing.T) {
	pb := newPhoneBox(t)
	enc, _ := pb.Seal("13800008888")
	enc[len(enc)-1] ^= 0xff
	if _, err := pb.Open(enc); err == nil {
		t.Error("被篡改的密文竟然解开了——GCM 认证没生效")
	}
}

func TestNormalizePhone(t *testing.T) {
	ok := []string{"13800008888", "+8613800008888", "8613800008888", "138 0000 8888"}
	for _, in := range ok {
		got, err := NormalizePhone(in)
		if err != nil || got != "13800008888" {
			t.Errorf("NormalizePhone(%q) = %q err=%v", in, got, err)
		}
	}
	bad := []string{"", "12345", "23800008888", "abcdefghijk", "138000088888888"}
	for _, in := range bad {
		if _, err := NormalizePhone(in); err == nil {
			t.Errorf("NormalizePhone(%q) 应当报错", in)
		}
	}
}
```

- [ ] **Step 2: 实现手机号处理**

`server/go/internal/auth/phone.go`:

```go
package auth

import (
	"crypto/aes"
	"crypto/cipher"
	"crypto/hmac"
	"crypto/rand"
	"crypto/sha256"
	"encoding/hex"
	"errors"
	"regexp"
	"strings"
)

// PhoneBox 管手机号的三种形态：
//   HMAC —— 登录查询与唯一性，不可逆
//   Tail —— 界面展示，不可逆
//   Seal/Open —— 完整号码，可逆，存在单独的表里
//
// 两把密钥的运维含义不同：
//   pepper 换了全体用户无法登录（HMAC 对不上），绝不轮换
//   key    换了旧密文解不开，但登录不受影响
type PhoneBox struct {
	pepper []byte
	aead   cipher.AEAD
}

func NewPhoneBox(pepper, keyHex string) (*PhoneBox, error) {
	if pepper == "" {
		return nil, errors.New("auth: phone_pepper 必填")
	}
	key, err := hex.DecodeString(keyHex)
	if err != nil || len(key) != 32 {
		return nil, errors.New("auth: phone_key 必须是 32 字节的 hex")
	}
	blk, err := aes.NewCipher(key)
	if err != nil {
		return nil, err
	}
	aead, err := cipher.NewGCM(blk)
	if err != nil {
		return nil, err
	}
	return &PhoneBox{pepper: []byte(pepper), aead: aead}, nil
}

func (p *PhoneBox) HMAC(phone string) string {
	m := hmac.New(sha256.New, p.pepper)
	m.Write([]byte(phone))
	return hex.EncodeToString(m.Sum(nil))
}

func (p *PhoneBox) Tail(phone string) string {
	if len(phone) <= 4 {
		return phone
	}
	return phone[len(phone)-4:]
}

func (p *PhoneBox) Seal(phone string) ([]byte, error) {
	nonce := make([]byte, p.aead.NonceSize())
	if _, err := rand.Read(nonce); err != nil {
		return nil, err
	}
	return p.aead.Seal(nonce, nonce, []byte(phone), nil), nil
}

func (p *PhoneBox) Open(enc []byte) (string, error) {
	n := p.aead.NonceSize()
	if len(enc) < n {
		return "", errors.New("auth: 密文过短")
	}
	out, err := p.aead.Open(nil, enc[:n], enc[n:], nil)
	if err != nil {
		return "", err
	}
	return string(out), nil
}

var reCN = regexp.MustCompile(`^1[3-9]\d{9}$`)

// NormalizePhone 只处理中国大陆号码。国际号码不在范围内（见 spec 第 14 节）。
func NormalizePhone(in string) (string, error) {
	s := strings.NewReplacer(" ", "", "-", "").Replace(in)
	s = strings.TrimPrefix(s, "+")
	s = strings.TrimPrefix(s, "86")
	if !reCN.MatchString(s) {
		return "", errors.New("auth: 手机号格式不合法")
	}
	return s, nil
}
```

- [ ] **Step 3: 写失败的测试（验证码与防刷）**

`server/go/internal/auth/sms_test.go`:

```go
package auth

import (
	"context"
	"testing"
	"time"

	"github.com/dayuer/stickbox/server/go/internal/store"
)

type fakeSender struct {
	sent []string // 收到的验证码，测试里直接取
}

func (f *fakeSender) Send(ctx context.Context, phone, code string) error {
	f.sent = append(f.sent, code)
	return nil
}

func newSMS(t *testing.T) (*SMS, *fakeSender, *store.Store, *time.Time) {
	t.Helper()
	st, err := store.Open(t.TempDir() + "/jobs.db")
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { st.Close() })
	f := &fakeSender{}
	clk := time.Unix(1_000_000, 0)
	s := NewSMS(st, f, func() time.Time { return clk })
	return s, f, st, &clk
}

func TestIssueThenVerify(t *testing.T) {
	s, f, _, _ := newSMS(t)
	if err := s.Issue(context.Background(), "13800008888", "H", "1.2.3.4"); err != nil {
		t.Fatal(err)
	}
	if len(f.sent) != 1 || len(f.sent[0]) != 6 {
		t.Fatalf("发出的验证码 = %v，期望一条 6 位", f.sent)
	}
	if err := s.Verify("H", f.sent[0]); err != nil {
		t.Fatalf("正确验证码校验失败：%v", err)
	}
}

// 验证码一次性：用过就作废，防重放。
func TestVerifyIsSingleUse(t *testing.T) {
	s, f, _, _ := newSMS(t)
	s.Issue(context.Background(), "13800008888", "H", "1.2.3.4")
	code := f.sent[0]
	s.Verify("H", code)
	if err := s.Verify("H", code); err == nil {
		t.Error("同一验证码用了第二次")
	}
}

func TestVerifyRejectsWrongAndExpired(t *testing.T) {
	s, f, _, clk := newSMS(t)
	s.Issue(context.Background(), "13800008888", "H", "1.2.3.4")
	if err := s.Verify("H", "000000"); err == nil {
		t.Error("错误验证码应被拒")
	}
	*clk = clk.Add(6 * time.Minute)
	if err := s.Verify("H", f.sent[0]); err == nil {
		t.Error("过期验证码应被拒（TTL 5 分钟）")
	}
}

// 单个验证码最多试 5 次，超了整条作废——防暴力猜 6 位数字。
func TestVerifyLocksAfterFiveAttempts(t *testing.T) {
	s, f, _, _ := newSMS(t)
	s.Issue(context.Background(), "13800008888", "H", "1.2.3.4")
	for i := 0; i < 5; i++ {
		s.Verify("H", "000000")
	}
	if err := s.Verify("H", f.sent[0]); err == nil {
		t.Error("超过尝试次数后连正确验证码也该被拒")
	}
}

// 闸一：同号码 60 秒内不得重发。
func TestIssueRateLimitPerPhone(t *testing.T) {
	s, _, _, clk := newSMS(t)
	ctx := context.Background()
	if err := s.Issue(ctx, "13800008888", "H", "1.2.3.4"); err != nil {
		t.Fatal(err)
	}
	if err := s.Issue(ctx, "13800008888", "H", "1.2.3.4"); err == nil {
		t.Error("60 秒内重发应被拒")
	}
	*clk = clk.Add(61 * time.Second)
	if err := s.Issue(ctx, "13800008888", "H", "1.2.3.4"); err != nil {
		t.Errorf("61 秒后应允许重发：%v", err)
	}
}

// 闸二：同号码每天最多 10 条。
func TestIssueDailyCapPerPhone(t *testing.T) {
	s, _, _, clk := newSMS(t)
	ctx := context.Background()
	for i := 0; i < 10; i++ {
		if err := s.Issue(ctx, "13800008888", "H", "1.2.3.4"); err != nil {
			t.Fatalf("第 %d 条就被拒了：%v", i+1, err)
		}
		*clk = clk.Add(61 * time.Second)
	}
	if err := s.Issue(ctx, "13800008888", "H", "1.2.3.4"); err == nil {
		t.Error("第 11 条应被拒")
	}
}

// 闸三：同 IP 每小时最多 20 条——挡住换号码刷的。
func TestIssueRateLimitPerIP(t *testing.T) {
	s, _, _, clk := newSMS(t)
	ctx := context.Background()
	for i := 0; i < 20; i++ {
		phone := "1380000" + string(rune('0'+i/10)) + string(rune('0'+i%10)) + "8"
		if err := s.Issue(ctx, phone, "H"+phone, "9.9.9.9"); err != nil {
			t.Fatalf("第 %d 条就被拒了：%v", i+1, err)
		}
		*clk = clk.Add(time.Second)
	}
	if err := s.Issue(ctx, "13900000000", "HX", "9.9.9.9"); err == nil {
		t.Error("同 IP 第 21 条应被拒")
	}
}
```

- [ ] **Step 4: 运行确认失败**

Run: `cd server/go && go test ./internal/auth/`
Expected: FAIL，`undefined: NewSMS`

- [ ] **Step 5: 实现**

`server/go/internal/auth/sms.go`:

```go
package auth

import (
	"context"
	"crypto/rand"
	"crypto/sha256"
	"encoding/hex"
	"errors"
	"math/big"
	"sync"
	"time"

	"github.com/dayuer/stickbox/server/go/internal/store"
)

// Sender 是短信服务商的抽象。测试用假实现——单测和集成测试绝不真发短信。
type Sender interface {
	Send(ctx context.Context, phone, code string) error
}

const (
	codeTTL      = 5 * time.Minute
	resendGap    = 60 * time.Second
	dailyPerPhone = 10
	hourlyPerIP   = 20
	maxAttempts   = 5
)

var (
	ErrTooFrequent = errors.New("auth: 发送过于频繁")
	ErrDailyCap    = errors.New("auth: 今日发送次数已达上限")
	ErrIPCap       = errors.New("auth: 该网络发送次数已达上限")
	ErrBadCode     = errors.New("auth: 验证码错误或已失效")
)

type SMS struct {
	st     *store.Store
	sender Sender
	now    func() time.Time

	mu sync.Mutex
	ip map[string][]time.Time // IP → 最近一小时的发送时刻
}

func NewSMS(st *store.Store, sender Sender, now func() time.Time) *SMS {
	if now == nil {
		now = time.Now
	}
	return &SMS{st: st, sender: sender, now: now, ip: map[string][]time.Time{}}
}

// Issue 发一条验证码。四道闸按「先便宜后昂贵」的顺序查：
// 内存里的 IP 计数最便宜，真发短信最贵。
func (s *SMS) Issue(ctx context.Context, phone, phoneHMAC, ip string) error {
	if err := s.checkIP(ip); err != nil {
		return err
	}
	rec, found, err := s.st.SMSCode(phoneHMAC)
	if err != nil {
		return err
	}
	now := s.now()
	dayStart, dayCount := rec.DayStart, rec.DayCount
	if found {
		if now.Sub(time.Unix(rec.SentAt, 0)) < resendGap {
			return ErrTooFrequent
		}
		if now.Unix()-dayStart >= 86400 {
			dayStart, dayCount = now.Unix(), 0
		}
		if dayCount >= dailyPerPhone {
			return ErrDailyCap
		}
	} else {
		dayStart, dayCount = now.Unix(), 0
	}

	code := randomCode()
	if err := s.st.PutSMSCode(store.SMSCode{
		PhoneHMAC: phoneHMAC,
		CodeHash:  hashCode(code),
		Expires:   now.Add(codeTTL).Unix(),
		Attempts:  0,
		SentAt:    now.Unix(),
		DayStart:  dayStart,
		DayCount:  dayCount + 1,
	}); err != nil {
		return err
	}
	if err := s.sender.Send(ctx, phone, code); err != nil {
		return err
	}
	s.noteIP(ip)
	return nil
}

// Verify 校验并作废。成功即一次性失效，防重放。
func (s *SMS) Verify(phoneHMAC, code string) error {
	rec, found, err := s.st.SMSCode(phoneHMAC)
	if err != nil {
		return err
	}
	if !found || s.now().Unix() > rec.Expires || rec.Attempts >= maxAttempts {
		return ErrBadCode
	}
	if rec.CodeHash != hashCode(code) {
		s.st.BumpSMSAttempts(phoneHMAC)
		return ErrBadCode
	}
	return s.st.DeleteSMSCode(phoneHMAC)
}

func (s *SMS) checkIP(ip string) error {
	s.mu.Lock()
	defer s.mu.Unlock()
	cut := s.now().Add(-time.Hour)
	kept := s.ip[ip][:0]
	for _, t := range s.ip[ip] {
		if t.After(cut) {
			kept = append(kept, t)
		}
	}
	s.ip[ip] = kept
	if len(kept) >= hourlyPerIP {
		return ErrIPCap
	}
	return nil
}

func (s *SMS) noteIP(ip string) {
	s.mu.Lock()
	s.ip[ip] = append(s.ip[ip], s.now())
	s.mu.Unlock()
}

func randomCode() string {
	n, err := rand.Int(rand.Reader, big.NewInt(1_000_000))
	if err != nil {
		panic(err)
	}
	s := n.String()
	for len(s) < 6 {
		s = "0" + s
	}
	return s
}

func hashCode(code string) string {
	sum := sha256.Sum256([]byte(code))
	return hex.EncodeToString(sum[:])
}
```

- [ ] **Step 6: 补 store 的验证码读写**

追加到 `server/go/internal/store/user.go`:

```go
type SMSCode struct {
	PhoneHMAC string
	CodeHash  string
	Expires   int64
	Attempts  int
	SentAt    int64
	DayStart  int64
	DayCount  int
}

func (s *Store) SMSCode(phoneHMAC string) (SMSCode, bool, error) {
	var c SMSCode
	err := s.db.QueryRow(
		`SELECT phone_hmac,code_hash,expires,attempts,sent_at,day_start,day_count
		 FROM sms_codes WHERE phone_hmac=?`, phoneHMAC).
		Scan(&c.PhoneHMAC, &c.CodeHash, &c.Expires, &c.Attempts,
			&c.SentAt, &c.DayStart, &c.DayCount)
	if err != nil {
		if err.Error() == "sql: no rows in result set" {
			return SMSCode{}, false, nil
		}
		return SMSCode{}, false, err
	}
	return c, true, nil
}

// PutSMSCode 按号码单行覆盖——重发即替换，不留历史。
func (s *Store) PutSMSCode(c SMSCode) error {
	s.wmu.Lock()
	defer s.wmu.Unlock()
	_, err := s.db.Exec(
		`INSERT INTO sms_codes(phone_hmac,code_hash,expires,attempts,sent_at,day_start,day_count)
		 VALUES(?,?,?,?,?,?,?)
		 ON CONFLICT(phone_hmac) DO UPDATE SET
		   code_hash=excluded.code_hash, expires=excluded.expires,
		   attempts=0, sent_at=excluded.sent_at,
		   day_start=excluded.day_start, day_count=excluded.day_count`,
		c.PhoneHMAC, c.CodeHash, c.Expires, c.Attempts, c.SentAt, c.DayStart, c.DayCount)
	return err
}

func (s *Store) BumpSMSAttempts(phoneHMAC string) error {
	s.wmu.Lock()
	defer s.wmu.Unlock()
	_, err := s.db.Exec(
		`UPDATE sms_codes SET attempts=attempts+1 WHERE phone_hmac=?`, phoneHMAC)
	return err
}

func (s *Store) DeleteSMSCode(phoneHMAC string) error {
	s.wmu.Lock()
	defer s.wmu.Unlock()
	_, err := s.db.Exec(`DELETE FROM sms_codes WHERE phone_hmac=?`, phoneHMAC)
	return err
}
```

- [ ] **Step 7: 运行确认通过**

Run: `cd server/go && go test ./internal/auth/ ./internal/store/ -v`
Expected: PASS，四道闸的用例全绿

- [ ] **Step 8: 提交**

```bash
git add server/go/internal/auth server/go/internal/store
git commit -m "feat(server): 手机号 HMAC/加密与验证码四道防刷闸"
```

---

## Task 10: auth — session token 与按 user 的 ACL

**Files:**
- Modify: `server/go/internal/auth/verifier.go`（`Identity` 加 `UserID`）
- Modify: `server/go/internal/auth/acl.go`（`app` 角色按 user 判定）
- Modify: `server/go/internal/auth/acl_test.go`
- Create: `server/go/internal/auth/session.go`
- Create: `server/go/internal/auth/session_test.go`

`app` 密钥现在绑 **user** 而不是单个 dev，所以 ACL 从「topic 里的 dev 等于身份
里的 dev」变成「topic 里的 dev 属于该 user」。`device` 角色不变。

- [ ] **Step 1: 改 ACL 测试**

`server/go/internal/auth/acl_test.go` 整体替换：

```go
package auth

import "testing"

// fakeMembership: u1 名下有 aaa 和 ccc，没有 bbb。
type fakeMembership struct{}

func (fakeMembership) HasDevice(userID, dev string) bool {
	return userID == "u1" && (dev == "aaa" || dev == "ccc")
}

// 这张矩阵是安全边界，改动必须先改这里。
func TestACLMatrix(t *testing.T) {
	dev := Identity{Dev: "aaa", Role: RoleDevice}
	app := Identity{UserID: "u1", Role: RoleApp}

	cases := []struct {
		name  string
		id    Identity
		topic string
		write bool
		want  bool
	}{
		{"设备发状态", dev, "printer/aaa/status", true, true},
		{"设备发档案", dev, "printer/aaa/ident", true, true},
		{"设备收作业", dev, "printer/aaa/job", false, true},
		{"设备收命令", dev, "printer/aaa/cmd", false, true},
		{"设备收怪癖档案", dev, "printer/aaa/profile", false, true},
		{"设备不能自派作业", dev, "printer/aaa/job", true, false},
		{"设备不能碰别人", dev, "printer/bbb/status", true, false},
		{"设备不能订阅别人", dev, "printer/bbb/status", false, false},
		{"设备不能用通配符", dev, "printer/+/status", false, false},

		{"App 订阅名下设备", app, "printer/aaa/status", false, true},
		{"App 订阅名下另一台", app, "printer/ccc/status", false, true},
		{"App 不能订阅名下之外的", app, "printer/bbb/status", false, false},
		{"App 不能伪造状态", app, "printer/aaa/status", true, false},
		{"App 不能派作业", app, "printer/aaa/job", true, false},
		{"App 不能收作业内容", app, "printer/aaa/job", false, false},
		{"App 不能收怪癖档案", app, "printer/aaa/profile", false, false},

		{"越界前缀", dev, "printer/aaa", false, false},
		{"多余层级", dev, "printer/aaa/status/x", false, false},
		{"完全无关的 topic", dev, "$SYS/broker/uptime", false, false},
	}
	for _, c := range cases {
		if got := ACLAllowed(c.id, c.topic, c.write, fakeMembership{}); got != c.want {
			t.Errorf("%s: ACLAllowed(%+v, %q, write=%v) = %v，期望 %v",
				c.name, c.id, c.topic, c.write, got, c.want)
		}
	}
}
```

- [ ] **Step 2: 运行确认失败**

Run: `cd server/go && go test ./internal/auth/ -run TestACLMatrix`
Expected: FAIL，`too many arguments in call to ACLAllowed`

- [ ] **Step 3: 改实现**

`server/go/internal/auth/acl.go` 整体替换：

```go
package auth

import "strings"

// Membership 回答「这个 dev 属于这个 user 吗」。
// 实现方负责缓存——ACL 判定在 MQTT 热路径上，不能每次查库。
type Membership interface {
	HasDevice(userID, dev string) bool
}

// ACLAllowed 判定某身份能否读/写某 topic。
//
// topic 一律是 printer/{dev}/{leaf} 三段。不做通配符展开——
// 设备和 App 都不需要跨设备订阅，把口子焊死比事后审计便宜。
func ACLAllowed(id Identity, topic string, write bool, m Membership) bool {
	parts := strings.Split(topic, "/")
	if len(parts) != 3 || parts[0] != "printer" {
		return false
	}
	dev, leaf := parts[1], parts[2]

	switch id.Role {
	case RoleDevice:
		if dev != id.Dev {
			return false
		}
		if write {
			return leaf == "status" || leaf == "ident"
		}
		return leaf == "job" || leaf == "cmd" || leaf == "profile"

	case RoleApp:
		// App 只旁听自己名下设备的状态。
		// 能写 status 就能伪造打印机状态；能读 job 就能截胡作业信令；
		// 能读 profile 也没有意义——那是设备侧的 USB 层配置。
		if write || leaf != "status" {
			return false
		}
		return m != nil && m.HasDevice(id.UserID, dev)
	}
	return false
}
```

`server/go/internal/auth/verifier.go` 的 `Identity` 加一个字段，并在构造处填上：

```go
type Identity struct {
	Dev    string // device 角色专用
	UserID string // app 角色专用
	Role   Role
	KeyID  string
}
```

`Verify` 里构造 `Identity` 的那一行改为：

```go
	id := Identity{Dev: k.Dev, UserID: k.UserID, Role: Role(k.Role), KeyID: k.KeyID}
```

- [ ] **Step 4: 写 session 测试**

`server/go/internal/auth/session_test.go`:

```go
package auth

import (
	"testing"

	"github.com/dayuer/stickbox/server/go/internal/store"
)

func newStore(t *testing.T) *store.Store {
	t.Helper()
	st, err := store.Open(t.TempDir() + "/jobs.db")
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { st.Close() })
	return st
}

func TestIssueSessionThenVerify(t *testing.T) {
	st := newStore(t)
	v := NewVerifier(st)
	token, err := IssueSession(st, "u1", "iPhone 15")
	if err != nil {
		t.Fatal(err)
	}
	id, err := v.Verify(token)
	if err != nil {
		t.Fatal(err)
	}
	if id.Role != RoleApp || id.UserID != "u1" {
		t.Errorf("身份 = %+v", id)
	}
}

// 手机丢了只踢那一把，别的不受影响。
func TestRevokeOneSession(t *testing.T) {
	st := newStore(t)
	v := NewVerifier(st)
	a, _ := IssueSession(st, "u1", "手机 A")
	b, _ := IssueSession(st, "u1", "手机 B")

	keyID, _, _ := SplitToken(a)
	if err := RevokeSession(st, v, keyID); err != nil {
		t.Fatal(err)
	}
	if _, err := v.Verify(a); err == nil {
		t.Error("被吊销的 token 仍可用")
	}
	if _, err := v.Verify(b); err != nil {
		t.Errorf("另一台手机被误伤：%v", err)
	}
}

func TestRevokeAllSessions(t *testing.T) {
	st := newStore(t)
	v := NewVerifier(st)
	a, _ := IssueSession(st, "u1", "手机 A")
	b, _ := IssueSession(st, "u1", "手机 B")
	c, _ := IssueSession(st, "u2", "别人的手机")

	if err := RevokeAllSessions(st, v, "u1"); err != nil {
		t.Fatal(err)
	}
	for i, tok := range []string{a, b} {
		if _, err := v.Verify(tok); err == nil {
			t.Errorf("u1 的第 %d 把 token 仍可用", i+1)
		}
	}
	if _, err := v.Verify(c); err != nil {
		t.Errorf("别的用户被误伤：%v", err)
	}
}

// 设备密钥和 session 走同一套校验，但角色和归属不同。
func TestIssueDeviceKeyBindsUser(t *testing.T) {
	st := newStore(t)
	v := NewVerifier(st)
	token, err := IssueDeviceKey(st, "u1", "f412fa87c9e0", "工位打印机")
	if err != nil {
		t.Fatal(err)
	}
	id, err := v.Verify(token)
	if err != nil {
		t.Fatal(err)
	}
	if id.Role != RoleDevice || id.Dev != "f412fa87c9e0" || id.UserID != "u1" {
		t.Errorf("身份 = %+v", id)
	}
}
```

- [ ] **Step 5: 实现 session**

`server/go/internal/auth/session.go`:

```go
package auth

import "github.com/dayuer/stickbox/server/go/internal/store"

// IssueSession 为一台手机签发一把 app 令牌。
//
// 刻意不设过期：打印是低频操作，强制重新登录只会激怒用户。
// 要下线就靠吊销——一台手机一把，丢了单独踢。
func IssueSession(st *store.Store, userID, deviceName string) (string, error) {
	keyID, secret, token := NewToken()
	hash, err := HashSecret(secret)
	if err != nil {
		return "", err
	}
	err = st.InsertKey(store.Key{
		KeyID: keyID, Dev: "", Role: string(RoleApp),
		UserID: userID, Name: deviceName, Hash: hash,
	})
	return token, err
}

// IssueDeviceKey 为一台桥签发 device 密钥，绑到该用户。
// enroll 走这里，运维的 device add 也走这里。
func IssueDeviceKey(st *store.Store, userID, dev, name string) (string, error) {
	keyID, secret, token := NewToken()
	hash, err := HashSecret(secret)
	if err != nil {
		return "", err
	}
	err = st.InsertKey(store.Key{
		KeyID: keyID, Dev: dev, Role: string(RoleDevice),
		UserID: userID, Name: name, Hash: hash,
	})
	return token, err
}

// RevokeSession 吊销一把。必须同时清校验缓存，否则它还能再活一小时。
func RevokeSession(st *store.Store, v *Verifier, keyID string) error {
	if err := st.DisableKey(keyID); err != nil {
		return err
	}
	v.Invalidate(keyID)
	return nil
}

func RevokeAllSessions(st *store.Store, v *Verifier, userID string) error {
	ids, err := st.DisableKeysOfUser(userID, string(RoleApp))
	if err != nil {
		return err
	}
	for _, id := range ids {
		v.Invalidate(id)
	}
	return nil
}

// RevokeDeviceKeys 吊销某台桥的全部 device 密钥。重置设备时用。
func RevokeDeviceKeys(st *store.Store, v *Verifier, dev string) error {
	ids, err := st.DisableKeysOfDevice(dev, string(RoleDevice))
	if err != nil {
		return err
	}
	for _, id := range ids {
		v.Invalidate(id)
	}
	return nil
}
```

- [ ] **Step 6: 补 store 的批量吊销**

追加到 `server/go/internal/store/key.go`:

```go
// DisableKeysOfUser 吊销某用户某角色的全部密钥，返回被吊销的 key_id
// ——调用方要拿它去清 Verifier 的缓存。
func (s *Store) DisableKeysOfUser(userID, role string) ([]string, error) {
	return s.disableKeysWhere(`user_id=? AND role=? AND disabled=0`, userID, role)
}

func (s *Store) DisableKeysOfDevice(dev, role string) ([]string, error) {
	return s.disableKeysWhere(`dev=? AND role=? AND disabled=0`, dev, role)
}

func (s *Store) disableKeysWhere(where string, args ...any) ([]string, error) {
	rows, err := s.db.Query(`SELECT key_id FROM devices WHERE `+where, args...)
	if err != nil {
		return nil, err
	}
	var ids []string
	for rows.Next() {
		var id string
		if err := rows.Scan(&id); err != nil {
			rows.Close()
			return nil, err
		}
		ids = append(ids, id)
	}
	rows.Close()
	if err := rows.Err(); err != nil {
		return nil, err
	}
	s.wmu.Lock()
	defer s.wmu.Unlock()
	for _, id := range ids {
		if _, err := s.db.Exec(`UPDATE devices SET disabled=1 WHERE key_id=?`, id); err != nil {
			return nil, err
		}
	}
	return ids, nil
}
```

- [ ] **Step 7: 运行确认通过**

Run: `cd server/go && go test ./internal/auth/ -v`
Expected: PASS，ACL 矩阵 19 条 + session 4 个用例

- [ ] **Step 8: 提交**

```bash
git add server/go/internal/auth server/go/internal/store/key.go
git commit -m "feat(server): session token 与按 user 的 ACL"
```

---

## Task 11: registry — actor 生命周期

**Files:**
- Create: `server/go/internal/registry/registry.go`
- Create: `server/go/internal/registry/registry_test.go`

- [ ] **Step 1: 写失败的测试**

`server/go/internal/registry/registry_test.go`:

```go
package registry

import (
	"sync"
	"testing"
	"time"

	"github.com/dayuer/stickbox/server/go/internal/device"
	"github.com/dayuer/stickbox/server/go/internal/store"
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

func newReg(t *testing.T) (*Registry, *store.Store, *nopPub) {
	t.Helper()
	st, err := store.Open(t.TempDir() + "/jobs.db")
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { st.Close() })
	pub := &nopPub{}
	r := New(st, pub, device.Options{
		JobTimeout:  180 * time.Second,
		IdleTimeout: 100 * time.Millisecond,
	})
	t.Cleanup(r.Shutdown)
	return r, st, pub
}

// 第一条消息就该把 actor 建起来。
func TestSendCreatesActor(t *testing.T) {
	r, _, _ := newReg(t)
	r.Send("d1", device.Msg{Kind: device.KindHeartbeat, Serial: "PA"})
	if got := r.Online(); len(got) != 1 || got[0] != "d1" {
		t.Fatalf("在线列表 = %v，期望 [d1]", got)
	}
}

// 离线后 actor 自己退出并被摘除，不能泄漏 goroutine。
func TestActorExitsAndIsRemoved(t *testing.T) {
	r, _, _ := newReg(t)
	r.Send("d1", device.Msg{Kind: device.KindHeartbeat, Serial: "PA"})
	deadline := time.Now().Add(3 * time.Second)
	for time.Now().Before(deadline) {
		if len(r.Online()) == 0 {
			return
		}
		time.Sleep(10 * time.Millisecond)
	}
	t.Fatal("超过 IdleTimeout 后 actor 仍在 registry 里")
}

// 摘除后再来消息要能重建，且队列还在。
func TestActorRebuildsAfterExit(t *testing.T) {
	r, st, pub := newReg(t)
	st.InsertJob(store.Job{ID: "j1", Dev: "d1", Serial: "PA", State: store.StateQueued})

	r.Send("d1", device.Msg{Kind: device.KindHeartbeat, Serial: "PA"})
	for len(r.Online()) > 0 {
		time.Sleep(10 * time.Millisecond)
	}
	if j, _, _ := st.GetJob("j1"); j.State != store.StateDownloading {
		t.Fatalf("第一次没派出去，state=%q", j.State)
	}
	// 退出时作业仍是 downloading，重建后由超时或回执推进——队列本身没丢
	if _, ok, _ := st.GetJob("j1"); !ok {
		t.Fatal("actor 退出把作业带走了")
	}
	r.Send("d1", device.Msg{Kind: device.KindHeartbeat, Serial: "PA"})
	if len(r.Online()) != 1 {
		t.Fatal("actor 没能重建")
	}
	_ = pub
}

func TestConcurrentSendIsSafe(t *testing.T) {
	r, _, _ := newReg(t)
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
```

- [ ] **Step 2: 运行确认失败**

Run: `cd server/go && go test ./internal/registry/`
Expected: FAIL，`undefined: New`

- [ ] **Step 3: 实现**

`server/go/internal/registry/registry.go`:

```go
// Package registry 管 devid → actor 的映射与 actor 的生死。
//
// 一台设备一个 goroutine。actor 的状态全部私有，registry 只负责创建、
// 路由消息、在它退出后摘除，以及 panic 时重建。
package registry

import (
	"log/slog"
	"sync"

	"github.com/dayuer/stickbox/server/go/internal/device"
	"github.com/dayuer/stickbox/server/go/internal/store"
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
	a := r.getOrCreate(dev)
	if a != nil {
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

// run 跑一个 actor，并在它 panic 时把它摘掉。
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
```

- [ ] **Step 4: 运行确认通过**

Run: `cd server/go && go test -race ./internal/registry/ -v`
Expected: PASS，无 race 报告

- [ ] **Step 5: 提交**

```bash
git add server/go/internal/registry
git commit -m "feat(server): actor registry——分片路由、离线摘除、panic 不重建"
```

---

## Task 12: raster — 入口校验与 render-profile 解析

**Files:**
- Create: `server/go/internal/raster/verify.go`
- Create: `server/go/internal/raster/profile.go`
- Create: `server/go/internal/raster/verify_test.go`
- Create: `server/go/internal/raster/profile_test.go`

服务端不解析文档、设备不认识格式——**一份标称 URF 的 PDF 会被原样送进打印机，
用户收到几十张乱码纸**。这个包是唯一的拦截点。

参考实现见 `tools/reference/render.py` 的 `fix_page_count`：Debian 版
`cupsfilter` 把页数字段写成 0，打印机据此认为文档为空。客户端自己写编码器
一样会犯这个错。

- [ ] **Step 1: 写失败的测试**

`server/go/internal/raster/verify_test.go`:

```go
package raster

import (
	"encoding/binary"
	"testing"
)

// urf 拼一份最小可用的 URF：魔数 + 页数 + 一个 32 字节页头。
func urf(pages uint32, w, h uint32) []byte {
	b := make([]byte, 12+32)
	copy(b, "UNIRAST\x00")
	binary.BigEndian.PutUint32(b[8:12], pages)
	binary.BigEndian.PutUint32(b[12+12:12+16], w)
	binary.BigEndian.PutUint32(b[12+16:12+20], h)
	return b
}

func TestVerifyAcceptsGoodURF(t *testing.T) {
	info, err := Verify(FormatURF, urf(2, 4962, 7014))
	if err != nil {
		t.Fatalf("合法 URF 被拒：%v", err)
	}
	if info.Pages != 2 || info.Width != 4962 || info.Height != 7014 {
		t.Errorf("解析结果 = %+v", info)
	}
}

// 最要命的一条：把 PDF 当 URF 传。
func TestVerifyRejectsPDFClaimingURF(t *testing.T) {
	_, err := Verify(FormatURF, []byte("%PDF-1.7\n%\xc7\xec\x8f\xa2\n1 0 obj"))
	if err == nil {
		t.Fatal("PDF 冒充 URF 竟然通过了——用户会收到几十张乱码纸")
	}
}

// 页数为 0 时打印机认为文档为空，什么都不打。
func TestVerifyRejectsZeroPages(t *testing.T) {
	if _, err := Verify(FormatURF, urf(0, 4962, 7014)); err == nil {
		t.Error("页数 0 应被拒")
	}
}

func TestVerifyRejectsInsaneDimensions(t *testing.T) {
	for _, c := range []struct{ w, h uint32 }{{0, 7014}, {4962, 0}, {40000, 7014}} {
		if _, err := Verify(FormatURF, urf(1, c.w, c.h)); err == nil {
			t.Errorf("尺寸 %dx%d 应被拒", c.w, c.h)
		}
	}
}

func TestVerifyRejectsTruncated(t *testing.T) {
	if _, err := Verify(FormatURF, []byte("UNIRAST\x00\x00")); err == nil {
		t.Error("截断的数据应被拒")
	}
}

func TestVerifyPWG(t *testing.T) {
	b := make([]byte, 64)
	copy(b, "RaS2")
	if _, err := Verify(FormatPWG, b); err != nil {
		t.Errorf("合法 PWG 被拒：%v", err)
	}
	if _, err := Verify(FormatPWG, []byte("RaS9xxxx")); err == nil {
		t.Error("错误魔数应被拒")
	}
}

func TestFormatFromContentType(t *testing.T) {
	cases := map[string]Format{
		"image/urf":        FormatURF,
		"image/pwg-raster": FormatPWG,
	}
	for ct, want := range cases {
		if got, ok := FormatFromContentType(ct); !ok || got != want {
			t.Errorf("%q → %v ok=%v", ct, got, ok)
		}
	}
	for _, ct := range []string{"application/pdf", "image/png", "text/plain", ""} {
		if _, ok := FormatFromContentType(ct); ok {
			t.Errorf("%q 不该被接受", ct)
		}
	}
}
```

- [ ] **Step 2: 实现校验**

`server/go/internal/raster/verify.go`:

```go
// Package raster 是上传文档的唯一拦截点。
//
// 服务端不解析文档内容，设备也不认识格式——一份标称 URF 的 PDF 会被原样送进
// 打印机，用户收到几十张乱码纸。这里挡住它。
package raster

import (
	"encoding/binary"
	"fmt"
	"strings"
)

type Format int

const (
	FormatURF Format = iota
	FormatPWG
)

var magicURF = []byte("UNIRAST\x00")
var magicPWG = []byte("RaS2")

type Info struct {
	Pages         int
	Width, Height int
}

func FormatFromContentType(ct string) (Format, bool) {
	switch strings.ToLower(strings.TrimSpace(strings.Split(ct, ";")[0])) {
	case "image/urf":
		return FormatURF, true
	case "image/pwg-raster":
		return FormatPWG, true
	}
	return 0, false
}

// Verify 只看头部几十个字节。三条校验：魔数、页数非 0、首页尺寸合理。
//
// 只需要 head，调用方传前 4KB 即可——上传是流式的，不要为了校验把整份读进内存。
func Verify(f Format, head []byte) (Info, error) {
	switch f {
	case FormatURF:
		return verifyURF(head)
	case FormatPWG:
		return verifyPWG(head)
	}
	return Info{}, fmt.Errorf("raster: 未知格式")
}

func verifyURF(b []byte) (Info, error) {
	if len(b) < 12+32 {
		return Info{}, fmt.Errorf("raster: 数据过短（%d 字节），不是完整的 URF", len(b))
	}
	if string(b[:8]) != string(magicURF) {
		return Info{}, fmt.Errorf("raster: 魔数不匹配，期望 UNIRAST\\0，实际 %q", preview(b))
	}
	pages := binary.BigEndian.Uint32(b[8:12])
	if pages == 0 {
		// tools/reference/render.py 的 fix_page_count 就是为这个写的：
		// Debian 版 cupsfilter 写 0，打印机据此认为文档为空。
		return Info{}, fmt.Errorf("raster: 页数字段为 0，打印机会认为文档为空")
	}
	w := binary.BigEndian.Uint32(b[24:28])
	h := binary.BigEndian.Uint32(b[28:32])
	if w == 0 || h == 0 || w >= 30000 || h >= 30000 {
		return Info{}, fmt.Errorf("raster: 首页尺寸不合理 %dx%d", w, h)
	}
	return Info{Pages: int(pages), Width: int(w), Height: int(h)}, nil
}

func verifyPWG(b []byte) (Info, error) {
	if len(b) < 4 {
		return Info{}, fmt.Errorf("raster: 数据过短")
	}
	if string(b[:4]) != string(magicPWG) {
		return Info{}, fmt.Errorf("raster: 魔数不匹配，期望 RaS2，实际 %q", preview(b))
	}
	// PWG 的页头结构与 URF 不同，这里只认魔数。
	// 尺寸校验留给设备侧的打印机自己——PWG 路径当前没有客户端在用。
	return Info{}, nil
}

func preview(b []byte) string {
	if len(b) > 8 {
		b = b[:8]
	}
	return strings.ToValidUTF8(string(b), "?")
}
```

- [ ] **Step 3: 写 profile 解析测试**

`server/go/internal/raster/profile_test.go`:

```go
package raster

import "testing"

// 这串来自实测：HP Laser MFP 136a 的 IEEE-1284 URF 能力字段。
const caps = "CP1,IS1-4-5-19,MT1-2-3-4-5-6,RS600,V1.4,W8"

func TestParseCaps(t *testing.T) {
	p, err := ParseCaps(caps)
	if err != nil {
		t.Fatal(err)
	}
	if p.DPI != 600 {
		t.Errorf("DPI = %d，期望 600（RS600）", p.DPI)
	}
	if p.Color != "gray8" {
		t.Errorf("Color = %q，期望 gray8（W8）", p.Color)
	}
	a4 := p.Pages["a4"]
	if a4.W != 4962 || a4.H != 7014 {
		t.Errorf("A4 = %dx%d，期望 4962x7014", a4.W, a4.H)
	}
}

// 打印机只报了 RS300 就该是 300，不能硬编码 600。
func TestParseCapsHonorsReportedDPI(t *testing.T) {
	p, err := ParseCaps("RS300,W8")
	if err != nil {
		t.Fatal(err)
	}
	if p.DPI != 300 {
		t.Errorf("DPI = %d，期望 300", p.DPI)
	}
	if p.Pages["a4"].W != 2481 {
		t.Errorf("300dpi 的 A4 宽 = %d，期望 2481", p.Pages["a4"].W)
	}
}

// 多个分辨率时取最高的——URF 能力串可能写 RS300-600。
func TestParseCapsPicksHighestDPI(t *testing.T) {
	p, _ := ParseCaps("RS300-600,W8")
	if p.DPI != 600 {
		t.Errorf("DPI = %d，期望取最高的 600", p.DPI)
	}
}

func TestParseCapsRejectsGarbage(t *testing.T) {
	for _, in := range []string{"", "CP1,V1.4", "nonsense"} {
		if _, err := ParseCaps(in); err == nil {
			t.Errorf("ParseCaps(%q) 应当报错——没有 RS 字段就推不出尺寸", in)
		}
	}
}
```

- [ ] **Step 4: 实现 profile 解析**

`server/go/internal/raster/profile.go`:

```go
package raster

import (
	"errors"
	"strconv"
	"strings"
)

type PageSize struct{ W, H int }

// Profile 是下发给 App 的光栅参数。机型知识仍收敛在服务端，
// 只是形态从「拿 PPD 渲染」变成「把参数下发给客户端渲染」。
type Profile struct {
	Caps   string              `json:"urf_caps"`
	DPI    int                 `json:"dpi"`
	Color  string              `json:"color"`
	Format string              `json:"format"`
	Pages  map[string]PageSize `json:"pages"`
}

// 纸张物理尺寸（英寸）。乘以 dpi 得到像素。
var sheets = map[string][2]float64{
	"a4":     {8.27, 11.69},
	"letter": {8.5, 11.0},
}

// ParseCaps 从 IEEE-1284 的 URF 能力串解析光栅参数。
// 例：CP1,IS1-4-5-19,MT1-2-3-4-5-6,RS600,V1.4,W8
//
// 只认真正影响光栅的两个字段：RS（分辨率）和 W/SRGB（色彩）。
// 其余字段（CP 份数、IS 进纸盒、MT 介质类型）与光栅无关，忽略。
func ParseCaps(caps string) (Profile, error) {
	p := Profile{Caps: caps, Format: "urf", Color: "gray8",
		Pages: map[string]PageSize{}}
	for _, f := range strings.Split(caps, ",") {
		f = strings.TrimSpace(f)
		switch {
		case strings.HasPrefix(f, "RS"):
			// RS600 或 RS300-600，取最高的
			for _, v := range strings.Split(f[2:], "-") {
				if n, err := strconv.Atoi(v); err == nil && n > p.DPI {
					p.DPI = n
				}
			}
		case f == "SRGB24":
			p.Color = "srgb24"
		case f == "W8":
			p.Color = "gray8"
		}
	}
	if p.DPI <= 0 {
		return Profile{}, errors.New("raster: 能力串里没有 RS 字段，推不出光栅尺寸")
	}
	for name, wh := range sheets {
		p.Pages[name] = PageSize{
			W: int(wh[0] * float64(p.DPI)),
			H: int(wh[1] * float64(p.DPI)),
		}
	}
	return p, nil
}
```

- [ ] **Step 5: 运行确认通过**

Run: `cd server/go && go test ./internal/raster/ -v`
Expected: PASS，含「PDF 冒充 URF」那条

- [ ] **Step 6: 提交**

```bash
git add server/go/internal/raster
git commit -m "feat(server): 上传入口校验与 render-profile 解析"
```

---

## Task 13: tlsx — 证书热重载

**Files:**
- Create: `server/go/internal/tlsx/reloader.go`
- Create: `server/go/internal/tlsx/reloader_test.go`

现状是靠 `renewal-hooks/deploy/mosquitto.sh` 重启进程换证书，钩子一丢就是
「证书续了但服务还拿着旧的」。回调重载后钩子只是可选优化，不再是正确性的前提。

- [ ] **Step 1: 写失败的测试**

`server/go/internal/tlsx/reloader_test.go`:

```go
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

// writeCert 生成一张自签证书写到磁盘，返回 CN 用于断言。
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
	os.WriteFile(filepath.Join(dir, "fullchain.pem"), certPEM, 0o644)
	os.WriteFile(filepath.Join(dir, "privkey.pem"), keyPEM, 0o600)
}

func TestReloaderServesCert(t *testing.T) {
	dir := t.TempDir()
	writeCert(t, dir, "first.example")
	r, err := New(filepath.Join(dir, "fullchain.pem"), filepath.Join(dir, "privkey.pem"))
	if err != nil {
		t.Fatal(err)
	}
	c, err := r.GetCertificate(&tls.ClientHelloInfo{})
	if err != nil {
		t.Fatal(err)
	}
	leaf, _ := x509.ParseCertificate(c.Certificate[0])
	if leaf.Subject.CommonName != "first.example" {
		t.Errorf("CN = %q", leaf.Subject.CommonName)
	}
}

// certbot 续签后写了新文件，进程不重启也要换过来。
func TestReloaderPicksUpNewFile(t *testing.T) {
	dir := t.TempDir()
	writeCert(t, dir, "old.example")
	r, err := New(filepath.Join(dir, "fullchain.pem"), filepath.Join(dir, "privkey.pem"))
	if err != nil {
		t.Fatal(err)
	}
	r.GetCertificate(&tls.ClientHelloInfo{})

	time.Sleep(1100 * time.Millisecond) // 让 mtime 秒级精度能区分
	writeCert(t, dir, "new.example")

	c, err := r.GetCertificate(&tls.ClientHelloInfo{})
	if err != nil {
		t.Fatal(err)
	}
	leaf, _ := x509.ParseCertificate(c.Certificate[0])
	if leaf.Subject.CommonName != "new.example" {
		t.Errorf("CN = %q，期望 new.example——证书没有热重载", leaf.Subject.CommonName)
	}
}

// 续签中途文件可能只写了一半，这时要继续用旧证书，不能报错断连接。
func TestReloaderKeepsOldOnBrokenFile(t *testing.T) {
	dir := t.TempDir()
	writeCert(t, dir, "good.example")
	r, _ := New(filepath.Join(dir, "fullchain.pem"), filepath.Join(dir, "privkey.pem"))
	r.GetCertificate(&tls.ClientHelloInfo{})

	time.Sleep(1100 * time.Millisecond)
	os.WriteFile(filepath.Join(dir, "fullchain.pem"), []byte("half-written"), 0o644)

	c, err := r.GetCertificate(&tls.ClientHelloInfo{})
	if err != nil {
		t.Fatalf("坏文件不该让握手失败：%v", err)
	}
	leaf, _ := x509.ParseCertificate(c.Certificate[0])
	if leaf.Subject.CommonName != "good.example" {
		t.Errorf("没有退回旧证书，CN = %q", leaf.Subject.CommonName)
	}
}

func TestNewFailsOnMissingFile(t *testing.T) {
	if _, err := New("/nope/fullchain.pem", "/nope/privkey.pem"); err == nil {
		t.Error("启动时证书就不存在，应当直接失败")
	}
}
```

- [ ] **Step 2: 运行确认失败**

Run: `cd server/go && go test ./internal/tlsx/`
Expected: FAIL，`undefined: New`

- [ ] **Step 3: 实现**

`server/go/internal/tlsx/reloader.go`:

```go
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
}

// New 立即加载一次。启动时证书就不对的话直接失败——
// 起来了但没有证书，比起不来更难排查。
func New(certFile, keyFile string) (*Reloader, error) {
	r := &Reloader{certFile: certFile, keyFile: keyFile}
	if err := r.load(); err != nil {
		return nil, err
	}
	return r, nil
}

// GetCertificate 挂到 tls.Config.GetCertificate。
// 每秒最多 stat 一次——握手是热路径，不能每次都碰磁盘。
func (r *Reloader) GetCertificate(*tls.ClientHelloInfo) (*tls.Certificate, error) {
	r.mu.RLock()
	cert, checked := r.cert, r.checked
	r.mu.RUnlock()

	if time.Since(checked) >= time.Second {
		if fi, err := os.Stat(r.certFile); err == nil {
			r.mu.Lock()
			r.checked = time.Now()
			changed := fi.ModTime().After(r.mtime)
			r.mu.Unlock()
			if changed {
				// 加载失败就继续用旧的：续签中途文件可能只写了一半，
				// 这时断掉所有握手比用一张还没过期的旧证书糟得多。
				if err := r.load(); err != nil {
					slog.Warn("证书重载失败，继续使用旧证书", "err", err)
				} else {
					slog.Info("证书已热重载", "file", r.certFile)
				}
				r.mu.RLock()
				cert = r.cert
				r.mu.RUnlock()
			}
		}
	}
	return cert, nil
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
	r.cert, r.mtime, r.checked = &c, fi.ModTime(), time.Now()
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
```

- [ ] **Step 4: 运行确认通过**

Run: `cd server/go && go test -race ./internal/tlsx/ -v`
Expected: PASS（4 个用例，约 3 秒）

- [ ] **Step 5: 提交**

```bash
git add server/go/internal/tlsx
git commit -m "feat(server): 证书按 mtime 热重载，续签不再依赖 deploy hook"
```

---

## Task 14: broker — 内嵌 MQTT 与三个钩子

**Files:**
- Create: `server/go/internal/broker/broker.go`
- Create: `server/go/internal/broker/hooks.go`
- Create: `server/go/internal/broker/membership.go`
- Create: `server/go/internal/broker/hooks_test.go`

- [ ] **Step 1: 拉依赖并核对 API**

```bash
cd server/go && go get github.com/mochi-mqtt/server/v2@latest
```

**核对钩子签名再动手**——mochi 的 v2 在小版本间调整过参数：

```bash
cd server/go && go doc github.com/mochi-mqtt/server/v2.HookBase
cd server/go && go doc github.com/mochi-mqtt/server/v2.Hook
```

下面的实现按 `OnConnectAuthenticate(cl *mqtt.Client, pk packets.Packet) bool`、
`OnACLCheck(cl *mqtt.Client, topic string, write bool) bool`、
`OnPublished(cl *mqtt.Client, pk packets.Packet)` 写。签名对不上就照 `go doc`
的实际结果改，**逻辑不变**。

- [ ] **Step 2: 实现 membership 缓存**

`server/go/internal/broker/membership.go`:

```go
package broker

import (
	"sync"
	"time"

	"github.com/dayuer/stickbox/server/go/internal/store"
)

// Membership 回答「这个 dev 属于这个 user 吗」，带缓存。
//
// ACL 判定在 MQTT 热路径上，每条消息都要过——不能每次查库。
type Membership struct {
	st  *store.Store
	ttl time.Duration

	mu sync.RWMutex
	c  map[string]entry
}

type entry struct {
	devs map[string]bool
	exp  time.Time
}

func NewMembership(st *store.Store) *Membership {
	return &Membership{st: st, ttl: 5 * time.Minute, c: map[string]entry{}}
}

func (m *Membership) HasDevice(userID, dev string) bool {
	m.mu.RLock()
	e, ok := m.c[userID]
	m.mu.RUnlock()
	if ok && time.Now().Before(e.exp) {
		return e.devs[dev]
	}
	devs, err := m.st.DevicesOfUser(userID)
	if err != nil {
		return false // 查不到就拒绝，不放行
	}
	set := make(map[string]bool, len(devs))
	for _, d := range devs {
		set[d] = true
	}
	m.mu.Lock()
	m.c[userID] = entry{devs: set, exp: time.Now().Add(m.ttl)}
	m.mu.Unlock()
	return set[dev]
}

// Invalidate 在 enroll / unbind / 注销之后调用，否则新绑的设备最多要等 5 分钟
// 才能订阅成功。
func (m *Membership) Invalidate(userID string) {
	m.mu.Lock()
	delete(m.c, userID)
	m.mu.Unlock()
}
```

- [ ] **Step 3: 写钩子测试**

`server/go/internal/broker/hooks_test.go`:

```go
package broker

import (
	"encoding/json"
	"testing"

	"github.com/dayuer/stickbox/server/go/internal/device"
)

type capturedMsg struct {
	dev string
	msg device.Msg
}

type fakeRouter struct{ got []capturedMsg }

func (f *fakeRouter) Send(dev string, m device.Msg) {
	f.got = append(f.got, capturedMsg{dev, m})
}

func TestRouteStatusHeartbeat(t *testing.T) {
	r := &fakeRouter{}
	payload, _ := json.Marshal(map[string]any{
		"dev": "aaa", "job": "", "state": "ready", "bytes": 0,
		"serial": "PA",
		"prn":    map[string]any{"display": "Ready"},
	})
	routeStatus(r, "printer/aaa/status", payload)

	if len(r.got) != 1 {
		t.Fatalf("投递了 %d 条", len(r.got))
	}
	m := r.got[0].msg
	if m.Kind != device.KindHeartbeat || m.Serial != "PA" {
		t.Errorf("消息 = %+v，期望 Heartbeat + serial=PA", m)
	}
	if len(m.Printer) == 0 {
		t.Error("prn 原文没带上，/api/status 就没东西可显示")
	}
}

// done 必须带 job 字段，否则服务端要等 180 秒超时才知道这件结束了。
func TestRouteStatusDone(t *testing.T) {
	r := &fakeRouter{}
	payload := []byte(`{"dev":"aaa","job":"j1","state":"done","bytes":4096,"serial":"PA"}`)
	routeStatus(r, "printer/aaa/status", payload)

	m := r.got[0].msg
	if m.Kind != device.KindJobDone || m.JobID != "j1" || m.Bytes != 4096 {
		t.Errorf("消息 = %+v", m)
	}
}

func TestRouteStatusFailedCarriesError(t *testing.T) {
	r := &fakeRouter{}
	routeStatus(r, "printer/aaa/status",
		[]byte(`{"dev":"aaa","job":"j1","state":"failed","err":"USB 超时"}`))
	m := r.got[0].msg
	if m.Kind != device.KindJobFailed || m.Err != "USB 超时" {
		t.Errorf("消息 = %+v", m)
	}
}

// LWT：设备掉线时 broker 代发，serial 要清空。
func TestRouteStatusOffline(t *testing.T) {
	r := &fakeRouter{}
	routeStatus(r, "printer/aaa/status", []byte(`{"dev":"aaa","state":"offline"}`))
	m := r.got[0].msg
	if m.Kind != device.KindHeartbeat || m.Serial != "" {
		t.Errorf("消息 = %+v，offline 时 serial 应为空", m)
	}
}

// 畸形 JSON 不能让 broker 崩，也不能投出垃圾消息。
func TestRouteStatusIgnoresGarbage(t *testing.T) {
	r := &fakeRouter{}
	routeStatus(r, "printer/aaa/status", []byte("not json at all"))
	if len(r.got) != 0 {
		t.Errorf("畸形 payload 投出了 %d 条消息", len(r.got))
	}
}

// topic 里的 dev 是权威，payload 里的 dev 是设备自报的，不能信。
func TestRouteStatusTrustsTopicNotPayload(t *testing.T) {
	r := &fakeRouter{}
	routeStatus(r, "printer/aaa/status", []byte(`{"dev":"bbb","state":"ready"}`))
	if len(r.got) != 1 || r.got[0].dev != "aaa" {
		t.Errorf("路由到了 %+v，必须以 topic 为准", r.got)
	}
}
```

- [ ] **Step 4: 实现钩子**

`server/go/internal/broker/hooks.go`:

```go
package broker

import (
	"encoding/json"
	"log/slog"
	"strings"

	mqtt "github.com/mochi-mqtt/server/v2"
	"github.com/mochi-mqtt/server/v2/packets"

	"github.com/dayuer/stickbox/server/go/internal/auth"
	"github.com/dayuer/stickbox/server/go/internal/device"
)

// Router 是 broker 对 registry 的窄依赖。
type Router interface {
	Send(dev string, m device.Msg)
}

type Hook struct {
	mqtt.HookBase
	v      *auth.Verifier
	mem    *Membership
	router Router
	// ids 记录每个连接校验通过的身份，ACL 判定时直接取，不重复跑 argon2。
	ids *idTable
}

func (h *Hook) ID() string { return "stickbox-auth" }

func (h *Hook) Provides(b byte) bool {
	switch b {
	case mqtt.OnConnectAuthenticate, mqtt.OnACLCheck, mqtt.OnPublished, mqtt.OnDisconnect:
		return true
	}
	return false
}

func (h *Hook) OnConnectAuthenticate(cl *mqtt.Client, pk packets.Packet) bool {
	user := string(pk.Connect.Username)
	pass := string(pk.Connect.Password)
	id, err := h.v.Verify(pass)
	if err != nil {
		slog.Warn("MQTT 认证失败", "username", user, "remote", cl.Net.Remote)
		return false
	}
	// device 角色的 username 必须等于它自己的 dev——防止拿 A 的密钥冒充 B 连接。
	if id.Role == auth.RoleDevice && id.Dev != user {
		slog.Warn("MQTT username 与密钥不符", "username", user, "key_dev", id.Dev)
		return false
	}
	h.ids.put(cl.ID, id)
	return true
}

func (h *Hook) OnACLCheck(cl *mqtt.Client, topic string, write bool) bool {
	id, ok := h.ids.get(cl.ID)
	if !ok {
		return false
	}
	return auth.ACLAllowed(id, topic, write, h.mem)
}

func (h *Hook) OnDisconnect(cl *mqtt.Client, err error, expire bool) {
	h.ids.drop(cl.ID)
}

func (h *Hook) OnPublished(cl *mqtt.Client, pk packets.Packet) {
	if strings.HasSuffix(pk.TopicName, "/status") {
		routeStatus(h.router, pk.TopicName, pk.Payload)
	}
}

// statusPayload 是设备心跳的结构。字段定义见 docs/API-cloud-print.md 第 3.6 节。
type statusPayload struct {
	Job    string          `json:"job"`
	State  string          `json:"state"`
	Bytes  int64           `json:"bytes"`
	Err    string          `json:"err"`
	Serial string          `json:"serial"`
	Prn    json.RawMessage `json:"prn"`
}

// routeStatus 把一条 status 消息翻译成 actor 消息。
//
// dev 一律从 topic 取，不用 payload 里的——payload 是设备自报的，
// topic 是经过 ACL 校验的，只有后者可信。
func routeStatus(r Router, topic string, payload []byte) {
	parts := strings.Split(topic, "/")
	if len(parts) != 3 {
		return
	}
	dev := parts[1]

	var p statusPayload
	if err := json.Unmarshal(payload, &p); err != nil {
		slog.Warn("status payload 非法 JSON", "dev", dev)
		return
	}

	m := device.Msg{Serial: p.Serial, Printer: p.Prn, JobID: p.Job,
		Bytes: p.Bytes, Err: p.Err}
	switch p.State {
	case "done":
		m.Kind = device.KindJobDone
	case "failed":
		m.Kind = device.KindJobFailed
	case "downloading":
		m.Kind = device.KindDownloading
	case "offline":
		m.Kind = device.KindHeartbeat
		m.Serial = "" // 掉线了就没有打印机
	default:
		m.Kind = device.KindHeartbeat
	}
	r.Send(dev, m)
}
```

`server/go/internal/broker/broker.go`:

```go
// Package broker 把 MQTT broker 塞进本进程。
//
// 内嵌之后派发一件作业是一次内存中的函数调用，不再经网络。
// 认证和 ACL 也跟 HTTP 共用同一套密钥，不用维护第二份配置。
package broker

import (
	"crypto/tls"
	"encoding/json"
	"fmt"
	"sync"

	mqtt "github.com/mochi-mqtt/server/v2"
	"github.com/mochi-mqtt/server/v2/listeners"

	"github.com/dayuer/stickbox/server/go/internal/auth"
)

type idTable struct {
	mu sync.RWMutex
	m  map[string]auth.Identity
}

func newIDTable() *idTable { return &idTable{m: map[string]auth.Identity{}} }

func (t *idTable) put(cid string, id auth.Identity) {
	t.mu.Lock()
	t.m[cid] = id
	t.mu.Unlock()
}
func (t *idTable) get(cid string) (auth.Identity, bool) {
	t.mu.RLock()
	defer t.mu.RUnlock()
	id, ok := t.m[cid]
	return id, ok
}
func (t *idTable) drop(cid string) {
	t.mu.Lock()
	delete(t.m, cid)
	t.mu.Unlock()
}

type Broker struct {
	srv *mqtt.Server
	mem *Membership
}

func New(addr string, tlsCfg *tls.Config, v *auth.Verifier, mem *Membership, r Router) (*Broker, error) {
	srv := mqtt.New(nil)
	h := &Hook{v: v, mem: mem, router: r, ids: newIDTable()}
	if err := srv.AddHook(h, nil); err != nil {
		return nil, err
	}
	l := listeners.NewTCP(listeners.Config{
		ID: "mqtt-tls", Address: addr, TLSConfig: tlsCfg,
	})
	if err := srv.AddListener(l); err != nil {
		return nil, err
	}
	return &Broker{srv: srv, mem: mem}, nil
}

func (b *Broker) Serve() error { return b.srv.Serve() }
func (b *Broker) Close() error { return b.srv.Close() }

// PublishJob 实现 device.Publisher。信令只有几十字节。
func (b *Broker) PublishJob(dev, jobID string, size int64) error {
	payload, err := json.Marshal(map[string]any{"id": jobID, "size": size})
	if err != nil {
		return err
	}
	return b.srv.Publish(fmt.Sprintf("printer/%s/job", dev), payload, false, 1)
}

// PublishProfile 下发 USB 层怪癖档案。retain=1，设备重连就能拿到。
func (b *Broker) PublishProfile(dev string, profile []byte) error {
	return b.srv.Publish(fmt.Sprintf("printer/%s/profile", dev), profile, true, 1)
}
```

- [ ] **Step 5: 运行确认通过**

Run: `cd server/go && go test ./internal/broker/ -v`
Expected: PASS，6 个路由用例

- [ ] **Step 6: 提交**

```bash
git add server/go/internal/broker server/go/go.mod server/go/go.sum
git commit -m "feat(server): 内嵌 MQTT broker、认证/ACL/路由三个钩子"
```

---

## Task 15: httpapi — 鉴权中间件与认证端点

**Files:**
- Create: `server/go/internal/httpapi/api.go`
- Create: `server/go/internal/httpapi/auth_endpoints.go`
- Create: `server/go/internal/httpapi/auth_endpoints_test.go`

行为以 `docs/API-cloud-print.md` 为准，**不一致时以文档为准**——固件和 App
是照那份写的。

- [ ] **Step 1: 写失败的测试**

`server/go/internal/httpapi/auth_endpoints_test.go`:

```go
package httpapi

import (
	"context"
	"encoding/json"
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"
)

type fakeSender struct{ last string }

func (f *fakeSender) Send(ctx context.Context, phone, code string) error {
	f.last = code
	return nil
}

func TestSMSThenVerifyCreatesUser(t *testing.T) {
	h, dep := newTestAPI(t)

	rr := post(h, "/api/auth/sms", `{"phone":"13800008888"}`, nil)
	if rr.Code != 200 {
		t.Fatalf("发送验证码 = %d %s", rr.Code, rr.Body)
	}

	rr = post(h, "/api/auth/verify",
		`{"phone":"13800008888","code":"`+dep.sender.last+`","device":"iPhone 15"}`, nil)
	if rr.Code != 200 {
		t.Fatalf("校验 = %d %s", rr.Code, rr.Body)
	}
	var out struct {
		Token  string `json:"token"`
		UserID string `json:"user_id"`
		Tail   string `json:"phone_tail"`
	}
	json.Unmarshal(rr.Body.Bytes(), &out)
	if out.Token == "" || out.UserID == "" {
		t.Fatalf("响应缺字段：%s", rr.Body)
	}
	if out.Tail != "8888" {
		t.Errorf("phone_tail = %q", out.Tail)
	}

	// 完整号码必须能解回来——客服核验、换号迁移要用
	enc, ok, _ := dep.store.GetPhone(out.UserID)
	if !ok {
		t.Fatal("完整号码没落库")
	}
	phone, err := dep.phone.Open(enc)
	if err != nil || phone != "13800008888" {
		t.Errorf("解出 %q err=%v", phone, err)
	}
}

func TestVerifyRejectsWrongCode(t *testing.T) {
	h, _ := newTestAPI(t)
	post(h, "/api/auth/sms", `{"phone":"13800008888"}`, nil)
	rr := post(h, "/api/auth/verify", `{"phone":"13800008888","code":"000000"}`, nil)
	if rr.Code != 401 {
		t.Errorf("错误验证码 = %d，期望 401", rr.Code)
	}
}

func TestSMSRejectsBadPhone(t *testing.T) {
	h, _ := newTestAPI(t)
	rr := post(h, "/api/auth/sms", `{"phone":"12345"}`, nil)
	if rr.Code != 400 {
		t.Errorf("非法号码 = %d，期望 400", rr.Code)
	}
}

// 60 秒内重发要被挡住，且不能真发第二条短信。
func TestSMSRateLimited(t *testing.T) {
	h, dep := newTestAPI(t)
	post(h, "/api/auth/sms", `{"phone":"13800008888"}`, nil)
	first := dep.sender.last
	rr := post(h, "/api/auth/sms", `{"phone":"13800008888"}`, nil)
	if rr.Code != 429 {
		t.Errorf("60 秒内重发 = %d，期望 429", rr.Code)
	}
	if dep.sender.last != first {
		t.Error("被限流了却还是发了短信——在烧钱")
	}
}

func TestRequireAuthRejectsMissingAndBadToken(t *testing.T) {
	h, _ := newTestAPI(t)
	for _, hdr := range []map[string]string{
		nil,
		{"Authorization": "Bearer garbage"},
		{"Authorization": "not-bearer xxx"},
	} {
		rr := get(h, "/api/status", hdr)
		if rr.Code != 401 {
			t.Errorf("头 %v → %d，期望 401", hdr, rr.Code)
		}
	}
}

// 401 不能区分「设备不存在」和「密钥错」——不给探测者提供信息。
func TestUnauthorizedBodyIsUniform(t *testing.T) {
	h, _ := newTestAPI(t)
	a := get(h, "/api/status", map[string]string{"Authorization": "Bearer aaaaaaaaaaaa.xxx"})
	b := get(h, "/api/status", map[string]string{"Authorization": "Bearer bbbbbbbbbbbb.yyy"})
	if a.Body.String() != b.Body.String() {
		t.Errorf("401 响应体不一致，泄露了信息：%q vs %q", a.Body, b.Body)
	}
}

func TestLogoutRevokesToken(t *testing.T) {
	h, dep := newTestAPI(t)
	token := loginFixture(t, h, dep, "13800008888")

	rr := post(h, "/api/auth/logout", `{}`, bearer(token))
	if rr.Code != 200 {
		t.Fatalf("登出 = %d %s", rr.Code, rr.Body)
	}
	if rr := get(h, "/api/status", bearer(token)); rr.Code != 401 {
		t.Errorf("登出后 token 仍可用（%d）", rr.Code)
	}
}

// 注销要把个人数据清干净，但不碰按打印机序列号存的适配档案。
func TestAccountDeleteWipesPersonalData(t *testing.T) {
	h, dep := newTestAPI(t)
	token := loginFixture(t, h, dep, "13800008888")

	rr := post(h, "/api/account/delete", `{}`, bearer(token))
	if rr.Code != 200 {
		t.Fatalf("注销 = %d %s", rr.Code, rr.Body)
	}
	if _, ok, _ := dep.store.UserByHMAC(dep.phone.HMAC("13800008888")); ok {
		t.Error("users 行还在")
	}
	if rr := get(h, "/api/status", bearer(token)); rr.Code != 401 {
		t.Error("注销后 token 仍可用")
	}
}

// —— 测试辅助 ——

func bearer(tok string) map[string]string {
	return map[string]string{"Authorization": "Bearer " + tok}
}

func post(h http.Handler, path, body string, hdr map[string]string) *httptest.ResponseRecorder {
	req := httptest.NewRequest("POST", path, strings.NewReader(body))
	req.Header.Set("Content-Type", "application/json")
	for k, v := range hdr {
		req.Header.Set(k, v)
	}
	rr := httptest.NewRecorder()
	h.ServeHTTP(rr, req)
	return rr
}

func get(h http.Handler, path string, hdr map[string]string) *httptest.ResponseRecorder {
	req := httptest.NewRequest("GET", path, nil)
	for k, v := range hdr {
		req.Header.Set(k, v)
	}
	rr := httptest.NewRecorder()
	h.ServeHTTP(rr, req)
	return rr
}

func loginFixture(t *testing.T, h http.Handler, dep *testDeps, phone string) string {
	t.Helper()
	post(h, "/api/auth/sms", `{"phone":"`+phone+`"}`, nil)
	rr := post(h, "/api/auth/verify",
		`{"phone":"`+phone+`","code":"`+dep.sender.last+`","device":"test"}`, nil)
	var out struct {
		Token string `json:"token"`
	}
	json.Unmarshal(rr.Body.Bytes(), &out)
	if out.Token == "" {
		t.Fatalf("登录失败：%d %s", rr.Code, rr.Body)
	}
	return out.Token
}
```

- [ ] **Step 2: 实现路由与中间件**

`server/go/internal/httpapi/api.go`:

```go
// Package httpapi 是纯 JSON API，没有网页。
//
// 行为以 docs/API-cloud-print.md 为准——固件和 App 是照那份写的，
// 不一致时改这里，不是改文档。
package httpapi

import (
	"encoding/json"
	"log/slog"
	"net"
	"net/http"
	"strings"

	"github.com/dayuer/stickbox/server/go/internal/auth"
	"github.com/dayuer/stickbox/server/go/internal/config"
	"github.com/dayuer/stickbox/server/go/internal/device"
	"github.com/dayuer/stickbox/server/go/internal/registry"
	"github.com/dayuer/stickbox/server/go/internal/store"
)

type Publisher interface {
	PublishProfile(dev string, profile []byte) error
}

type Invalidator interface {
	Invalidate(userID string)
}

type API struct {
	cfg   *config.Config
	store *store.Store
	v     *auth.Verifier
	phone *auth.PhoneBox
	sms   *auth.SMS
	reg   *registry.Registry
	pub   Publisher
	mem   Invalidator
}

func New(cfg *config.Config, st *store.Store, v *auth.Verifier, pb *auth.PhoneBox,
	sms *auth.SMS, reg *registry.Registry, pub Publisher, mem Invalidator) *API {
	return &API{cfg: cfg, store: st, v: v, phone: pb, sms: sms,
		reg: reg, pub: pub, mem: mem}
}

func (a *API) Handler() http.Handler {
	mux := http.NewServeMux()

	// 不需要身份的
	mux.HandleFunc("POST /api/auth/sms", a.handleSMS)
	mux.HandleFunc("POST /api/auth/verify", a.handleVerify)

	// app 角色
	mux.Handle("POST /api/auth/logout", a.requireApp(a.handleLogout))
	mux.Handle("POST /api/account/delete", a.requireApp(a.handleAccountDelete))
	mux.Handle("POST /api/device/enroll", a.requireApp(a.handleEnroll))
	mux.Handle("POST /api/device/{dev}/unbind", a.requireApp(a.handleUnbind))
	mux.Handle("POST /api/print", a.requireApp(a.handlePrint))
	mux.Handle("GET /api/device/{dev}/render-profile", a.requireApp(a.handleRenderProfile))
	mux.Handle("GET /api/device/{dev}/printers", a.requireApp(a.handlePrinters))

	// device 角色
	mux.Handle("GET /api/job/{id}/data", a.requireDevice(a.handleJobData))
	mux.Handle("POST /api/device/{dev}/ident", a.requireDevice(a.handleIdent))

	// 两种角色都行
	mux.Handle("GET /api/status", a.requireAny(a.handleStatus))

	return logging(mux)
}

type handlerFn func(http.ResponseWriter, *http.Request, auth.Identity)

func (a *API) requireApp(fn handlerFn) http.Handler    { return a.guard(fn, auth.RoleApp) }
func (a *API) requireDevice(fn handlerFn) http.Handler { return a.guard(fn, auth.RoleDevice) }
func (a *API) requireAny(fn handlerFn) http.Handler    { return a.guard(fn, "") }

func (a *API) guard(fn handlerFn, want auth.Role) http.Handler {
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		tok, ok := bearerToken(r)
		if !ok {
			unauthorized(w)
			return
		}
		id, err := a.v.Verify(tok)
		if err != nil {
			// 刻意不区分「不存在」和「密钥错」——不给探测者提供信息。
			unauthorized(w)
			return
		}
		if want != "" && id.Role != want {
			unauthorized(w)
			return
		}
		fn(w, r, id)
	})
}

func bearerToken(r *http.Request) (string, bool) {
	h := r.Header.Get("Authorization")
	const p = "Bearer "
	if len(h) <= len(p) || !strings.EqualFold(h[:len(p)], p) {
		return "", false
	}
	return h[len(p):], true
}

func unauthorized(w http.ResponseWriter) {
	writeJSON(w, http.StatusUnauthorized, map[string]string{"e": "unauthorized"})
}

func writeJSON(w http.ResponseWriter, code int, v any) {
	w.Header().Set("Content-Type", "application/json; charset=utf-8")
	w.WriteHeader(code)
	json.NewEncoder(w).Encode(v)
}

func fail(w http.ResponseWriter, code int, e, detail string) {
	m := map[string]string{"e": e}
	if detail != "" {
		m["detail"] = detail
	}
	writeJSON(w, code, m)
}

func clientIP(r *http.Request) string {
	host, _, err := net.SplitHostPort(r.RemoteAddr)
	if err != nil {
		return r.RemoteAddr
	}
	return host
}

func logging(next http.Handler) http.Handler {
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		slog.Debug("http", "method", r.Method, "path", r.URL.Path)
		next.ServeHTTP(w, r)
	})
}

// devMsg 是给 registry 投递消息的快捷方式。
func (a *API) devMsg(dev string, m device.Msg) { a.reg.Send(dev, m) }
```

- [ ] **Step 3: 实现认证端点**

`server/go/internal/httpapi/auth_endpoints.go`:

```go
package httpapi

import (
	"encoding/json"
	"errors"
	"net/http"
	"os"
	"path/filepath"

	"github.com/dayuer/stickbox/server/go/internal/auth"
)

func (a *API) handleSMS(w http.ResponseWriter, r *http.Request) {
	var in struct {
		Phone string `json:"phone"`
	}
	if err := json.NewDecoder(http.MaxBytesReader(w, r.Body, 1<<12)).Decode(&in); err != nil {
		fail(w, 400, "bad request", "")
		return
	}
	phone, err := auth.NormalizePhone(in.Phone)
	if err != nil {
		fail(w, 400, "bad phone", "只支持中国大陆手机号")
		return
	}
	err = a.sms.Issue(r.Context(), phone, a.phone.HMAC(phone), clientIP(r))
	switch {
	case err == nil:
		writeJSON(w, 200, map[string]any{"ok": 1, "ttl": 300})
	case errors.Is(err, auth.ErrTooFrequent), errors.Is(err, auth.ErrDailyCap),
		errors.Is(err, auth.ErrIPCap):
		// 429 而不是 400：这是限流，不是请求本身有问题。
		fail(w, 429, "rate limited", err.Error())
	default:
		fail(w, 500, "sms failed", "")
	}
}

func (a *API) handleVerify(w http.ResponseWriter, r *http.Request) {
	var in struct {
		Phone, Code, Device string
	}
	if err := json.NewDecoder(http.MaxBytesReader(w, r.Body, 1<<12)).Decode(&in); err != nil {
		fail(w, 400, "bad request", "")
		return
	}
	phone, err := auth.NormalizePhone(in.Phone)
	if err != nil {
		fail(w, 400, "bad phone", "")
		return
	}
	hmac := a.phone.HMAC(phone)
	if err := a.sms.Verify(hmac, in.Code); err != nil {
		fail(w, 401, "bad code", "")
		return
	}

	u, created, err := a.store.UpsertUser(hmac, a.phone.Tail(phone))
	if err != nil {
		fail(w, 500, "server error", "")
		return
	}
	if created {
		// 完整号码单独落库，加密存储。热路径永远不碰这张表。
		enc, err := a.phone.Seal(phone)
		if err != nil {
			fail(w, 500, "server error", "")
			return
		}
		if err := a.store.PutPhone(u.ID, enc); err != nil {
			fail(w, 500, "server error", "")
			return
		}
	}
	name := in.Device
	if name == "" {
		name = "unknown"
	}
	token, err := auth.IssueSession(a.store, u.ID, name)
	if err != nil {
		fail(w, 500, "server error", "")
		return
	}
	writeJSON(w, 200, map[string]any{
		"token": token, "user_id": u.ID, "phone_tail": u.PhoneTail, "new_user": created,
	})
}

func (a *API) handleLogout(w http.ResponseWriter, r *http.Request, id auth.Identity) {
	var err error
	if r.URL.Query().Get("all") == "1" {
		err = auth.RevokeAllSessions(a.store, a.v, id.UserID)
	} else {
		err = auth.RevokeSession(a.store, a.v, id.KeyID)
	}
	if err != nil {
		fail(w, 500, "server error", "")
		return
	}
	writeJSON(w, 200, map[string]int{"ok": 1})
}

// handleAccountDelete 立即执行，不设冷静期——那需要额外的状态机和定时任务，
// 而这里没有值得挽回的东西。
func (a *API) handleAccountDelete(w http.ResponseWriter, r *http.Request, id auth.Identity) {
	jobIDs, err := a.store.DeleteUser(id.UserID)
	if err != nil {
		fail(w, 500, "server error", "")
		return
	}
	for _, jid := range jobIDs {
		os.Remove(filepath.Join(a.cfg.JobsDir(), jid+".urf"))
	}
	a.v.InvalidateAll() // 该用户的所有 token 都失效了
	a.mem.Invalidate(id.UserID)
	writeJSON(w, 200, map[string]any{"ok": 1, "jobs_deleted": len(jobIDs)})
}
```

- [ ] **Step 4: 补 Verifier 的 InvalidateAll**

追加到 `server/go/internal/auth/verifier.go`:

```go
// InvalidateAll 清空整张校验缓存。注销账号时用——那一刻有多把 token 同时失效，
// 逐个 Invalidate 需要先把 key_id 全查出来，不如整表清掉重建。
// 代价是所有在线连接下一次校验要重跑一遍 argon2，注销是极低频操作，可以接受。
func (v *Verifier) InvalidateAll() {
	v.mu.Lock()
	v.c = map[string]cacheEntry{}
	v.mu.Unlock()
}
```

- [ ] **Step 5: 写测试装配（`newTestAPI`）**

`server/go/internal/httpapi/testdeps_test.go`:

```go
package httpapi

import (
	"net/http"
	"testing"
	"time"

	"github.com/dayuer/stickbox/server/go/internal/auth"
	"github.com/dayuer/stickbox/server/go/internal/config"
	"github.com/dayuer/stickbox/server/go/internal/device"
	"github.com/dayuer/stickbox/server/go/internal/registry"
	"github.com/dayuer/stickbox/server/go/internal/store"
)

type testDeps struct {
	store  *store.Store
	phone  *auth.PhoneBox
	sender *fakeSender
	reg    *registry.Registry
	clock  time.Time
}

type nopPub struct{ profiles map[string][]byte }

func (p *nopPub) PublishProfile(dev string, b []byte) error {
	if p.profiles == nil {
		p.profiles = map[string][]byte{}
	}
	p.profiles[dev] = b
	return nil
}
func (p *nopPub) PublishJob(dev, job string, size int64) error { return nil }

type nopMem struct{}

func (nopMem) Invalidate(string) {}

func newTestAPI(t *testing.T) (http.Handler, *testDeps) {
	t.Helper()
	dir := t.TempDir()
	st, err := store.Open(dir + "/jobs.db")
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { st.Close() })

	pb, err := auth.NewPhoneBox("test-pepper",
		"000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f")
	if err != nil {
		t.Fatal(err)
	}
	dep := &testDeps{store: st, phone: pb, sender: &fakeSender{},
		clock: time.Unix(1_000_000, 0)}
	sms := auth.NewSMS(st, dep.sender, func() time.Time { return dep.clock })
	pub := &nopPub{}
	reg := registry.New(st, pub, device.Options{
		JobTimeout: 180 * time.Second, IdleTimeout: time.Hour,
	})
	t.Cleanup(reg.Shutdown)
	dep.reg = reg

	cfg := &config.Config{Root: dir, JobRetainHours: 24, JobRetainPerDevice: 50}
	if err := os.MkdirAll(cfg.JobsDir(), 0o755); err != nil {
		t.Fatal(err)
	}
	api := New(cfg, st, auth.NewVerifier(st), pb, sms, reg, pub, nopMem{})
	return api.Handler(), dep
}
```

（顶部需要 `"os"`，写的时候补上 import。）

- [ ] **Step 6: 运行确认通过**

Run: `cd server/go && go test ./internal/httpapi/ -v`
Expected: PASS，8 个用例

- [ ] **Step 7: 提交**

```bash
git add server/go/internal/httpapi server/go/internal/auth/verifier.go
git commit -m "feat(server): HTTP 鉴权中间件与短信登录端点"
```

---

## Task 16: httpapi — 设备与作业端点

**Files:**
- Create: `server/go/internal/httpapi/device_endpoints.go`
- Create: `server/go/internal/httpapi/job_endpoints.go`
- Create: `server/go/internal/httpapi/device_endpoints_test.go`
- Create: `server/go/internal/httpapi/job_endpoints_test.go`

- [ ] **Step 1: 写 enroll / unbind 测试**

`server/go/internal/httpapi/device_endpoints_test.go`:

```go
package httpapi

import (
	"encoding/json"
	"testing"
)

func enroll(t *testing.T, h interface{ ServeHTTP(a, b any) }, token, dev string) string { return "" }

func TestEnrollIssuesDeviceKey(t *testing.T) {
	h, dep := newTestAPI(t)
	token := loginFixture(t, h, dep, "13800008888")

	rr := post(h, "/api/device/enroll", `{"dev":"f412fa87c9e0","name":"工位"}`, bearer(token))
	if rr.Code != 200 {
		t.Fatalf("enroll = %d %s", rr.Code, rr.Body)
	}
	var out struct{ DeviceKey string `json:"device_key"` }
	json.Unmarshal(rr.Body.Bytes(), &out)
	if out.DeviceKey == "" {
		t.Fatalf("没返回设备密钥：%s", rr.Body)
	}
	if owner, ok, _ := dep.store.OwnerOfDevice("f412fa87c9e0"); !ok || owner == "" {
		t.Error("设备没绑到用户上")
	}
}

// 抢绑防护：别人的设备不能被绑走，否则是条现成的 DoS。
func TestEnrollRejectsOtherUsersDevice(t *testing.T) {
	h, dep := newTestAPI(t)
	tokenA := loginFixture(t, h, dep, "13800008888")
	post(h, "/api/device/enroll", `{"dev":"f412fa87c9e0"}`, bearer(tokenA))

	dep.clock = dep.clock.Add(2 * 60e9) // 越过 60 秒重发闸
	tokenB := loginFixture(t, h, dep, "13900009999")
	rr := post(h, "/api/device/enroll", `{"dev":"f412fa87c9e0"}`, bearer(tokenB))
	if rr.Code != 409 {
		t.Errorf("抢绑 = %d，期望 409", rr.Code)
	}
}

// 重复 enroll 自己的设备 = 重置：旧密钥吊销，新密钥生效。
func TestEnrollTwiceIsReset(t *testing.T) {
	h, dep := newTestAPI(t)
	token := loginFixture(t, h, dep, "13800008888")

	rr := post(h, "/api/device/enroll", `{"dev":"f412fa87c9e0"}`, bearer(token))
	var first struct{ DeviceKey string `json:"device_key"` }
	json.Unmarshal(rr.Body.Bytes(), &first)

	rr = post(h, "/api/device/enroll", `{"dev":"f412fa87c9e0"}`, bearer(token))
	if rr.Code != 200 {
		t.Fatalf("重置 = %d %s", rr.Code, rr.Body)
	}
	var second struct{ DeviceKey string `json:"device_key"` }
	json.Unmarshal(rr.Body.Bytes(), &second)
	if second.DeviceKey == first.DeviceKey {
		t.Error("重置后密钥没变")
	}
	if _, err := dep.verifier().Verify(first.DeviceKey); err == nil {
		t.Error("旧设备密钥仍可用——重置没吊销它")
	}
}

func TestUnbindReleasesDevice(t *testing.T) {
	h, dep := newTestAPI(t)
	token := loginFixture(t, h, dep, "13800008888")
	post(h, "/api/device/enroll", `{"dev":"f412fa87c9e0"}`, bearer(token))

	if rr := post(h, "/api/device/f412fa87c9e0/unbind", `{}`, bearer(token)); rr.Code != 200 {
		t.Fatalf("解绑 = %d %s", rr.Code, rr.Body)
	}
	if _, ok, _ := dep.store.OwnerOfDevice("f412fa87c9e0"); ok {
		t.Error("解绑后设备仍有归属")
	}
}
```

在 `testdeps_test.go` 的 `testDeps` 上补一个访问器（实现时加）：

```go
func (d *testDeps) verifier() *auth.Verifier { return d.v }
```

并在 `newTestAPI` 里把 `auth.NewVerifier(st)` 的结果存进 `dep.v`，
供上面的用例复用同一个缓存实例——**用新建的 Verifier 会因为缓存是空的而测不出
吊销是否生效**。

- [ ] **Step 2: 实现 enroll / unbind**

`server/go/internal/httpapi/device_endpoints.go`:

```go
package httpapi

import (
	"encoding/json"
	"net/http"
	"regexp"

	"github.com/dayuer/stickbox/server/go/internal/auth"
)

var reDev = regexp.MustCompile(`^[0-9a-f]{12}$`)

// handleEnroll 为一台桥签发 device 密钥。
//
// 「重置」不另设机制：给一个已属于本用户的 dev 重新 enroll，就是重置。
// 用户按住按键恢复出厂时设备清了 NVS，服务端全程不知情——落在 enroll 上
// 才不需要任何额外协调。
func (a *API) handleEnroll(w http.ResponseWriter, r *http.Request, id auth.Identity) {
	var in struct{ Dev, Name string }
	if err := json.NewDecoder(http.MaxBytesReader(w, r.Body, 1<<12)).Decode(&in); err != nil {
		fail(w, 400, "bad request", "")
		return
	}
	if !reDev.MatchString(in.Dev) {
		fail(w, 400, "bad dev", "dev 必须是 12 位小写十六进制的 MAC")
		return
	}

	owner, bound, err := a.store.OwnerOfDevice(in.Dev)
	if err != nil {
		fail(w, 500, "server error", "")
		return
	}
	// 抢绑防护：服务端无法验证 App 是否真的物理接触了设备，
	// 不设防的话任何人拿别人的 MAC 就能重置别人的设备。
	if bound && owner != id.UserID {
		fail(w, 409, "device bound", "该设备已绑定到其他账号，需原持有人先解绑")
		return
	}
	if bound {
		if err := auth.RevokeDeviceKeys(a.store, a.v, in.Dev); err != nil {
			fail(w, 500, "server error", "")
			return
		}
	}
	token, err := auth.IssueDeviceKey(a.store, id.UserID, in.Dev, in.Name)
	if err != nil {
		fail(w, 500, "server error", "")
		return
	}
	a.mem.Invalidate(id.UserID) // 否则新绑的设备最多要等 5 分钟才能订阅成功
	writeJSON(w, 200, map[string]any{
		"device_key": token, "dev": in.Dev, "reset": bound,
	})
}

func (a *API) handleUnbind(w http.ResponseWriter, r *http.Request, id auth.Identity) {
	dev := r.PathValue("dev")
	owner, bound, err := a.store.OwnerOfDevice(dev)
	if err != nil {
		fail(w, 500, "server error", "")
		return
	}
	if !bound {
		writeJSON(w, 200, map[string]int{"ok": 1}) // 幂等
		return
	}
	if owner != id.UserID {
		fail(w, 403, "forbidden", "")
		return
	}
	if err := auth.RevokeDeviceKeys(a.store, a.v, dev); err != nil {
		fail(w, 500, "server error", "")
		return
	}
	a.mem.Invalidate(id.UserID)
	writeJSON(w, 200, map[string]int{"ok": 1})
}
```

- [ ] **Step 3: 写作业端点测试**

`server/go/internal/httpapi/job_endpoints_test.go`:

```go
package httpapi

import (
	"bytes"
	"encoding/binary"
	"encoding/json"
	"net/http"
	"net/http/httptest"
	"testing"
)

func urfBytes(pages uint32, w, h uint32) []byte {
	b := make([]byte, 12+32+100)
	copy(b, "UNIRAST\x00")
	binary.BigEndian.PutUint32(b[8:12], pages)
	binary.BigEndian.PutUint32(b[24:28], w)
	binary.BigEndian.PutUint32(b[28:32], h)
	return b
}

func upload(h http.Handler, token, dev, serial, ct string, body []byte) *httptest.ResponseRecorder {
	req := httptest.NewRequest("POST", "/api/print", bytes.NewReader(body))
	req.Header.Set("Content-Type", ct)
	req.Header.Set("X-Device", dev)
	req.Header.Set("X-Printer-Serial", serial)
	req.Header.Set("Authorization", "Bearer "+token)
	rr := httptest.NewRecorder()
	h.ServeHTTP(rr, req)
	return rr
}

func TestPrintAcceptsURFAndQueues(t *testing.T) {
	h, dep := newTestAPI(t)
	token := loginFixture(t, h, dep, "13800008888")
	post(h, "/api/device/enroll", `{"dev":"f412fa87c9e0"}`, bearer(token))

	rr := upload(h, token, "f412fa87c9e0", "PA", "image/urf", urfBytes(2, 4962, 7014))
	if rr.Code != 200 {
		t.Fatalf("上传 = %d %s", rr.Code, rr.Body)
	}
	var out struct {
		Job   string `json:"job"`
		State string `json:"state"`
	}
	json.Unmarshal(rr.Body.Bytes(), &out)
	if out.State != "queued" {
		t.Errorf("state = %q，期望 queued（服务端不渲染，没有 rendering 态）", out.State)
	}
	j, ok, _ := dep.store.GetJob(out.Job)
	if !ok || j.Serial != "PA" {
		t.Errorf("作业没绑打印机：%+v", j)
	}
}

// 最要命的一条：把 PDF 当 URF 传，必须在入口挡住。
func TestPrintRejectsPDFClaimingURF(t *testing.T) {
	h, dep := newTestAPI(t)
	token := loginFixture(t, h, dep, "13800008888")
	post(h, "/api/device/enroll", `{"dev":"f412fa87c9e0"}`, bearer(token))

	rr := upload(h, token, "f412fa87c9e0", "PA", "image/urf", []byte("%PDF-1.7\nrest of pdf"))
	if rr.Code != 400 {
		t.Fatalf("PDF 冒充 URF = %d，期望 400——用户会收到几十张乱码纸", rr.Code)
	}
}

func TestPrintRejectsWrongContentType(t *testing.T) {
	h, dep := newTestAPI(t)
	token := loginFixture(t, h, dep, "13800008888")
	post(h, "/api/device/enroll", `{"dev":"f412fa87c9e0"}`, bearer(token))

	rr := upload(h, token, "f412fa87c9e0", "PA", "application/pdf", urfBytes(1, 4962, 7014))
	if rr.Code != 415 {
		t.Errorf("PDF 的 Content-Type = %d，期望 415", rr.Code)
	}
}

func TestPrintRequiresPrinterSerial(t *testing.T) {
	h, dep := newTestAPI(t)
	token := loginFixture(t, h, dep, "13800008888")
	post(h, "/api/device/enroll", `{"dev":"f412fa87c9e0"}`, bearer(token))

	rr := upload(h, token, "f412fa87c9e0", "", "image/urf", urfBytes(1, 4962, 7014))
	if rr.Code != 400 {
		t.Errorf("缺 X-Printer-Serial = %d，期望 400", rr.Code)
	}
}

// 不是自己名下的设备，不能往里投作业。
func TestPrintRejectsForeignDevice(t *testing.T) {
	h, dep := newTestAPI(t)
	token := loginFixture(t, h, dep, "13800008888")
	rr := upload(h, token, "aaaaaaaaaaaa", "PA", "image/urf", urfBytes(1, 4962, 7014))
	if rr.Code != 403 {
		t.Errorf("投给别人的设备 = %d，期望 403", rr.Code)
	}
}

// 取件必须校验作业归属——v1 完全没查，任何人能下载任何作业。
func TestJobDataChecksOwnership(t *testing.T) {
	h, dep := newTestAPI(t)
	appTok := loginFixture(t, h, dep, "13800008888")
	rr := post(h, "/api/device/enroll", `{"dev":"f412fa87c9e0"}`, bearer(appTok))
	var en struct{ DeviceKey string `json:"device_key"` }
	json.Unmarshal(rr.Body.Bytes(), &en)

	rr = upload(h, appTok, "f412fa87c9e0", "PA", "image/urf", urfBytes(1, 4962, 7014))
	var up struct{ Job string `json:"job"` }
	json.Unmarshal(rr.Body.Bytes(), &up)

	// 正主取件
	got := get(h, "/api/job/"+up.Job+"/data", map[string]string{
		"X-Device": "f412fa87c9e0", "Authorization": "Bearer " + en.DeviceKey})
	if got.Code != 200 {
		t.Fatalf("正主取件 = %d %s", got.Code, got.Body)
	}
	if got.Body.Len() == 0 {
		t.Error("取回来是空的")
	}
	// 取件后作业应转为 downloading
	if j, _, _ := dep.store.GetJob(up.Job); j.State != "downloading" {
		t.Errorf("取件后 state = %q", j.State)
	}

	// 换一台设备来取同一件
	dep.clock = dep.clock.Add(2 * 60e9)
	otherApp := loginFixture(t, h, dep, "13900009999")
	rr = post(h, "/api/device/enroll", `{"dev":"aaaaaaaaaaaa"}`, bearer(otherApp))
	var en2 struct{ DeviceKey string `json:"device_key"` }
	json.Unmarshal(rr.Body.Bytes(), &en2)

	got = get(h, "/api/job/"+up.Job+"/data", map[string]string{
		"X-Device": "aaaaaaaaaaaa", "Authorization": "Bearer " + en2.DeviceKey})
	if got.Code != 403 {
		t.Errorf("别人的设备取件 = %d，期望 403", got.Code)
	}
}
```

- [ ] **Step 4: 实现作业端点**

`server/go/internal/httpapi/job_endpoints.go`:

```go
package httpapi

import (
	"crypto/rand"
	"encoding/hex"
	"encoding/json"
	"io"
	"net/http"
	"os"
	"path/filepath"

	"github.com/dayuer/stickbox/server/go/internal/auth"
	"github.com/dayuer/stickbox/server/go/internal/device"
	"github.com/dayuer/stickbox/server/go/internal/raster"
	"github.com/dayuer/stickbox/server/go/internal/store"
)

const maxJobBytes = 200 << 20 // URF 是光栅，整页照片单页就能到 15MB

func newJobID() string {
	b := make([]byte, 6)
	rand.Read(b)
	return hex.EncodeToString(b)
}

// handlePrint 收一份已光栅的 URF/PWG。服务端不渲染，只校验、落盘、入队。
func (a *API) handlePrint(w http.ResponseWriter, r *http.Request, id auth.Identity) {
	dev := r.Header.Get("X-Device")
	serial := r.Header.Get("X-Printer-Serial")
	if dev == "" || serial == "" {
		fail(w, 400, "bad request", "X-Device 和 X-Printer-Serial 都必填")
		return
	}
	owner, bound, err := a.store.OwnerOfDevice(dev)
	if err != nil {
		fail(w, 500, "server error", "")
		return
	}
	if !bound || owner != id.UserID {
		fail(w, 403, "forbidden", "")
		return
	}
	format, ok := raster.FormatFromContentType(r.Header.Get("Content-Type"))
	if !ok {
		fail(w, 415, "unsupported media type",
			"只接受 image/urf 与 image/pwg-raster；服务端不渲染")
		return
	}

	jid := newJobID()
	path := filepath.Join(a.cfg.JobsDir(), jid+".urf")
	f, err := os.Create(path)
	if err != nil {
		fail(w, 500, "server error", "")
		return
	}

	// 先读头部做校验，再流式写盘——不为了校验把整份读进内存。
	head := make([]byte, 4096)
	n, _ := io.ReadFull(io.LimitReader(r.Body, int64(len(head))), head)
	head = head[:n]
	info, verr := raster.Verify(format, head)
	if verr != nil {
		f.Close()
		os.Remove(path)
		fail(w, 400, "bad raster", verr.Error())
		return
	}
	if _, err := f.Write(head); err != nil {
		f.Close()
		os.Remove(path)
		fail(w, 500, "server error", "")
		return
	}
	written, err := io.Copy(f, http.MaxBytesReader(w, r.Body, maxJobBytes))
	f.Close()
	if err != nil {
		os.Remove(path)
		fail(w, 413, "too large", "")
		return
	}
	size := int64(n) + written

	name := r.Header.Get("X-Filename")
	if err := a.store.InsertJob(store.Job{
		ID: jid, Dev: dev, Name: name, Size: size,
		State: store.StateQueued, Serial: serial,
	}); err != nil {
		os.Remove(path)
		fail(w, 500, "server error", "")
		return
	}

	// 叫醒 actor。设备不在线或插着别的打印机时，作业就留在队列里等着。
	a.devMsg(dev, device.Msg{Kind: device.KindWake})

	attached := false
	if act := a.reg.Actor(dev); act != nil {
		attached = act.Serial() == serial
	}
	writeJSON(w, 200, map[string]any{
		"job": jid, "size": size, "pages": info.Pages,
		"state": store.StateQueued, "printer_attached": attached,
	})
}

// handleJobData 是设备取件。v1 完全没有校验，任何人能下载任何作业。
func (a *API) handleJobData(w http.ResponseWriter, r *http.Request, id auth.Identity) {
	jid := r.PathValue("id")
	j, ok, err := a.store.GetJob(jid)
	if err != nil {
		fail(w, 500, "server error", "")
		return
	}
	if !ok {
		fail(w, 404, "not found", "")
		return
	}
	if j.Dev != id.Dev {
		fail(w, 403, "forbidden", "")
		return
	}
	f, err := os.Open(filepath.Join(a.cfg.JobsDir(), jid+".urf"))
	if err != nil {
		fail(w, 404, "not found", "作业文件已被清理")
		return
	}
	defer f.Close()

	// 请求一到就续超时：只要在传，就不会被 180 秒误判。
	a.store.SetJobState(jid, store.StateDownloading, 0, "")
	a.devMsg(id.Dev, device.Msg{Kind: device.KindDownloading, JobID: jid})

	fi, _ := f.Stat()
	// ServeContent 顺带把 Range 处理了——固件当前不用，但服务端先支持着，
	// 将来做断点续传不用改服务端。
	http.ServeContent(w, r, jid+".urf", fi.ModTime(), f)
}

func (a *API) handleIdent(w http.ResponseWriter, r *http.Request, id auth.Identity) {
	dev := r.PathValue("dev")
	if dev != id.Dev {
		fail(w, 403, "forbidden", "")
		return
	}
	raw, err := io.ReadAll(http.MaxBytesReader(w, r.Body, 256<<10))
	if err != nil {
		fail(w, 413, "too large", "")
		return
	}
	var probe map[string]any
	if err := json.Unmarshal(raw, &probe); err != nil {
		fail(w, 400, "bad json", "")
		return
	}
	dir := filepath.Join(a.cfg.IdentsDir(), dev)
	if err := os.MkdirAll(dir, 0o755); err != nil {
		fail(w, 500, "server error", "")
		return
	}
	// 按时间戳留历史：机型档案会随探针改进而变化，覆盖掉就没法比对了。
	os.WriteFile(filepath.Join(dir, "latest.json"), raw, 0o644)
	writeJSON(w, 200, map[string]int{"ok": 1})
}
```

- [ ] **Step 5: 实现 status / render-profile / printers**

追加到 `server/go/internal/httpapi/device_endpoints.go`:

```go
// handleRenderProfile 下发光栅参数。App 在光栅之前必须先拉这个，不要硬编码——
// 换打印机时改的是服务端，App 不用发版。
func (a *API) handleRenderProfile(w http.ResponseWriter, r *http.Request, id auth.Identity) {
	dev := r.PathValue("dev")
	owner, bound, _ := a.store.OwnerOfDevice(dev)
	if !bound || owner != id.UserID {
		fail(w, 403, "forbidden", "")
		return
	}
	caps, serial, ok := a.loadIdentCaps(dev)
	if !ok {
		fail(w, 404, "no printer", "该设备尚未上报机型档案——没插打印机或还没枚举完")
		return
	}
	p, err := raster.ParseCaps(caps)
	if err != nil {
		fail(w, 500, "bad caps", err.Error())
		return
	}
	writeJSON(w, 200, map[string]any{
		"dev": dev, "serial": serial, "format": p.Format,
		"urf_caps": p.Caps, "dpi": p.DPI, "color": p.Color, "pages": p.Pages,
		// 实测值，不是从能力串推的：不可打印区必须出纸才测得出（HANDOFF 3.6 第 2 层）
		"margins_mm":    []int{4, 4, 4, 4},
		"max_job_bytes": maxJobBytes,
	})
}

// handlePrinters 列出这个桥见过的所有打印机及各自的排队数。
// 用户看不到排队数就会以为打印失败了。
func (a *API) handlePrinters(w http.ResponseWriter, r *http.Request, id auth.Identity) {
	dev := r.PathValue("dev")
	owner, bound, _ := a.store.OwnerOfDevice(dev)
	if !bound || owner != id.UserID {
		fail(w, 403, "forbidden", "")
		return
	}
	attached := ""
	if act := a.reg.Actor(dev); act != nil {
		attached = act.Serial()
	}
	counts, err := a.store.QueuedCountByPrinter(dev)
	if err != nil {
		fail(w, 500, "server error", "")
		return
	}
	type row struct {
		Serial     string `json:"serial"`
		Attached   bool   `json:"attached"`
		QueuedJobs int    `json:"queued_jobs"`
	}
	var rows []row
	seen := map[string]bool{}
	for ser, n := range counts {
		rows = append(rows, row{ser, ser == attached, n})
		seen[ser] = true
	}
	if attached != "" && !seen[attached] {
		rows = append(rows, row{attached, true, 0})
	}
	writeJSON(w, 200, map[string]any{"attached": attached, "printers": rows})
}

func (a *API) handleStatus(w http.ResponseWriter, r *http.Request, id auth.Identity) {
	dev := id.Dev
	if id.Role == auth.RoleApp {
		dev = r.Header.Get("X-Device")
		owner, bound, _ := a.store.OwnerOfDevice(dev)
		if !bound || owner != id.UserID {
			fail(w, 403, "forbidden", "")
			return
		}
	}
	jobs, err := a.store.JobsForDevice(dev, 15)
	if err != nil {
		fail(w, 500, "server error", "")
		return
	}
	out := map[string]any{"jobs": jobs}
	if act := a.reg.Actor(dev); act != nil {
		out["device"] = map[string]any{
			"dev": dev, "online": true, "serial": act.Serial(),
			"prn": json.RawMessage(act.Printer()),
		}
	} else {
		out["device"] = map[string]any{"dev": dev, "online": false, "prn": nil}
	}
	writeJSON(w, 200, out)
}

// loadIdentCaps 从最近一份 ident 里取出 URF 能力串和打印机序列号。
func (a *API) loadIdentCaps(dev string) (caps, serial string, ok bool) {
	raw, err := os.ReadFile(filepath.Join(a.cfg.IdentsDir(), dev, "latest.json"))
	if err != nil {
		return "", "", false
	}
	var doc struct {
		URFCaps string `json:"urf_caps"`
		Serial  string `json:"serial"`
		Printer struct {
			URFCaps string `json:"urf"`
			Serial  string `json:"serial"`
		} `json:"printer_class"`
	}
	if err := json.Unmarshal(raw, &doc); err != nil {
		return "", "", false
	}
	caps, serial = doc.URFCaps, doc.Serial
	if caps == "" {
		caps = doc.Printer.URFCaps
	}
	if serial == "" {
		serial = doc.Printer.Serial
	}
	return caps, serial, caps != ""
}
```

（顶部需要 `"encoding/json"`、`"os"`、`"path/filepath"`、`raster` 的 import。）

- [ ] **Step 6: 运行确认通过**

Run: `cd server/go && go test ./internal/httpapi/ -v`
Expected: PASS，含「PDF 冒充 URF」和「别人的设备取件 403」

- [ ] **Step 7: 提交**

```bash
git add server/go/internal/httpapi
git commit -m "feat(server): 设备与作业端点——enroll 抢绑防护、上传入口校验、取件归属校验"
```

---

## Task 17: janitor — 作业文件清理

**Files:**
- Create: `server/go/internal/janitor/janitor.go`
- Create: `server/go/internal/janitor/janitor_test.go`

URF 比 PDF 大一到两个数量级，不清理会很快写满盘。Python 版没有任何清理，
因为那时作业小、量也小。

- [ ] **Step 1: 写失败的测试**

`server/go/internal/janitor/janitor_test.go`:

```go
package janitor

import (
	"os"
	"path/filepath"
	"testing"
	"time"

	"github.com/dayuer/stickbox/server/go/internal/store"
)

func setup(t *testing.T) (*store.Store, string) {
	t.Helper()
	dir := t.TempDir()
	jobs := filepath.Join(dir, "jobs")
	os.MkdirAll(jobs, 0o755)
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

func TestSweepDeletesOldDoneJobs(t *testing.T) {
	st, dir := setup(t)
	now := time.Unix(1_000_000, 0)
	old := now.Add(-25 * time.Hour).Unix()
	fresh := now.Add(-1 * time.Hour).Unix()

	st.InsertJob(store.Job{ID: "old", Dev: "d", Serial: "P", State: store.StateDone, Updated: old, Created: old})
	st.InsertJob(store.Job{ID: "new", Dev: "d", Serial: "P", State: store.StateDone, Updated: fresh, Created: fresh})
	touch(t, dir, "old")
	touch(t, dir, "new")

	j := New(st, dir, 24, 72, 50, func() time.Time { return now })
	if _, err := j.Sweep(); err != nil {
		t.Fatal(err)
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
	now := time.Unix(1_000_000, 0)
	ts := now.Add(-48 * time.Hour).Unix()
	st.InsertJob(store.Job{ID: "f", Dev: "d", Serial: "P", State: store.StateFailed, Updated: ts, Created: ts})
	touch(t, dir, "f")

	j := New(st, dir, 24, 72, 50, func() time.Time { return now })
	j.Sweep()
	if !exists(dir, "f") {
		t.Error("48 小时的失败作业不该删（保留 72 小时）")
	}
}

// 排队中的作业永远不删——用户可能只是换了台打印机，插回来就该接着打。
func TestSweepNeverDeletesQueued(t *testing.T) {
	st, dir := setup(t)
	now := time.Unix(1_000_000, 0)
	ts := now.Add(-1000 * time.Hour).Unix()
	st.InsertJob(store.Job{ID: "q", Dev: "d", Serial: "P", State: store.StateQueued, Updated: ts, Created: ts})
	touch(t, dir, "q")

	j := New(st, dir, 24, 72, 50, func() time.Time { return now })
	j.Sweep()
	if !exists(dir, "q") {
		t.Error("排队中的作业被删了——用户插回那台打印机就打不出来了")
	}
}

func TestSweepEnforcesPerDeviceCap(t *testing.T) {
	st, dir := setup(t)
	now := time.Unix(1_000_000, 0)
	for i := 0; i < 5; i++ {
		id := string(rune('a' + i))
		ts := now.Add(-time.Duration(i) * time.Minute).Unix()
		st.InsertJob(store.Job{ID: id, Dev: "d", Serial: "P",
			State: store.StateDone, Updated: ts, Created: ts})
		touch(t, dir, id)
	}
	j := New(st, dir, 24, 72, 3, func() time.Time { return now }) // 每设备最多 3 件
	j.Sweep()

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
```

- [ ] **Step 2: 实现**

`server/go/internal/janitor/janitor.go`:

```go
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

	"github.com/dayuer/stickbox/server/go/internal/store"
)

type Janitor struct {
	st       *store.Store
	dir      string
	doneHrs  int
	failHrs  int
	perDev   int
	now      func() time.Time
}

func New(st *store.Store, jobsDir string, doneHrs, failHrs, perDev int,
	now func() time.Time) *Janitor {
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
```

- [ ] **Step 3: 补 store 的查询**

追加到 `server/go/internal/store/job.go`:

```go
// ExpiredJobFiles 返回文件该被删掉的作业 id。
//
// 两条规则：按时长（done/failed 各有阈值），以及每设备保留上限。
// 只看已结束的作业——queued 永远不动。
func (s *Store) ExpiredJobFiles(doneBefore, failBefore int64, perDev int) ([]string, error) {
	rows, err := s.db.Query(
		`SELECT id FROM jobs
		 WHERE (state=? AND updated < ?) OR (state=? AND updated < ?)`,
		StateDone, doneBefore, StateFailed, failBefore)
	if err != nil {
		return nil, err
	}
	var out []string
	for rows.Next() {
		var id string
		if err := rows.Scan(&id); err != nil {
			rows.Close()
			return nil, err
		}
		out = append(out, id)
	}
	rows.Close()
	if err := rows.Err(); err != nil {
		return nil, err
	}

	// 每设备保留上限：按 updated 倒序，第 perDev 名之后的一律删。
	rows, err = s.db.Query(
		`SELECT id FROM jobs WHERE state IN (?,?) AND id NOT IN (
		   SELECT id FROM jobs j2 WHERE j2.dev = jobs.dev AND j2.state IN (?,?)
		   ORDER BY j2.updated DESC LIMIT ?)`,
		StateDone, StateFailed, StateDone, StateFailed, perDev)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	seen := map[string]bool{}
	for _, id := range out {
		seen[id] = true
	}
	for rows.Next() {
		var id string
		if err := rows.Scan(&id); err != nil {
			return nil, err
		}
		if !seen[id] {
			out = append(out, id)
		}
	}
	return out, rows.Err()
}
```

- [ ] **Step 4: 运行确认通过**

Run: `cd server/go && go test ./internal/janitor/ ./internal/store/ -v`
Expected: PASS，含「排队中的作业永不删」

- [ ] **Step 5: 提交**

```bash
git add server/go/internal/janitor server/go/internal/store/job.go
git commit -m "feat(server): 作业文件清理——按时长与每设备上限，queued 永不删"
```

---

## Task 18: main 装配与运维子命令

**Files:**
- Create: `server/go/cmd/stickboxd/main.go`
- Create: `server/go/cmd/stickboxd/cli.go`
- Create: `server/go/internal/sms/aliyun.go`

- [ ] **Step 1: 实现短信服务商**

`server/go/internal/sms/aliyun.go`:

```go
// Package sms 是短信服务商的具体实现。
//
// 单测和集成测试绝不真发短信——那既花钱又会骚扰真实号码。
// 测试一律用 auth.Sender 的假实现。
package sms

import (
	"context"
	"log/slog"
)

// Logger 是开发环境用的实现：只打日志，不发短信。
// 生产环境用 Aliyun。
type Logger struct{}

func (Logger) Send(ctx context.Context, phone, code string) error {
	slog.Warn("【开发模式】短信未真实发送", "phone", phone, "code", code)
	return nil
}

type AliyunConfig struct {
	AccessKeyID     string
	AccessKeySecret string
	SignName        string
	TemplateCode    string
}

type Aliyun struct{ cfg AliyunConfig }

func NewAliyun(c AliyunConfig) (*Aliyun, error) {
	// 实现时用阿里云 SDK：dysmsapi。这里只定形状——
	// 接入哪家不影响上层，auth.SMS 只认 Send 这一个方法。
	return &Aliyun{cfg: c}, nil
}

func (a *Aliyun) Send(ctx context.Context, phone, code string) error {
	// TemplateParam 形如 {"code":"123456"}
	panic("接入阿里云 SDK 时实现；开发环境用 Logger")
}
```

**注意**：上面的 `panic` 不是占位符敷衍——它是刻意的失败方式。配置里选了
`aliyun` 但 SDK 还没接时，进程要在启动时就炸掉，而不是安静地不发短信、
让所有人登不进来还查不出原因。接入 SDK 时把它替换掉。

- [ ] **Step 2: 实现 main**

`server/go/cmd/stickboxd/main.go`:

```go
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

	"github.com/dayuer/stickbox/server/go/internal/auth"
	"github.com/dayuer/stickbox/server/go/internal/broker"
	"github.com/dayuer/stickbox/server/go/internal/config"
	"github.com/dayuer/stickbox/server/go/internal/device"
	"github.com/dayuer/stickbox/server/go/internal/httpapi"
	"github.com/dayuer/stickbox/server/go/internal/janitor"
	"github.com/dayuer/stickbox/server/go/internal/registry"
	"github.com/dayuer/stickbox/server/go/internal/sms"
	"github.com/dayuer/stickbox/server/go/internal/store"
	"github.com/dayuer/stickbox/server/go/internal/tlsx"
	"github.com/dayuer/stickbox/server/go/internal/version"
)

func main() {
	confPath := flag.String("conf", envOr("STICKBOX_CONF", "/opt/stickbox/config.json"), "配置文件")
	flag.Parse()

	slog.SetDefault(slog.New(slog.NewTextHandler(os.Stderr, &slog.HandlerOptions{})))

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

	api := httpapi.New(cfg, st, v, pb, smsSvc, reg, br, mem)
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

	errc := make(chan error, 2)
	go func() { errc <- br.Serve() }()
	go func() {
		slog.Info("HTTPS 监听", "addr", cfg.HTTPAddr, "version", version.String())
		err := srv.ListenAndServeTLS("", "") // 证书由 GetCertificate 回调提供
		if errors.Is(err, http.ErrServerClosed) {
			err = nil
		}
		errc <- err
	}()
	slog.Info("MQTT/TLS 监听", "addr", cfg.MQTTAddr)

	sig := make(chan os.Signal, 1)
	signal.Notify(sig, syscall.SIGINT, syscall.SIGTERM)
	select {
	case err := <-errc:
		close(stop)
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
```

`config.Config` 相应补三个字段（Task 2 的结构体上加）：

```go
	PhonePepper string `json:"phone_pepper"`
	PhoneKey    string `json:"phone_key"`
	SMS         struct {
		Provider        string `json:"provider"`
		AccessKeyID     string `json:"access_key_id"`
		AccessKeySecret string `json:"access_key_secret"`
		SignName        string `json:"sign_name"`
		TemplateCode    string `json:"template_code"`
	} `json:"sms"`
```

并在 `Load` 里加校验（`cert_dir` 那段之后）：

```go
	if c.PhonePepper == "" || c.PhoneKey == "" {
		return nil, errors.New("config: phone_pepper 和 phone_key 必填")
	}
```

- [ ] **Step 3: 实现子命令**

`server/go/cmd/stickboxd/cli.go`:

```go
package main

import (
	"fmt"
	"time"

	"github.com/dayuer/stickbox/server/go/internal/auth"
	"github.com/dayuer/stickbox/server/go/internal/config"
	"github.com/dayuer/stickbox/server/go/internal/store"
)

// runCLI 是运维和排障用的。正常用户走 App 的 enroll，不碰这些。
func runCLI(cfg *config.Config, args []string) error {
	st, err := store.Open(cfg.DBPath())
	if err != nil {
		return err
	}
	defer st.Close()
	v := auth.NewVerifier(st)

	switch args[0] + " " + arg(args, 1) {
	case "device add":
		dev := arg(args, 2)
		if dev == "" {
			return fmt.Errorf("用法: stickboxd device add <dev> [name]")
		}
		token, err := auth.IssueDeviceKey(st, "", dev, arg(args, 3))
		if err != nil {
			return err
		}
		fmt.Printf("设备密钥（只显示这一次）：\n  %s\n", token)
		return nil

	case "device list":
		keys, err := st.ListKeys()
		if err != nil {
			return err
		}
		for _, k := range keys {
			state := "启用"
			if k.Disabled {
				state = "已吊销"
			}
			fmt.Printf("%-12s %-8s dev=%-14s user=%-32s %s %s\n",
				k.KeyID, k.Role, k.Dev, k.UserID, state, k.Name)
		}
		return nil

	case "device revoke":
		return auth.RevokeSession(st, v, arg(args, 2))

	// 抢绑防护的逃生门：原持有人不配合时用。
	// 绕过所有权检查，所以只能在服务器上执行，不暴露为 API。
	case "device unbind":
		return auth.RevokeDeviceKeys(st, v, arg(args, 2))

	case "user list":
		// 只打尾号，不解明文——列表场景不需要完整号码
		return fmt.Errorf("实现时补：SELECT id,phone_tail,created,last_login FROM users")

	// 唯一能解出完整号码的入口。每次调用记审计日志——得知道谁什么时候看过。
	case "user phone":
		userID := arg(args, 2)
		pb, err := auth.NewPhoneBox(cfg.PhonePepper, cfg.PhoneKey)
		if err != nil {
			return err
		}
		enc, ok, err := st.GetPhone(userID)
		if err != nil || !ok {
			return fmt.Errorf("该用户没有号码记录")
		}
		phone, err := pb.Open(enc)
		if err != nil {
			return err
		}
		fmt.Printf("%s\n", phone)
		fmt.Fprintf(auditWriter(), "%s 解密查看 user=%s\n",
			time.Now().Format(time.RFC3339), userID)
		return nil
	}
	return fmt.Errorf("未知命令：%v", args)
}

func arg(a []string, i int) string {
	if i < len(a) {
		return a[i]
	}
	return ""
}
```

`auditWriter()` 实现时指向 `/opt/stickbox/audit.log`（`0600`，追加模式）。

- [ ] **Step 4: 编译并跑全量测试**

Run: `cd server/go && go build ./... && go vet ./... && go test -race ./...`
Expected: 编译通过，`go vet` 无输出，全部测试 PASS

- [ ] **Step 5: 提交**

```bash
git add server/go/cmd server/go/internal/sms server/go/internal/config
git commit -m "feat(server): main 装配、优雅退出与运维子命令"
```

---

## Task 19: 集成测试

**Files:**
- Create: `server/go/integration_test.go`

起真 broker + 真 HTTP，用一个假设备跑完整链路。**这是唯一能验证「各个包接起来
还对」的地方**——单测都过不代表串起来能用。

- [ ] **Step 1: 写测试**

`server/go/integration_test.go`:

```go
package main_test

// 完整链路：登录 → enroll → 上传 URF → 设备取件 → 回执 → 派下一件。
//
// 起真 broker（自签证书）和真 HTTP 服务，用 paho 当假设备。
// 不真发短信——sender 是假的，验证码直接取。

import (
	"testing"
	"time"
)

func TestEndToEndPrintFlow(t *testing.T) {
	env := startServer(t) // 见 Step 2

	// 1. 登录
	token := env.login(t, "13800008888")

	// 2. enroll 一台设备
	devKey := env.enroll(t, token, "f412fa87c9e0")

	// 3. 假设备连 MQTT，订阅 job，报一条带 serial 的心跳
	dev := env.connectDevice(t, "f412fa87c9e0", devKey)
	dev.heartbeat(t, "ready", "", "CNB9K1P2X4")

	// 4. 上传一份 URF
	jid := env.upload(t, token, "f412fa87c9e0", "CNB9K1P2X4", urfBytes(2, 4962, 7014))

	// 5. 设备应当在几百毫秒内收到派发信令
	got := dev.waitJob(t, 3*time.Second)
	if got.ID != jid {
		t.Fatalf("派发的作业 = %s，期望 %s", got.ID, jid)
	}

	// 6. 取件
	body := env.fetchJob(t, devKey, "f412fa87c9e0", jid)
	if len(body) != got.Size {
		t.Errorf("取回 %d 字节，信令说 %d", len(body), got.Size)
	}

	// 7. 回执
	dev.heartbeat(t, "done", jid, "CNB9K1P2X4")
	env.waitJobState(t, jid, "done", 3*time.Second)
}

// 换打印机后，为旧机器排的作业不能派给新机器。
func TestEndToEndPrinterSwap(t *testing.T) {
	env := startServer(t)
	token := env.login(t, "13800008888")
	devKey := env.enroll(t, token, "f412fa87c9e0")
	dev := env.connectDevice(t, "f412fa87c9e0", devKey)

	dev.heartbeat(t, "ready", "", "PRINTER-A")
	jidA := env.upload(t, token, "f412fa87c9e0", "PRINTER-A", urfBytes(1, 4962, 7014))
	dev.waitJob(t, 3*time.Second)

	// 用户换了台打印机
	dev.heartbeat(t, "ready", "", "PRINTER-B")
	jidB := env.upload(t, token, "f412fa87c9e0", "PRINTER-B", urfBytes(1, 4962, 7014))

	got := dev.waitJob(t, 3*time.Second)
	if got.ID != jidB {
		t.Fatalf("换机后派了 %s，期望 %s（A 机的作业必须留在队列里）", got.ID, jidB)
	}
	env.waitJobState(t, jidA, "queued", 2*time.Second)
}

// ACL：设备不能订阅别人的 topic。
func TestEndToEndACLBlocksCrossDevice(t *testing.T) {
	env := startServer(t)
	token := env.login(t, "13800008888")
	devKey := env.enroll(t, token, "f412fa87c9e0")
	dev := env.connectDevice(t, "f412fa87c9e0", devKey)

	if err := dev.subscribe("printer/aaaaaaaaaaaa/job"); err == nil {
		t.Error("设备订阅了别人的 topic——ACL 没生效")
	}
}

// 认证失败必须被拒，且不能是「连上了但收不到消息」那种半死状态。
func TestEndToEndRejectsBadKey(t *testing.T) {
	env := startServer(t)
	if _, err := env.tryConnectDevice("f412fa87c9e0", "bogus.key"); err == nil {
		t.Error("错误密钥竟然连上了")
	}
}
```

- [ ] **Step 2: 写测试脚手架**

`server/go/integration_helpers_test.go` 实现 `startServer`、`login`、`enroll`、
`upload`、`fetchJob`、`waitJobState`、`connectDevice`、`urfBytes`。要点：

- 用 `crypto/x509` 现生成一张自签证书写进 `t.TempDir()`，客户端侧
  `InsecureSkipVerify: true`（**只在测试里**）
- 端口用 `:0` 让内核分配，再从 listener 上读回实际端口——**固定端口会让
  并行测试互相打架**
- MQTT 客户端用 `github.com/eclipse/paho.mqtt.golang`
- 短信 sender 用假实现，验证码直接从它身上取
- `waitJobState` 轮询数据库，不要 `time.Sleep` 一个固定值——那是 flaky 之源

- [ ] **Step 3: 运行**

Run: `cd server/go && go test -race -run TestEndToEnd -v ./...`
Expected: 四个用例全 PASS

- [ ] **Step 4: 提交**

```bash
git add server/go/integration_test.go server/go/integration_helpers_test.go
git commit -m "test(server): 端到端——完整链路、换打印机、ACL、认证拒绝"
```

---

## Task 20: 部署与迁移

**Files:**
- Modify: `server/stickbox-job.service` → 重写为 `server/stickboxd.service`
- Delete: `server/web/index.html`、`server/bin/jobsrv.py`
- Create: `server/DEPLOY.md`

- [ ] **Step 1: 写 systemd unit**

`server/stickboxd.service`:

```ini
[Unit]
Description=StickBox 云打印服务（Go）
After=network-online.target
Wants=network-online.target

[Service]
Type=simple
ExecStart=/opt/stickbox/bin/stickboxd -conf /opt/stickbox/config.json
Restart=always
RestartSec=3
User=stickbox
Group=stickbox

# 证书目录要可读；LE 的私钥默认 root-only，部署时把 stickbox 加进对应组
ReadWritePaths=/opt/stickbox
ProtectSystem=strict
ProtectHome=yes
PrivateTmp=yes
NoNewPrivileges=yes

# 几万条长连接时 fd 会成为第一个瓶颈
LimitNOFILE=65535

[Install]
WantedBy=multi-user.target
```

**没有 `Requires=mosquitto.service`**——broker 已内嵌，mosquitto 下线。

- [ ] **Step 2: 删掉不再部署的东西**

```bash
git rm server/web/index.html
git rm server/bin/jobsrv.py
git rm server/stickbox-job.service
```

`render.py` / `text2pdf.py` 已在早前移到 `tools/reference/` 并标注，不要再删——
`fix_page_count` 是客户端 URF 编码器的参考实现。

- [ ] **Step 3: 写部署文档**

`server/DEPLOY.md` 要覆盖：

1. **构建**：`cd server/go && CGO_ENABLED=0 go build -ldflags "-X .../version.Version=$(git describe --tags --always)" -o stickboxd ./cmd/stickboxd`
   （`CGO_ENABLED=0` 是可行的——sqlite 用的是纯 Go 的 modernc）
2. **首次部署**：建 `stickbox` 用户、`/opt/stickbox` 目录树、写 `config.json`
   （`chmod 600`）、生成 `phone_pepper` 与 `phone_key`
   （`openssl rand -hex 32`）
3. **两把密钥的运维含义**（这段必须显眼）：
   - `phone_pepper` 换了 **全体用户无法登录**，绝不轮换
   - `phone_key` 换了旧密文解不开但登录不受影响，要轮换得写一次全表重加密
4. **证书**：certbot 照旧；deploy hook 从「必需」降级为「可选」——
   进程按 mtime 自己重载。hook 仍建议保留用于改权限
5. **备份**：`jobs.db` 现在含个人信息（加密的手机号），**备份文件必须加密且限权**，
   不能像以前那样随手 `scp`
6. **切换步骤**：
   ```
   systemctl stop stickbox-job.service mosquitto.service
   systemctl disable mosquitto.service
   systemctl enable --now stickboxd.service
   ```
7. **回滚**：反过来。数据库双向兼容——旧版忽略新增的表和列。
   **前提是 CUPS 还在**，所以别急着卸载，跑稳一个月再清
8. **验证清单**：见下一步

- [ ] **Step 4: 上线验证清单**

写进 `DEPLOY.md`，切换后逐条执行：

```bash
# 1. 两个端口都起来了
ss -lntp | grep -E ':(8883|9443)'

# 2. 证书正确
openssl s_client -connect mqtt.silkline.id:9443 -servername mqtt.silkline.id </dev/null 2>/dev/null | openssl x509 -noout -dates

# 3. 错误密钥必须被拒
mosquitto_pub -h mqtt.silkline.id -p 8883 --capath /etc/ssl/certs \
  -u f412fa87c9e0 -P 'bogus.key' -t 'printer/f412fa87c9e0/status' -m '{}' ; echo "退出码=$? （非 0 才对）"

# 4. ACL 生效：不能碰别人的 topic
mosquitto_pub -h mqtt.silkline.id -p 8883 --capath /etc/ssl/certs \
  -u f412fa87c9e0 -P "$DEVKEY" -t 'printer/aaaaaaaaaaaa/status' -m '{}' ; echo "退出码=$? （非 0 才对）"

# 5. 无鉴权访问必须 401
curl -s -o /dev/null -w '%{http_code}\n' https://mqtt.silkline.id:9443/api/status

# 6. PDF 冒充 URF 必须 400
curl -s -o /dev/null -w '%{http_code}\n' -X POST https://mqtt.silkline.id:9443/api/print \
  -H 'Content-Type: image/urf' -H "X-Device: $DEV" -H "X-Printer-Serial: $SERIAL" \
  -H "Authorization: Bearer $APPTOK" --data-binary '%PDF-1.7 fake'

# 7. 证书热重载：摸一下证书文件，日志里应出现「证书已热重载」
touch /etc/letsencrypt/live/mqtt.silkline.id/fullchain.pem
journalctl -u stickboxd -n 20 | grep 热重载
```

第 3、4、6 条是这次重写补的三个洞（无设备认证、无 ACL、无入口校验），
**上线后必须亲手验一遍**——它们没有用户可见的症状，坏了也不会有人报障。

- [ ] **Step 5: 提交**

```bash
git add server/stickboxd.service server/DEPLOY.md
git commit -m "chore(server): systemd unit、部署文档与上线验证清单"
```

---

## 完成标准

全部 20 个 Task 做完后，逐条确认：

- [ ] `cd server/go && go test -race ./...` 全绿
- [ ] `go vet ./...` 无输出
- [ ] `CGO_ENABLED=0 go build ./...` 通过（纯静态二进制）
- [ ] 上线验证清单第 3、4、6 条在真服务器上手工验过
- [ ] `docs/API-cloud-print.md` 与实现逐条对齐，不一致时**改实现不改文档**
- [ ] 固件侧三项改动（NVS 读密钥、HTTP 加鉴权头、配网页加输入框）另开一轮
