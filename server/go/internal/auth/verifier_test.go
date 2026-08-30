package auth

import (
	"sync"
	"testing"
	"time"

	"github.com/dayuer/esp-airprint/server/go/internal/store"
)

type fakeKeys struct {
	mu    sync.Mutex
	keys  map[string]store.Key
	reads int
}

func (f *fakeKeys) KeyByID(id string) (store.Key, bool, error) {
	f.mu.Lock()
	defer f.mu.Unlock()
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
	keyID, _, _ := SplitToken(token)
	v.Invalidate(keyID)
	before := f.reads
	v.Verify(token)
	if f.reads == before {
		t.Error("Invalidate 后应重新查库")
	}
}

// 重连风暴：几十个并发校验不能把内存打爆，也不能出错。
// 信号量把同时进行的 argon2 限制在 8 个以内。
func TestVerifyConcurrentIsBounded(t *testing.T) {
	v, _, token := newFixture(t)
	var wg sync.WaitGroup
	errs := make(chan error, 32)
	for i := 0; i < 32; i++ {
		wg.Add(1)
		go func() {
			defer wg.Done()
			if _, err := v.Verify(token); err != nil {
				errs <- err
			}
		}()
	}
	wg.Wait()
	close(errs)
	for err := range errs {
		t.Fatalf("并发校验出错：%v", err)
	}
}
