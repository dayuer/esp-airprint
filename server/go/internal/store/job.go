package store

import (
	"database/sql"
	"errors"
	"time"
)

// Job 的 JSON 字段名对外可见（GET /api/status），必须与
// docs/API-cloud-print.md 一致——App 是照那份写的。
type Job struct {
	ID    string `json:"id"`
	Dev   string `json:"-"` // 不外泄：调用方本来就只能查自己的设备
	Name  string `json:"name"`
	Size  int64  `json:"size"`
	State string `json:"state"`
	Bytes int64  `json:"bytes"`
	Err   string `json:"err"`
	// Serial 是这份光栅为哪台打印机生成的。
	// 派发前必须校验它等于设备当前插着的那台——URF 按特定 dpi 和像素尺寸
	// 光栅，派给另一台就是一沓废纸，而且没有任何环节会发现。
	Serial  string `json:"serial"`
	Created int64  `json:"created"`
	Updated int64  `json:"updated"`
}

// 作业状态机：queued → downloading → done / failed
// 没有 rendering——服务端不渲染，上传校验通过即入队。
const (
	StateQueued      = "queued"
	StateDownloading = "downloading"
	StateDone        = "done"
	StateFailed      = "failed"
)

func now() int64 { return time.Now().Unix() }

const jobCols = `id,dev,name,size,state,bytes,err,serial,created,updated`

func (s *Store) InsertJob(j Job) error {
	if j.Created == 0 {
		j.Created = now()
	}
	if j.Updated == 0 {
		j.Updated = j.Created
	}
	s.wmu.Lock()
	defer s.wmu.Unlock()
	_, err := s.db.Exec(
		`INSERT INTO jobs(`+jobCols+`) VALUES(?,?,?,?,?,?,?,?,?,?)`,
		j.ID, j.Dev, j.Name, j.Size, j.State, j.Bytes, j.Err, j.Serial,
		j.Created, j.Updated)
	return err
}

func (s *Store) SetJobState(id, state string, bytes int64, errMsg string) error {
	s.wmu.Lock()
	defer s.wmu.Unlock()
	_, err := s.db.Exec(
		`UPDATE jobs SET state=?, bytes=?, err=?, updated=? WHERE id=?`,
		state, bytes, errMsg, now(), id)
	return err
}

// NextQueued 返回该设备上、为当前这台打印机排的最早一件。
//
// serial 过滤是必需的：用户换打印机后，为旧机器光栅的作业绝不能派给新机器。
// 那些作业留在队列里等旧机器插回来，不失败也不删。
//
// 不做「取出并标记」的原子操作：标记由 actor 在派发成功后自己做，
// 而 actor 是该设备唯一的写入者，不存在竞争。
func (s *Store) NextQueued(dev, serial string) (Job, bool, error) {
	row := s.db.QueryRow(
		`SELECT `+jobCols+` FROM jobs
		 WHERE dev=? AND serial=? AND state=? ORDER BY created LIMIT 1`,
		dev, serial, StateQueued)
	return scanJob(row)
}

func (s *Store) GetJob(id string) (Job, bool, error) {
	return scanJob(s.db.QueryRow(`SELECT `+jobCols+` FROM jobs WHERE id=?`, id))
}

func (s *Store) JobsForDevice(dev string, limit int) ([]Job, error) {
	rows, err := s.db.Query(
		`SELECT `+jobCols+` FROM jobs WHERE dev=? ORDER BY created DESC LIMIT ?`,
		dev, limit)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	out := []Job{}
	for rows.Next() {
		var j Job
		if err := scanJobInto(rows, &j); err != nil {
			return nil, err
		}
		out = append(out, j)
	}
	return out, rows.Err()
}

// QueuedCountByPrinter 统计该桥上每台打印机各有多少件在排队，
// 给 GET /api/device/{dev}/printers 的 queued_jobs 用。
// 用户看不到这个数就会以为打印失败了。
func (s *Store) QueuedCountByPrinter(dev string) (map[string]int, error) {
	rows, err := s.db.Query(
		`SELECT serial, COUNT(*) FROM jobs WHERE dev=? AND state=? GROUP BY serial`,
		dev, StateQueued)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	out := map[string]int{}
	for rows.Next() {
		var ser string
		var n int
		if err := rows.Scan(&ser, &n); err != nil {
			return nil, err
		}
		out[ser] = n
	}
	return out, rows.Err()
}

// ExpiredJobFiles 返回文件该被删掉的作业 id。
//
// 两条规则：按时长（done/failed 各有阈值），以及每设备保留上限。
// 只看已结束的作业——queued 永远不动，用户可能只是换了台打印机。
func (s *Store) ExpiredJobFiles(doneBefore, failBefore int64, perDev int) ([]string, error) {
	seen := map[string]bool{}
	var out []string

	collect := func(rows *sql.Rows, err error) error {
		if err != nil {
			return err
		}
		defer rows.Close()
		for rows.Next() {
			var id string
			if err := rows.Scan(&id); err != nil {
				return err
			}
			if !seen[id] {
				seen[id] = true
				out = append(out, id)
			}
		}
		return rows.Err()
	}

	if err := collect(s.db.Query(
		`SELECT id FROM jobs
		 WHERE (state=? AND updated < ?) OR (state=? AND updated < ?)`,
		StateDone, doneBefore, StateFailed, failBefore)); err != nil {
		return nil, err
	}

	// 每设备保留上限：按 updated 倒序，第 perDev 名之后的一律删。
	if err := collect(s.db.Query(
		`SELECT id FROM jobs AS j WHERE j.state IN (?,?) AND j.id NOT IN (
		   SELECT j2.id FROM jobs AS j2
		   WHERE j2.dev = j.dev AND j2.state IN (?,?)
		   ORDER BY j2.updated DESC LIMIT ?)`,
		StateDone, StateFailed, StateDone, StateFailed, perDev)); err != nil {
		return nil, err
	}
	return out, nil
}

type rowScanner interface{ Scan(...any) error }

func scanJobInto(r rowScanner, j *Job) error {
	return r.Scan(&j.ID, &j.Dev, &j.Name, &j.Size, &j.State,
		&j.Bytes, &j.Err, &j.Serial, &j.Created, &j.Updated)
}

func scanJob(r rowScanner) (Job, bool, error) {
	var j Job
	if err := scanJobInto(r, &j); err != nil {
		if errors.Is(err, sql.ErrNoRows) {
			return Job{}, false, nil
		}
		return Job{}, false, err
	}
	return j, true, nil
}
