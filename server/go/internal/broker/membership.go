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
	now func() time.Time

	mu sync.RWMutex
	c  map[string]entry
}

type entry struct {
	devs map[string]bool
	exp  time.Time
}

func NewMembership(st *store.Store) *Membership {
	return &Membership{st: st, ttl: 5 * time.Minute, now: time.Now,
		c: map[string]entry{}}
}

func (m *Membership) HasDevice(userID, dev string) bool {
	m.mu.RLock()
	e, ok := m.c[userID]
	m.mu.RUnlock()
	if ok && m.now().Before(e.exp) {
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
	m.c[userID] = entry{devs: set, exp: m.now().Add(m.ttl)}
	m.mu.Unlock()
	return set[dev]
}

// Invalidate 在 enroll / unbind / 注销之后调用，否则新绑的设备最多要等
// 5 分钟才能订阅成功。
func (m *Membership) Invalidate(userID string) {
	m.mu.Lock()
	delete(m.c, userID)
	m.mu.Unlock()
}
