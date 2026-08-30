package device

import "sync"

// snapshot 让 HTTP 侧能读到 actor 的当前状态，而不必往 mailbox 里投消息等回复。
//
// actor 自己是单 goroutine 无锁的；这里的锁只保护「跨 goroutine 读」这一件事，
// 不参与 actor 内部的任何判断。
type snapshot struct {
	mu      sync.RWMutex
	serial  string
	printer []byte
}

func newSnapshot() *snapshot { return &snapshot{} }

func (s *snapshot) set(serial string, printer []byte) {
	s.mu.Lock()
	s.serial, s.printer = serial, printer
	s.mu.Unlock()
}

// Serial 返回当前插着的打印机序列号，给 /api/device/{dev}/printers 用。
func (a *Actor) Serial() string {
	a.snap.mu.RLock()
	defer a.snap.mu.RUnlock()
	return a.snap.serial
}

// Printer 返回最近一次心跳里的打印机面板状态原文，给 /api/status 用。
func (a *Actor) Printer() []byte {
	a.snap.mu.RLock()
	defer a.snap.mu.RUnlock()
	return a.snap.printer
}
