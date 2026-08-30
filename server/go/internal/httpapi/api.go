// Package httpapi 是纯 JSON API，没有网页。
//
// 行为以 docs/API-cloud-print.md 为准——固件和 App 是照那份写的，
// 不一致时改这里，不是改文档。
package httpapi

import (
	"encoding/json"
	"io"
	"log/slog"
	"net"
	"net/http"
	"strconv"
	"strings"

	"github.com/dayuer/stickbox/server/go/internal/auth"
	"github.com/dayuer/stickbox/server/go/internal/config"
	"github.com/dayuer/stickbox/server/go/internal/device"
	"github.com/dayuer/stickbox/server/go/internal/registry"
	"github.com/dayuer/stickbox/server/go/internal/store"
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
	mux.Handle("GET /api/devices", a.requireApp(a.handleDevices))
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

	return drainBody(mux)
}

// 单次最多排掉多少请求体。设备自己的请求都 <4KB（SERVER-REQUIREMENTS 1.1），
// 1MB 已经很宽松；再大就不值得为一个错误响应去读，直接关连接。
const maxDrainBytes = 1 << 20

// drainBody 保证带请求体的请求在响应之后把体读完。
//
// 这条是真踩过的故障（SERVER-REQUIREMENTS 3.1）：旧服务端对 POST 直接返回
// 404 而不读 body，HTTP/1.1 长连接下连接失步——设备已经把 7KB 发出去了、
// 服务端不收，客户端卡在那儿等一个读不完的响应，34 秒后连 MQTT 都跟着写
// 超时，最后设备重启。
//
// 放在中间件里而不是每个 handler 各自处理：错误分支太多，漏一个就复现。
func drainBody(next http.Handler) http.Handler {
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		next.ServeHTTP(w, r)
		if r.Body == nil {
			return
		}
		n, err := io.Copy(io.Discard, io.LimitReader(r.Body, maxDrainBytes))
		if err != nil || n == maxDrainBytes {
			// 排不完就别装作长连接还能用，让它关掉重开
			w.Header().Set("Connection", "close")
		}
	})
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

// writeJSON 先编码再发头，为的是带上 Content-Length。
//
// 直接 WriteHeader 再 Encode 的话 Go 会用 chunked 传输编码，而设备用的
// esp_http_client **不解 chunked 响应体**（SERVER-REQUIREMENTS 3.3）。
func writeJSON(w http.ResponseWriter, code int, v any) {
	b, err := json.Marshal(v)
	if err != nil {
		http.Error(w, `{"e":"server error"}`, http.StatusInternalServerError)
		return
	}
	b = append(b, '\n')
	w.Header().Set("Content-Type", "application/json; charset=utf-8")
	w.Header().Set("Content-Length", strconv.Itoa(len(b)))
	w.WriteHeader(code)
	w.Write(b)
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
