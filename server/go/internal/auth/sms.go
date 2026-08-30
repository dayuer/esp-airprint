package auth

import (
	"context"
	"crypto/rand"
	"crypto/sha256"
	"encoding/hex"
	"errors"
	"math/big"
	"sync"
	"time"

	"github.com/dayuer/esp-airprint/server/go/internal/store"
)

// Sender 是短信服务商的抽象。测试用假实现——单测和集成测试绝不真发短信。
type Sender interface {
	Send(ctx context.Context, phone, code string) error
}

// 四道防刷闸。短信每条都是真金白银，被刷等于烧钱加骚扰他人。
const (
	codeTTL       = 5 * time.Minute
	resendGap     = 60 * time.Second
	dailyPerPhone = 10
	hourlyPerIP   = 20
	maxAttempts   = 5
)

var (
	ErrTooFrequent = errors.New("发送过于频繁，请稍后再试")
	ErrDailyCap    = errors.New("今日发送次数已达上限")
	ErrIPCap       = errors.New("该网络发送次数已达上限")
	ErrBadCode     = errors.New("验证码错误或已失效")
)

type SMSStore interface {
	SMSCode(phoneHMAC string) (store.SMSCode, bool, error)
	PutSMSCode(c store.SMSCode) error
	BumpSMSAttempts(phoneHMAC string) error
	ConsumeSMSCode(phoneHMAC string) error
}

type SMS struct {
	st     SMSStore
	sender Sender
	now    func() time.Time

	mu sync.Mutex
	ip map[string][]time.Time // IP → 最近一小时的发送时刻
}

func NewSMS(st SMSStore, sender Sender, now func() time.Time) *SMS {
	if now == nil {
		now = time.Now
	}
	return &SMS{st: st, sender: sender, now: now, ip: map[string][]time.Time{}}
}

// Issue 发一条验证码。四道闸按「先便宜后昂贵」的顺序查：
// 内存里的 IP 计数最便宜，真发短信最贵。
func (s *SMS) Issue(ctx context.Context, phone, phoneHMAC, ip string) error {
	if err := s.checkIP(ip); err != nil {
		return err
	}
	rec, found, err := s.st.SMSCode(phoneHMAC)
	if err != nil {
		return err
	}
	now := s.now()
	dayStart, dayCount := now.Unix(), 0
	if found {
		if now.Sub(time.Unix(rec.SentAt, 0)) < resendGap {
			return ErrTooFrequent
		}
		if now.Unix()-rec.DayStart < 86400 {
			dayStart, dayCount = rec.DayStart, rec.DayCount
		}
		if dayCount >= dailyPerPhone {
			return ErrDailyCap
		}
	}

	code := randomCode()
	if err := s.st.PutSMSCode(store.SMSCode{
		PhoneHMAC: phoneHMAC,
		CodeHash:  hashCode(code),
		Expires:   now.Add(codeTTL).Unix(),
		SentAt:    now.Unix(),
		DayStart:  dayStart,
		DayCount:  dayCount + 1,
	}); err != nil {
		return err
	}
	if err := s.sender.Send(ctx, phone, code); err != nil {
		return err
	}
	s.noteIP(ip)
	return nil
}

// Verify 校验并作废。成功即一次性失效，防重放。
func (s *SMS) Verify(phoneHMAC, code string) error {
	rec, found, err := s.st.SMSCode(phoneHMAC)
	if err != nil {
		return err
	}
	if !found || s.now().Unix() > rec.Expires || rec.Attempts >= maxAttempts {
		return ErrBadCode
	}
	if rec.CodeHash != hashCode(code) {
		s.st.BumpSMSAttempts(phoneHMAC)
		return ErrBadCode
	}
	return s.st.ConsumeSMSCode(phoneHMAC)
}

func (s *SMS) checkIP(ip string) error {
	s.mu.Lock()
	defer s.mu.Unlock()
	cut := s.now().Add(-time.Hour)
	kept := s.ip[ip][:0]
	for _, t := range s.ip[ip] {
		if t.After(cut) {
			kept = append(kept, t)
		}
	}
	s.ip[ip] = kept
	if len(kept) >= hourlyPerIP {
		return ErrIPCap
	}
	return nil
}

func (s *SMS) noteIP(ip string) {
	s.mu.Lock()
	s.ip[ip] = append(s.ip[ip], s.now())
	s.mu.Unlock()
}

func randomCode() string {
	n, err := rand.Int(rand.Reader, big.NewInt(1_000_000))
	if err != nil {
		panic(err)
	}
	s := n.String()
	for len(s) < 6 {
		s = "0" + s
	}
	return s
}

func hashCode(code string) string {
	sum := sha256.Sum256([]byte(code))
	return hex.EncodeToString(sum[:])
}
