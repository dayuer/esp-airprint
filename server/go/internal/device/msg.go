package device

type Kind int

const (
	KindHeartbeat   Kind = iota // 设备心跳（含打印机面板状态与当前 serial）
	KindJobDone                 // 作业完成回执
	KindJobFailed               // 作业失败回执
	KindDownloading             // 设备开始取件，用于续超时
	KindWake                    // 有新作业入队 / 巡检唤醒
	KindTick                    // 时间推进，测试里由外部驱动
)

type Msg struct {
	Kind  Kind
	JobID string
	Bytes int64
	Err   string
	// Serial 是心跳里上报的、当前插着的打印机序列号。拔掉时是空串。
	// 用户换打印机是常态，actor 靠它决定派哪些作业。
	Serial string
	// Printer 是设备上报的打印机面板状态原文，原样存下给 /api/status 用。
	Printer []byte
}
