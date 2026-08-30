package profile

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

// 存一份把 job_end 清空的档案，代表「这台机器实测不需要 UEL」。
func putNoUEL(t *testing.T, st *store.Store, scope, match string, votes int, disputed, pinned bool) {
	t.Helper()
	p := Default("")
	p.Hooks.JobEnd = nil
	b, _ := Marshal(p)
	if err := st.PutProfile(store.ProfileRow{
		Scope: scope, Match: match, Body: string(b),
		Votes: votes, Disputed: disputed, Pinned: pinned,
	}); err != nil {
		t.Fatal(err)
	}
}

func sendsUEL(p Profile) bool {
	for _, s := range p.Hooks.JobEnd {
		if s.Op == OpSendHex && s.Data == UEL {
			return true
		}
	}
	return false
}

func TestLookupFallsBackToDefault(t *testing.T) {
	st := newStore(t)
	p := Lookup(st, "PA", "03F0", "F22A", "HP Laser MFP 136a")
	if p.Src != SrcDefault {
		t.Errorf("src = %q，期望 default", p.Src)
	}
	if !sendsUEL(p) {
		t.Error("默认档案必须发 UEL")
	}
}

func TestLookupPrefersSerialOverModel(t *testing.T) {
	st := newStore(t)
	mkey := ModelKey("03F0", "F22A", "M")
	putNoUEL(t, st, store.ScopeModel, mkey, 5, false, false)

	// 本机实测：明确要发 UEL
	b, _ := Marshal(Default(""))
	st.PutProfile(store.ProfileRow{Scope: store.ScopeSerial, Match: "PA", Body: string(b)})

	p := Lookup(st, "PA", "03F0", "F22A", "M")
	if p.Src != SrcSerial {
		t.Fatalf("src = %q，期望 serial 优先", p.Src)
	}
	if !sendsUEL(p) {
		t.Error("本机实测的结论没生效")
	}
}

// 票数不够就不能当机型默认——一个用户看错纸不能让同型号所有人打不出东西。
func TestLookupRequiresVoteThreshold(t *testing.T) {
	st := newStore(t)
	mkey := ModelKey("03F0", "F22A", "M")
	putNoUEL(t, st, store.ScopeModel, mkey, ModelVoteThreshold-1, false, false)

	if p := Lookup(st, "PA", "03F0", "F22A", "M"); p.Src != SrcDefault {
		t.Errorf("票数 %d 就生效了（src=%q）", ModelVoteThreshold-1, p.Src)
	}
	putNoUEL(t, st, store.ScopeModel, mkey, ModelVoteThreshold, false, false)
	p := Lookup(st, "PA", "03F0", "F22A", "M")
	if p.Src != SrcModel {
		t.Errorf("票数够了却没生效：src=%q", p.Src)
	}
	if sendsUEL(p) {
		t.Error("机型档案的结论没生效")
	}
}

// 分歧即冻结：不自动采用，回退到下一层等人工仲裁。
func TestLookupSkipsDisputed(t *testing.T) {
	st := newStore(t)
	mkey := ModelKey("03F0", "F22A", "M")
	putNoUEL(t, st, store.ScopeModel, mkey, 99, true, false)
	if p := Lookup(st, "PA", "03F0", "F22A", "M"); p.Src != SrcDefault {
		t.Errorf("有分歧的机型档案被采用了：src=%q", p.Src)
	}
}

// pin 是逃生门：众包数据明显错了时，开发者的档案压过实测。
func TestLookupPinnedModelBeatsSerial(t *testing.T) {
	st := newStore(t)
	mkey := ModelKey("03F0", "F22A", "M")
	putNoUEL(t, st, store.ScopeModel, mkey, 0, false, true) // pinned

	b, _ := Marshal(Default("")) // 本机实测说要发 UEL
	st.PutProfile(store.ProfileRow{Scope: store.ScopeSerial, Match: "PA", Body: string(b)})

	p := Lookup(st, "PA", "03F0", "F22A", "M")
	if sendsUEL(p) {
		t.Error("pin 的机型档案没能压过本机实测")
	}
}

// 下发的档案必须带当前打印机的序列号——设备靠它判断该不该套用。
func TestLookupStampsSerial(t *testing.T) {
	st := newStore(t)
	if p := Lookup(st, "CNB9K1P2X4", "03F0", "F22A", "M"); p.Serial != "CNB9K1P2X4" {
		t.Errorf("serial = %q", p.Serial)
	}
}

// 存档里是坏 JSON 时降级，不能把整条链路打死。
func TestLookupSurvivesCorruptRow(t *testing.T) {
	st := newStore(t)
	st.PutProfile(store.ProfileRow{Scope: store.ScopeSerial, Match: "PA", Body: "{not json"})
	if p := Lookup(st, "PA", "03F0", "F22A", "M"); p.Src != SrcDefault {
		t.Errorf("坏档案没能降级：src=%q", p.Src)
	}
}
