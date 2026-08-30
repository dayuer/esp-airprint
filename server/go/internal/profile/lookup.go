package profile

import (
	"log/slog"

	"github.com/dayuer/esp-airprint/server/go/internal/store"
)

// Store 是 profile 对持久层的窄依赖。
type Store interface {
	GetProfile(scope, match string) (store.ProfileRow, bool, error)
}

// 同型号档案要几台独立设备结论一致才生效。
// 一个用户看错纸不能让同型号所有人打不出东西。
const ModelVoteThreshold = 3

// ModelKey 是 model 级档案的匹配键。
func ModelKey(vid, pid, model string) string {
	return vid + ":" + pid + ":" + model
}

// Lookup 分层查找，先命中先用：
//
//  1. 本机实测（按打印机序列号）
//  2. 同型号已验证（≥3 台独立设备一致，且未被标记分歧）
//  3. 开发者标记的权威档案
//  4. 保守默认值
//
// 开发者可以把某份档案 pin 为强制，pin 后压过第 1、2 层——这是给
// 「众包数据明显错了」准备的逃生门，正常不用。
func Lookup(st Store, serial, vid, pid, model string) Profile {
	mkey := ModelKey(vid, pid, model)

	// pin 优先：逃生门
	for _, c := range []struct{ scope, match string }{
		{store.ScopeSerial, serial},
		{store.ScopeModel, mkey},
	} {
		if row, ok, _ := st.GetProfile(c.scope, c.match); ok && row.Pinned {
			if p, err := Unmarshal([]byte(row.Body)); err == nil {
				p.Src = c.scope
				p.Serial = serial
				return p
			}
		}
	}

	if row, ok, _ := st.GetProfile(store.ScopeSerial, serial); ok && !row.Disputed {
		if p, err := Unmarshal([]byte(row.Body)); err == nil {
			p.Src = SrcSerial
			p.Serial = serial
			return p
		}
		slog.Warn("serial 级档案解析失败，降级", "serial", serial)
	}

	if row, ok, _ := st.GetProfile(store.ScopeModel, mkey); ok {
		switch {
		case row.Disputed:
			// 同型号出现过相反结论。不自动采用，回退到下一层等人工仲裁。
			slog.Info("机型档案存在分歧，已降级", "model", mkey)
		case row.Votes < ModelVoteThreshold:
			slog.Debug("机型档案票数不足", "model", mkey, "votes", row.Votes)
		default:
			if p, err := Unmarshal([]byte(row.Body)); err == nil {
				p.Src = SrcModel
				p.Serial = serial
				p.Match = vid + ":" + pid
				return p
			}
		}
	}

	if row, ok, _ := st.GetProfile(store.ScopeAuthoritative, mkey); ok {
		if p, err := Unmarshal([]byte(row.Body)); err == nil {
			p.Src = SrcAuthoritative
			p.Serial = serial
			p.Match = vid + ":" + pid
			return p
		}
	}

	d := Default(serial)
	d.Match = vid + ":" + pid
	return d
}
