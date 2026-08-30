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
	var n int
	s.db.QueryRow(
		`SELECT COUNT(*) FROM pragma_table_info('users') WHERE name='phone_enc'`).Scan(&n)
	if n != 0 {
		t.Error("users 表里出现了 phone_enc 列——个人信息进了热表")
	}
}

// 注销账号必须把个人数据清干净。
func TestDeleteUserRemovesEverything(t *testing.T) {
	s := open(t)
	u, _, _ := s.UpsertUser("HMAC-A", "8888")
	s.PutPhone(u.ID, []byte("x"))
	s.InsertKey(Key{KeyID: "k1", Dev: "d1", Role: "device", UserID: u.ID, Hash: "h"})
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
		t.Error("密钥还在")
	}
	if _, ok, _ := s.GetJob("j1"); ok {
		t.Error("作业记录还在")
	}
}

func TestDevicesOfUserDedupes(t *testing.T) {
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

// 吊销后不该再算作该用户的设备，也不该再算作有主。
func TestDisabledKeyReleasesDevice(t *testing.T) {
	s := open(t)
	s.InsertKey(Key{KeyID: "k1", Dev: "d1", Role: "device", UserID: "u1", Hash: "h"})
	if _, err := s.DisableKeysOfDevice("d1", "device"); err != nil {
		t.Fatal(err)
	}
	if _, ok, _ := s.OwnerOfDevice("d1"); ok {
		t.Error("吊销后设备仍有归属——解绑就没意义了")
	}
	if devs, _ := s.DevicesOfUser("u1"); len(devs) != 0 {
		t.Errorf("吊销后仍列在名下：%v", devs)
	}
}

func TestSMSCodeRoundTrip(t *testing.T) {
	s := open(t)
	if err := s.PutSMSCode(SMSCode{PhoneHMAC: "H", CodeHash: "abc",
		Expires: 100, SentAt: 50, DayStart: 0, DayCount: 1}); err != nil {
		t.Fatal(err)
	}
	c, ok, err := s.SMSCode("H")
	if err != nil || !ok || c.CodeHash != "abc" || c.DayCount != 1 {
		t.Fatalf("取回 %+v ok=%v err=%v", c, ok, err)
	}
	s.BumpSMSAttempts("H")
	c, _, _ = s.SMSCode("H")
	if c.Attempts != 1 {
		t.Errorf("attempts = %d，期望 1", c.Attempts)
	}
	// 重发覆盖同一行，且把 attempts 清零
	s.PutSMSCode(SMSCode{PhoneHMAC: "H", CodeHash: "def", Expires: 200, SentAt: 150, DayCount: 2})
	c, _, _ = s.SMSCode("H")
	if c.CodeHash != "def" || c.Attempts != 0 {
		t.Errorf("重发后 %+v，期望换码且 attempts 归零", c)
	}
	s.DeleteSMSCode("H")
	if _, ok, _ := s.SMSCode("H"); ok {
		t.Error("删除后仍存在")
	}
}
