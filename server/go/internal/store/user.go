package store

import (
	"crypto/rand"
	"database/sql"
	"encoding/hex"
	"errors"
)

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
	return hex.EncodeToString(b)
}

const userCols = `id,phone_hmac,phone_tail,created,last_login,disabled`

// UpsertUser 按手机号 HMAC 找用户，没有就创建。created 表示是否新建。
// 登录路径只碰 HMAC，全程不解密完整号码。
func (s *Store) UpsertUser(phoneHMAC, tail string) (User, bool, error) {
	if u, ok, err := s.UserByHMAC(phoneHMAC); err != nil {
		return User{}, false, err
	} else if ok {
		ts := now()
		s.wmu.Lock()
		_, err = s.db.Exec(`UPDATE users SET last_login=? WHERE id=?`, ts, u.ID)
		s.wmu.Unlock()
		u.LastLogin = ts
		return u, false, err
	}
	u := User{ID: newID(), PhoneHMAC: phoneHMAC, PhoneTail: tail,
		Created: now(), LastLogin: now()}
	s.wmu.Lock()
	_, err := s.db.Exec(
		`INSERT INTO users(`+userCols+`) VALUES(?,?,?,?,?,0)`,
		u.ID, u.PhoneHMAC, u.PhoneTail, u.Created, u.LastLogin)
	s.wmu.Unlock()
	return u, true, err
}

func (s *Store) UserByHMAC(phoneHMAC string) (User, bool, error) {
	return scanUser(s.db.QueryRow(
		`SELECT `+userCols+` FROM users WHERE phone_hmac=?`, phoneHMAC))
}

func (s *Store) UserByID(id string) (User, bool, error) {
	return scanUser(s.db.QueryRow(`SELECT `+userCols+` FROM users WHERE id=?`, id))
}

func (s *Store) ListUsers() ([]User, error) {
	rows, err := s.db.Query(`SELECT ` + userCols + ` FROM users ORDER BY created`)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	out := []User{}
	for rows.Next() {
		var u User
		var disabled int
		if err := rows.Scan(&u.ID, &u.PhoneHMAC, &u.PhoneTail,
			&u.Created, &u.LastLogin, &disabled); err != nil {
			return nil, err
		}
		u.Disabled = disabled != 0
		out = append(out, u)
	}
	return out, rows.Err()
}

func scanUser(r rowScanner) (User, bool, error) {
	var u User
	var disabled int
	err := r.Scan(&u.ID, &u.PhoneHMAC, &u.PhoneTail, &u.Created, &u.LastLogin, &disabled)
	if err != nil {
		if errors.Is(err, sql.ErrNoRows) {
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
	err := s.db.QueryRow(
		`SELECT phone_enc FROM user_phones WHERE user_id=?`, userID).Scan(&enc)
	if err != nil {
		if errors.Is(err, sql.ErrNoRows) {
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
	jobIDs := []string{}
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
	for _, q := range []string{
		`DELETE FROM devices WHERE user_id=?`,
		`DELETE FROM user_phones WHERE user_id=?`,
		`DELETE FROM users WHERE id=?`,
	} {
		if _, err := tx.Exec(q, userID); err != nil {
			return nil, err
		}
	}
	return jobIDs, tx.Commit()
}

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
		if errors.Is(err, sql.ErrNoRows) {
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
