package broker

import (
	"encoding/json"
	"log/slog"
	"strings"

	mqtt "github.com/mochi-mqtt/server/v2"
	"github.com/mochi-mqtt/server/v2/packets"

	"github.com/dayuer/esp-airprint/server/go/internal/auth"
	"github.com/dayuer/esp-airprint/server/go/internal/device"
)

// Router 是 broker 对 registry 的窄依赖。
type Router interface {
	Send(dev string, m device.Msg)
}

type Hook struct {
	mqtt.HookBase
	v      *auth.Verifier
	mem    *Membership
	router Router
	// ids 记录每个连接校验通过的身份，ACL 判定时直接取，不重复跑 argon2。
	ids *idTable
}

func (h *Hook) ID() string { return "airprint-auth" }

func (h *Hook) Provides(b byte) bool {
	switch b {
	case mqtt.OnConnectAuthenticate, mqtt.OnACLCheck, mqtt.OnPublished, mqtt.OnDisconnect:
		return true
	}
	return false
}

func (h *Hook) OnConnectAuthenticate(cl *mqtt.Client, pk packets.Packet) bool {
	user := string(pk.Connect.Username)
	id, err := h.v.Verify(string(pk.Connect.Password))
	if err != nil {
		slog.Warn("MQTT 认证失败", "username", user, "remote", cl.Net.Remote)
		return false
	}
	// device 角色的 username 必须等于它自己的 dev——防止拿 A 的密钥冒充 B 连接。
	if id.Role == auth.RoleDevice && id.Dev != user {
		slog.Warn("MQTT username 与密钥不符", "username", user, "key_dev", id.Dev)
		return false
	}
	h.ids.put(cl.ID, id)
	return true
}

func (h *Hook) OnACLCheck(cl *mqtt.Client, topic string, write bool) bool {
	id, ok := h.ids.get(cl.ID)
	if !ok {
		return false
	}
	return auth.ACLAllowed(id, topic, write, h.mem)
}

func (h *Hook) OnDisconnect(cl *mqtt.Client, err error, expire bool) {
	h.ids.drop(cl.ID)
}

func (h *Hook) OnPublished(cl *mqtt.Client, pk packets.Packet) {
	if strings.HasSuffix(pk.TopicName, "/status") {
		routeStatus(h.router, pk.TopicName, pk.Payload)
	}
}

// statusPayload 是设备心跳的结构。字段定义见 docs/API-cloud-print.md 第 3.6 节。
type statusPayload struct {
	Job    string          `json:"job"`
	State  string          `json:"state"`
	Bytes  int64           `json:"bytes"`
	Err    string          `json:"err"`
	Serial string          `json:"serial"`
	Prn    json.RawMessage `json:"prn"`
}

// routeStatus 把一条 status 消息翻译成 actor 消息。
//
// dev 一律从 topic 取，不用 payload 里的——payload 是设备自报的，
// topic 是经过 ACL 校验的，只有后者可信。
func routeStatus(r Router, topic string, payload []byte) {
	parts := strings.Split(topic, "/")
	if len(parts) != 3 || parts[0] != "printer" {
		return
	}
	dev := parts[1]

	var p statusPayload
	if err := json.Unmarshal(payload, &p); err != nil {
		slog.Warn("status payload 非法 JSON", "dev", dev)
		return
	}

	m := device.Msg{Serial: p.Serial, Printer: p.Prn, JobID: p.Job,
		Bytes: p.Bytes, Err: p.Err}
	switch p.State {
	case "done":
		m.Kind = device.KindJobDone
	case "failed":
		m.Kind = device.KindJobFailed
	case "downloading":
		m.Kind = device.KindDownloading
	case "offline":
		m.Kind = device.KindHeartbeat
		m.Serial = "" // 掉线了就没有打印机
	default:
		m.Kind = device.KindHeartbeat
	}
	r.Send(dev, m)
}
