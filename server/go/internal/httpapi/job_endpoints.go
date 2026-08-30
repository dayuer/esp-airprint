package httpapi

import (
	"crypto/rand"
	"encoding/hex"
	"encoding/json"
	"io"
	"net/http"
	"os"
	"path/filepath"

	"github.com/dayuer/esp-airprint/server/go/internal/auth"
	"github.com/dayuer/esp-airprint/server/go/internal/device"
	"github.com/dayuer/esp-airprint/server/go/internal/raster"
	"github.com/dayuer/esp-airprint/server/go/internal/store"
)

const maxJobBytes = 200 << 20 // URF 是光栅，整页照片单页就能到 15MB

func newJobID() string {
	b := make([]byte, 6)
	if _, err := rand.Read(b); err != nil {
		panic(err)
	}
	return hex.EncodeToString(b)
}

// handlePrint 收一份已光栅的 URF/PWG。服务端不渲染，只校验、落盘、入队。
func (a *API) handlePrint(w http.ResponseWriter, r *http.Request, id auth.Identity) {
	dev := r.Header.Get("X-Device")
	serial := r.Header.Get("X-Printer-Serial")
	if dev == "" || serial == "" {
		fail(w, 400, "bad request", "X-Device 和 X-Printer-Serial 都必填")
		return
	}
	if !a.ownDevice(id, dev) {
		fail(w, 403, "forbidden", "")
		return
	}
	format, ok := raster.FormatFromContentType(r.Header.Get("Content-Type"))
	if !ok {
		fail(w, 415, "unsupported media type",
			"只接受 image/urf 与 image/pwg-raster；服务端不渲染")
		return
	}

	body := http.MaxBytesReader(w, r.Body, maxJobBytes)

	// 先读头部做校验，再流式写盘——不为了校验把整份读进内存。
	head := make([]byte, 4096)
	n, _ := io.ReadFull(body, head)
	head = head[:n]
	info, verr := raster.Verify(format, head)
	if verr != nil {
		fail(w, 400, "bad raster", verr.Error())
		return
	}

	jid := newJobID()
	path := filepath.Join(a.cfg.JobsDir(), jid+".urf")
	f, err := os.Create(path)
	if err != nil {
		fail(w, 500, "server error", "")
		return
	}
	written, err := f.Write(head)
	if err == nil {
		var m int64
		m, err = io.Copy(f, body)
		written += int(m)
	}
	f.Close()
	if err != nil {
		os.Remove(path)
		fail(w, 413, "too large", "")
		return
	}

	if err := a.store.InsertJob(store.Job{
		ID: jid, Dev: dev, Name: r.Header.Get("X-Filename"),
		Size: int64(written), State: store.StateQueued, Serial: serial,
	}); err != nil {
		os.Remove(path)
		fail(w, 500, "server error", "")
		return
	}

	// 叫醒 actor。设备不在线或插着别的打印机时，作业就留在队列里等着。
	a.devMsg(dev, device.Msg{Kind: device.KindWake})

	attached := false
	if act := a.reg.Actor(dev); act != nil {
		attached = act.Serial() == serial
	}
	writeJSON(w, 200, map[string]any{
		"job": jid, "size": written, "pages": info.Pages,
		"state": store.StateQueued, "printer_attached": attached,
	})
}

// handleJobData 是设备取件。v1 完全没有校验，任何人能下载任何作业。
func (a *API) handleJobData(w http.ResponseWriter, r *http.Request, id auth.Identity) {
	jid := r.PathValue("id")
	j, ok, err := a.store.GetJob(jid)
	if err != nil {
		fail(w, 500, "server error", "")
		return
	}
	if !ok {
		fail(w, 404, "not found", "")
		return
	}
	if j.Dev != id.Dev {
		fail(w, 403, "forbidden", "")
		return
	}
	f, err := os.Open(filepath.Join(a.cfg.JobsDir(), jid+".urf"))
	if err != nil {
		fail(w, 404, "not found", "作业文件已被清理")
		return
	}
	defer f.Close()

	// 请求一到就续超时：只要在传，就不会被 180 秒误判。
	a.store.SetJobState(jid, store.StateDownloading, 0, "")
	a.devMsg(id.Dev, device.Msg{Kind: device.KindDownloading, JobID: jid})

	fi, err := f.Stat()
	if err != nil {
		fail(w, 500, "server error", "")
		return
	}
	// ServeContent 顺带把 Range 处理了——固件当前不用，但服务端先支持着，
	// 将来做断点续传不用改服务端。
	http.ServeContent(w, r, jid+".urf", fi.ModTime(), f)
}

func (a *API) handleIdent(w http.ResponseWriter, r *http.Request, id auth.Identity) {
	dev := r.PathValue("dev")
	if dev != id.Dev {
		fail(w, 403, "forbidden", "")
		return
	}
	raw, err := io.ReadAll(http.MaxBytesReader(w, r.Body, 256<<10))
	if err != nil {
		fail(w, 413, "too large", "")
		return
	}
	var probe map[string]any
	if err := json.Unmarshal(raw, &probe); err != nil {
		fail(w, 400, "bad json", "")
		return
	}
	dir := filepath.Join(a.cfg.IdentsDir(), dev)
	if err := os.MkdirAll(dir, 0o755); err != nil {
		fail(w, 500, "server error", "")
		return
	}
	if err := os.WriteFile(filepath.Join(dir, "latest.json"), raw, 0o644); err != nil {
		fail(w, 500, "server error", "")
		return
	}
	writeJSON(w, 200, map[string]int{"ok": 1})
}
