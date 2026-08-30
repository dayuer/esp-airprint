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
	v.Verify(a)
	v.Verify(b)

	keyID, _, _ := SplitToken(a)
	if err := RevokeSession(st, v, keyID); err != nil {
		t.Fatal(err)
	}
	if _, err := v.Verify(a); err == nil {
		t.Error("被吊销的 token 仍可用——缓存没清")
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
	v.Verify(a)
	v.Verify(b)
	v.Verify(c)

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

// 重置设备：吊销旧的、签发新的，旧的必须立刻失效。
func TestRevokeDeviceKeysThenReissue(t *testing.T) {
	st := newStore(t)
	v := NewVerifier(st)
	old, _ := IssueDeviceKey(st, "u1", "dev1", "")
	v.Verify(old)

	if err := RevokeDeviceKeys(st, v, "dev1"); err != nil {
		t.Fatal(err)
	}
	if _, err := v.Verify(old); err == nil {
		t.Error("重置后旧设备密钥仍可用")
	}
	fresh, _ := IssueDeviceKey(st, "u1", "dev1", "")
	if _, err := v.Verify(fresh); err != nil {
		t.Errorf("新签发的密钥不可用：%v", err)
	}
}
