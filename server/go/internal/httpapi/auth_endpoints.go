package httpapi

import (
	"encoding/json"
	"errors"
	"net/http"
	"os"
	"path/filepath"

	"github.com/dayuer/stickbox/server/go/internal/auth"
)

func decodeJSON(w http.ResponseWriter, r *http.Request, v any) bool {
	if err := json.NewDecoder(http.MaxBytesReader(w, r.Body, 1<<12)).Decode(v); err != nil {
		fail(w, 400, "bad request", "")
		return false
	}
	return true
}

func (a *API) handleSMS(w http.ResponseWriter, r *http.Request) {
	var in struct {
		Phone string `json:"phone"`
	}
	if !decodeJSON(w, r, &in) {
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
		Phone  string `json:"phone"`
		Code   string `json:"code"`
		Device string `json:"device"`
	}
	if !decodeJSON(w, r, &in) {
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
