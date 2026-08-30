package httpapi

import (
	"net/http"

	"github.com/dayuer/stickbox/server/go/internal/auth"
)

// handleDevices 实现 docs/API-cloud-print.md 4.5b：GET /api/devices。
//
// 为什么需要这个端点：其余所有设备端点都要求调用方**已经知道** {dev}，而
// dev 唯一的来源是配网时从 SoftAP 读到的 MAC。没有它，App 的设备列表只能
// 存在手机本地——用户换手机或重装 App 之后，账号还在、设备还绑着，但 App
// 再也找不到它，只能把每台设备重新配一遍网。
//
// 不带 X-Device：这个端点就是用来得知有哪些 dev 的。
func (a *API) handleDevices(w http.ResponseWriter, r *http.Request, id auth.Identity) {
	devs, err := a.store.DevicesOfUser(id.UserID)
	if err != nil {
		fail(w, 500, "server error", "")
		return
	}

	type printerBrief struct {
		Serial   string `json:"serial"`
		Make     string `json:"make"`
		Model    string `json:"model"`
		Attached bool   `json:"attached"`
	}
	type row struct {
		Dev        string        `json:"dev"`
		Name       string        `json:"name"`
		Online     bool          `json:"online"`
		Seen       int64         `json:"seen"`
		State      string        `json:"state"`
		Bound      int64         `json:"bound"`
		Printer    *printerBrief `json:"printer"`
		QueuedJobs int           `json:"queued_jobs"`
	}

	// 一台设备都没有时返回 {"devices":[]}，不是 404——「没有设备」是正常状态，
	// 不是错误。App 靠这个区分「新用户」和「出问题了」。
	out := []row{}
	for _, dev := range devs {
		it := row{Dev: dev, Printer: nil}

		if meta, ok, err := a.store.DeviceMeta(dev); err == nil && ok {
			it.Name = meta.Name
			it.Bound = meta.Bound
		}

		// 在线与否以 actor 在不在为准，跟 4.6 的 /api/status 用同一个判据。
		if act := a.reg.Actor(dev); act != nil {
			it.Online = true
			it.State = "ready"
			if serial := act.Serial(); serial != "" {
				b := printerBrief{Serial: serial, Attached: true}
				if p, ok, err := a.store.GetPrinter(serial); err == nil && ok {
					b.Make, b.Model = p.Make, p.Model
				}
				it.Printer = &b
			}
		} else {
			it.State = "offline"
		}

		// 没插着的打印机也可能有排队作业——用户得知道「插上 XXX 就会自动打印」。
		if counts, err := a.store.QueuedCountByPrinter(dev); err == nil {
			for _, n := range counts {
				it.QueuedJobs += n
			}
		}

		out = append(out, it)
	}

	writeJSON(w, 200, map[string]any{"devices": out})
}
