// Package httpapi 是纯 JSON API，没有网页。
//
// 行为以 docs/API-cloud-print.md 为准——固件和 App 是照那份写的，
// 不一致时改这里，不是改文档。
package httpapi

import (
	"encoding/json"
	"log/slog"
	"net"
	"net/http"
	"strings"

	"github.com/dayuer/esp-airprint/server/go/internal/auth"
	"github.com/dayuer/esp-airprint/server/go/internal/config"
	"github.com/dayuer/esp-airprint/server/go/internal/device"
	"github.com/dayuer/esp-airprint/server/go/internal/registry"
	"github.com/dayuer/esp-airprint/server/go/internal/store"
)

// Invalidator 是 ACL 缓存的失效入口（由 broker.Membership 实现）。
type Invalidator interface {
	Invalidate(userID string)
}

// Publisher 是 httpapi 对 MQTT 的窄依赖：只用来下发怪癖档案。
type Publisher interface {
	PublishProfile(dev string, profile []byte) error
}

type API struct {
	cfg   *config.Config
	store *store.Store
	v     *auth.Verifier
	phone *auth.PhoneBox
	sms   *auth.SMS
	reg   *registry.Registry
	mem   Invalidator
	pub   Publisher
}

func New(cfg *config.Config, st *store.Store, v *auth.Verifier, pb *auth.PhoneBox,
	sms *auth.SMS, reg *registry.Registry, mem Invalidator, pub Publisher) *API {
	return &API{cfg: cfg, store: st, v: v, phone: pb, sms: sms,
		reg: reg, mem: mem, pub: pub}
}

func (a *API) Handler() http.Handler {
	mux := http.NewServeMux()

	// 不需要身份
	mux.HandleFunc("POST /api/auth/sms", a.handleSMS)
	mux.HandleFunc("POST /api/auth/verify", a.handleVerify)

	// app 角色
	mux.Handle("POST /api/auth/logout", a.requireApp(a.handleLogout))
	mux.Handle("POST /api/account/delete", a.requireApp(a.handleAccountDelete))
	mux.Handle("POST /api/device/enroll", a.requireApp(a.handleEnroll))
	mux.Handle("POST /api/device/{dev}/unbind", a.requireApp(a.handleUnbind))
	mux.Handle("POST /api/print", a.requireApp(a.handlePrint))
	mux.Handle("GET /api/device/{dev}/render-profile", a.requireApp(a.handleRenderProfile))
	mux.Handle("GET /api/device/{dev}/printers", a.requireApp(a.handlePrinters))
	mux.Handle("GET /api/device/{dev}/printer", a.requireApp(a.handlePrinter))

	// device 角色
	mux.Handle("GET /api/job/{id}/data", a.requireDevice(a.handleJobData))
	mux.Handle("POST /api/device/{dev}/ident", a.requireDevice(a.handleIdent))

	// 两种角色都行
	mux.Handle("GET /api/status", a.requireAny(a.handleStatus))

	return mux
}

type handlerFn func(http.ResponseWriter, *http.Request, auth.Identity)

func (a *API) requireApp(fn handlerFn) http.Handler    { return a.guard(fn, auth.RoleApp) }
func (a *API) requireDevice(fn handlerFn) http.Handler { return a.guard(fn, auth.RoleDevice) }
func (a *API) requireAny(fn handlerFn) http.Handler    { return a.guard(fn, "") }

func (a *API) guard(fn handlerFn, want auth.Role) http.Handler {
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		tok, ok := bearerToken(r)
		if !ok {
			unauthorized(w)
			return
		}
		id, err := a.v.Verify(tok)
		if err != nil {
			// 刻意不区分「不存在」和「密钥错」——不给探测者提供信息。
			unauthorized(w)
			return
		}
		if want != "" && id.Role != want {
			unauthorized(w)
			return
		}
		fn(w, r, id)
	})
}

// ownDevice 是 app 角色操作某台设备前的统一校验。
func (a *API) ownDevice(id auth.Identity, dev string) bool {
	if dev == "" {
		return false
	}
	owner, bound, err := a.store.OwnerOfDevice(dev)
	return err == nil && bound && owner == id.UserID
}

func bearerToken(r *http.Request) (string, bool) {
	h := r.Header.Get("Authorization")
	const p = "Bearer "
	if len(h) <= len(p) || !strings.EqualFold(h[:len(p)], p) {
		return "", false
	}
	return h[len(p):], true
}

func unauthorized(w http.ResponseWriter) {
	writeJSON(w, http.StatusUnauthorized, map[string]string{"e": "unauthorized"})
}

func writeJSON(w http.ResponseWriter, code int, v any) {
	w.Header().Set("Content-Type", "application/json; charset=utf-8")
	w.WriteHeader(code)
	json.NewEncoder(w).Encode(v)
}

func fail(w http.ResponseWriter, code int, e, detail string) {
	m := map[string]string{"e": e}
	if detail != "" {
		m["detail"] = detail
	}
	writeJSON(w, code, m)
}

func clientIP(r *http.Request) string {
	host, _, err := net.SplitHostPort(r.RemoteAddr)
	if err != nil {
		return r.RemoteAddr
	}
	return host
}

func (a *API) devMsg(dev string, m device.Msg) {
	slog.Debug("投递 actor 消息", "dev", dev, "kind", m.Kind)
	a.reg.Send(dev, m)
}
