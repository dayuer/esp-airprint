package store

import (
	"database/sql"
	"errors"
)

type Printer struct {
	Serial    string `json:"serial"`
	VID       string `json:"vid"`
	PID       string `json:"pid"`
	Make      string `json:"make"`
	Model     string `json:"model"`
	CMD       string `json:"cmd"`
	URFCaps   string `json:"urf_caps"`
	LastDev   string `json:"-"`
	FirstSeen int64  `json:"first_seen"`
	LastSeen  int64  `json:"last_seen"`
}

const printerCols = `serial,vid,pid,make,model,cmd,urf_caps,last_dev,first_seen,last_seen`

// UpsertPrinter 记下这台打印机。first_seen 只在首次写入时设，
// 之后的上报只刷新 last_seen 和可能变化的字段——机型档案会随探针改进而变化。
func (s *Store) UpsertPrinter(p Printer) error {
	if p.Serial == "" {
		return errors.New("store: 打印机序列号为空")
	}
	ts := now()
	s.wmu.Lock()
	defer s.wmu.Unlock()
	_, err := s.db.Exec(
		`INSERT INTO printers(`+printerCols+`) VALUES(?,?,?,?,?,?,?,?,?,?)
		 ON CONFLICT(serial) DO UPDATE SET
		   vid=excluded.vid, pid=excluded.pid, make=excluded.make,
		   model=excluded.model, cmd=excluded.cmd, urf_caps=excluded.urf_caps,
		   last_dev=excluded.last_dev, last_seen=excluded.last_seen`,
		p.Serial, p.VID, p.PID, p.Make, p.Model, p.CMD, p.URFCaps, p.LastDev, ts, ts)
	return err
}

func (s *Store) GetPrinter(serial string) (Printer, bool, error) {
	return scanPrinter(s.db.QueryRow(
		`SELECT `+printerCols+` FROM printers WHERE serial=?`, serial))
}

// PrintersOfDevice 列出这个桥见过的所有打印机，最近插过的排前面。
func (s *Store) PrintersOfDevice(dev string) ([]Printer, error) {
	rows, err := s.db.Query(
		`SELECT `+printerCols+` FROM printers WHERE last_dev=? ORDER BY last_seen DESC`, dev)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	out := []Printer{}
	for rows.Next() {
		var p Printer
		if err := scanPrinterInto(rows, &p); err != nil {
			return nil, err
		}
		out = append(out, p)
	}
	return out, rows.Err()
}

func scanPrinterInto(r rowScanner, p *Printer) error {
	return r.Scan(&p.Serial, &p.VID, &p.PID, &p.Make, &p.Model,
		&p.CMD, &p.URFCaps, &p.LastDev, &p.FirstSeen, &p.LastSeen)
}

func scanPrinter(r rowScanner) (Printer, bool, error) {
	var p Printer
	if err := scanPrinterInto(r, &p); err != nil {
		if errors.Is(err, sql.ErrNoRows) {
			return Printer{}, false, nil
		}
		return Printer{}, false, err
	}
	return p, true, nil
}

// 档案的匹配范围。
const (
	ScopeSerial        = "serial"
	ScopeModel         = "model"
	ScopeAuthoritative = "authoritative"
)

// ProfileRow 是一份存档的怪癖档案。Body 是下发给设备的 JSON 原文。
type ProfileRow struct {
	Scope     string
	Match     string
	Body      string
	MarginsMM string
	Votes     int
	Disputed  bool
	Pinned    bool
	Updated   int64
}

const profileCols = `scope,match,body,margins_mm,votes,disputed,pinned,updated`

func (s *Store) PutProfile(p ProfileRow) error {
	s.wmu.Lock()
	defer s.wmu.Unlock()
	_, err := s.db.Exec(
		`INSERT INTO profiles(`+profileCols+`) VALUES(?,?,?,?,?,?,?,?)
		 ON CONFLICT(scope,match) DO UPDATE SET
		   body=excluded.body, margins_mm=excluded.margins_mm,
		   votes=excluded.votes, disputed=excluded.disputed,
		   pinned=excluded.pinned, updated=excluded.updated`,
		p.Scope, p.Match, p.Body, p.MarginsMM, p.Votes,
		b2i(p.Disputed), b2i(p.Pinned), now())
	return err
}

func (s *Store) GetProfile(scope, match string) (ProfileRow, bool, error) {
	var p ProfileRow
	var disputed, pinned int
	err := s.db.QueryRow(
		`SELECT `+profileCols+` FROM profiles WHERE scope=? AND match=?`, scope, match).
		Scan(&p.Scope, &p.Match, &p.Body, &p.MarginsMM, &p.Votes,
			&disputed, &pinned, &p.Updated)
	if err != nil {
		if errors.Is(err, sql.ErrNoRows) {
			return ProfileRow{}, false, nil
		}
		return ProfileRow{}, false, err
	}
	p.Disputed, p.Pinned = disputed != 0, pinned != 0
	return p, true, nil
}

func b2i(b bool) int {
	if b {
		return 1
	}
	return 0
}
