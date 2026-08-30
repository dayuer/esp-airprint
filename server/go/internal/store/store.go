// Package store 是作业、密钥与用户的唯一真相来源。
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
  user_id   TEXT NOT NULL DEFAULT '',
  name      TEXT NOT NULL DEFAULT '',
  key_hash  TEXT NOT NULL,
  created   INTEGER NOT NULL DEFAULT 0,
  last_seen INTEGER NOT NULL DEFAULT 0,
  disabled  INTEGER NOT NULL DEFAULT 0
);
CREATE INDEX IF NOT EXISTS devices_dev  ON devices(dev);
CREATE INDEX IF NOT EXISTS devices_user ON devices(user_id);

CREATE TABLE IF NOT EXISTS users(
  id         TEXT PRIMARY KEY,
  phone_hmac TEXT NOT NULL UNIQUE,
  phone_tail TEXT NOT NULL DEFAULT '',
  created    INTEGER NOT NULL DEFAULT 0,
  last_login INTEGER NOT NULL DEFAULT 0,
  disabled   INTEGER NOT NULL DEFAULT 0
);

-- 完整号码单独成表：登录和 ACL 是高频路径，个人信息不进热表
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
  day_start  INTEGER NOT NULL DEFAULT 0,
  day_count  INTEGER NOT NULL DEFAULT 0
);
`

func Open(path string) (*Store, error) {
	db, err := sql.Open("sqlite",
		path+"?_pragma=busy_timeout(5000)&_pragma=journal_mode(WAL)")
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
	db.Exec(`ALTER TABLE devices ADD COLUMN user_id TEXT NOT NULL DEFAULT ''`)
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
		`UPDATE jobs SET state='queued', bytes=0
		 WHERE state IN ('downloading','rendering','no-device')`)
	if err != nil {
		return 0, err
	}
	n, err := r.RowsAffected()
	return int(n), err
}
