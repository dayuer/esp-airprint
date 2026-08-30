package broker

import (
	"testing"
	"time"

	"github.com/dayuer/stickbox/server/go/internal/store"
)

func newMem(t *testing.T) (*Membership, *store.Store, *time.Time) {
	t.Helper()
	st, err := store.Open(t.TempDir() + "/jobs.db")
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { st.Close() })
	clk := time.Unix(1_000_000, 0)
	m := NewMembership(st)
	m.now = func() time.Time { return clk }
	return m, st, &clk
}

func TestMembershipAnswersFromStore(t *testing.T) {
	m, st, _ := newMem(t)
	st.InsertKey(store.Key{KeyID: "k1", Dev: "d1", Role: "device", UserID: "u1", Hash: "h"})

	if !m.HasDevice("u1", "d1") {
		t.Error("名下设备被判为不属于")
	}
	if m.HasDevice("u1", "other") {
		t.Error("不在名下的设备被放行")
	}
	if m.HasDevice("u2", "d1") {
		t.Error("别人的设备被放行")
	}
}

// enroll 之后必须失效缓存，否则新绑的设备最多要等 5 分钟才能订阅成功。
func TestMembershipInvalidate(t *testing.T) {
	m, st, _ := newMem(t)
	if m.HasDevice("u1", "d1") {
		t.Fatal("还没绑就说属于")
	}
	st.InsertKey(store.Key{KeyID: "k1", Dev: "d1", Role: "device", UserID: "u1", Hash: "h"})
	if m.HasDevice("u1", "d1") {
		t.Error("缓存该挡住这次查询——说明根本没缓存")
	}
	m.Invalidate("u1")
	if !m.HasDevice("u1", "d1") {
		t.Error("Invalidate 后仍拿旧结果")
	}
}

func TestMembershipCacheExpires(t *testing.T) {
	m, st, clk := newMem(t)
	m.HasDevice("u1", "d1")
	st.InsertKey(store.Key{KeyID: "k1", Dev: "d1", Role: "device", UserID: "u1", Hash: "h"})
	*clk = clk.Add(6 * time.Minute)
	if !m.HasDevice("u1", "d1") {
		t.Error("缓存过期后仍拿旧结果")
	}
}
