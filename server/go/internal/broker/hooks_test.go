package broker

import (
	"encoding/json"
	"testing"

	"github.com/dayuer/esp-airprint/server/go/internal/device"
)

type capturedMsg struct {
	dev string
	msg device.Msg
}

type fakeRouter struct{ got []capturedMsg }

func (f *fakeRouter) Send(dev string, m device.Msg) {
	f.got = append(f.got, capturedMsg{dev, m})
}

func TestRouteStatusHeartbeat(t *testing.T) {
	r := &fakeRouter{}
	payload, _ := json.Marshal(map[string]any{
		"dev": "aaa", "job": "", "state": "ready", "bytes": 0,
		"serial": "PA",
		"prn":    map[string]any{"display": "Ready"},
	})
	routeStatus(r, "printer/aaa/status", payload)

	if len(r.got) != 1 {
		t.Fatalf("投递了 %d 条", len(r.got))
	}
	m := r.got[0].msg
	if m.Kind != device.KindHeartbeat || m.Serial != "PA" {
		t.Errorf("消息 = %+v，期望 Heartbeat + serial=PA", m)
	}
	if len(m.Printer) == 0 {
		t.Error("prn 原文没带上，/api/status 就没东西可显示")
	}
}

// done 必须带 job 字段，否则服务端要等 180 秒超时才知道这件结束了。
func TestRouteStatusDone(t *testing.T) {
	r := &fakeRouter{}
	routeStatus(r, "printer/aaa/status",
		[]byte(`{"dev":"aaa","job":"j1","state":"done","bytes":4096,"serial":"PA"}`))

	m := r.got[0].msg
	if m.Kind != device.KindJobDone || m.JobID != "j1" || m.Bytes != 4096 {
		t.Errorf("消息 = %+v", m)
	}
}

func TestRouteStatusFailedCarriesError(t *testing.T) {
	r := &fakeRouter{}
	routeStatus(r, "printer/aaa/status",
		[]byte(`{"dev":"aaa","job":"j1","state":"failed","err":"USB 超时"}`))
	m := r.got[0].msg
	if m.Kind != device.KindJobFailed || m.Err != "USB 超时" {
		t.Errorf("消息 = %+v", m)
	}
}

func TestRouteStatusDownloading(t *testing.T) {
	r := &fakeRouter{}
	routeStatus(r, "printer/aaa/status",
		[]byte(`{"dev":"aaa","job":"j1","state":"downloading","serial":"PA"}`))
	if m := r.got[0].msg; m.Kind != device.KindDownloading || m.JobID != "j1" {
		t.Errorf("消息 = %+v", m)
	}
}

// LWT：设备掉线时 broker 代发，serial 要清空。
func TestRouteStatusOffline(t *testing.T) {
	r := &fakeRouter{}
	routeStatus(r, "printer/aaa/status", []byte(`{"dev":"aaa","state":"offline"}`))
	m := r.got[0].msg
	if m.Kind != device.KindHeartbeat || m.Serial != "" {
		t.Errorf("消息 = %+v，offline 时 serial 应为空", m)
	}
}

// 畸形 JSON 不能让 broker 崩，也不能投出垃圾消息。
func TestRouteStatusIgnoresGarbage(t *testing.T) {
	r := &fakeRouter{}
	routeStatus(r, "printer/aaa/status", []byte("not json at all"))
	if len(r.got) != 0 {
		t.Errorf("畸形 payload 投出了 %d 条消息", len(r.got))
	}
}

// topic 里的 dev 是权威，payload 里的 dev 是设备自报的，不能信。
func TestRouteStatusTrustsTopicNotPayload(t *testing.T) {
	r := &fakeRouter{}
	routeStatus(r, "printer/aaa/status", []byte(`{"dev":"bbb","state":"ready"}`))
	if len(r.got) != 1 || r.got[0].dev != "aaa" {
		t.Errorf("路由到了 %+v，必须以 topic 为准", r.got)
	}
}

func TestRouteStatusIgnoresBadTopic(t *testing.T) {
	r := &fakeRouter{}
	for _, topic := range []string{"status", "printer/status", "x/aaa/status"} {
		routeStatus(r, topic, []byte(`{"state":"ready"}`))
	}
	if len(r.got) != 0 {
		t.Errorf("非法 topic 投出了 %d 条消息", len(r.got))
	}
}
