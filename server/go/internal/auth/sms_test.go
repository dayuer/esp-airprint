package auth

import (
	"context"
	"fmt"
	"testing"
	"time"

	"github.com/dayuer/esp-airprint/server/go/internal/store"
)

type fakeSender struct{ sent []string }

func (f *fakeSender) Send(ctx context.Context, phone, code string) error {
	f.sent = append(f.sent, code)
	return nil
}

func newSMS(t *testing.T) (*SMS, *fakeSender, *time.Time) {
	t.Helper()
	st, err := store.Open(t.TempDir() + "/jobs.db")
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { st.Close() })
	f := &fakeSender{}
	clk := time.Unix(1_000_000, 0)
	return NewSMS(st, f, func() time.Time { return clk }), f, &clk
}

func TestIssueThenVerify(t *testing.T) {
	s, f, _ := newSMS(t)
	if err := s.Issue(context.Background(), "13800008888", "H", "1.2.3.4"); err != nil {
		t.Fatal(err)
	}
	if len(f.sent) != 1 || len(f.sent[0]) != 6 {
		t.Fatalf("发出的验证码 = %v，期望一条 6 位", f.sent)
	}
	if err := s.Verify("H", f.sent[0]); err != nil {
		t.Fatalf("正确验证码校验失败：%v", err)
	}
}

// 验证码一次性：用过就作废，防重放。
func TestVerifyIsSingleUse(t *testing.T) {
	s, f, _ := newSMS(t)
	s.Issue(context.Background(), "13800008888", "H", "1.2.3.4")
	code := f.sent[0]
	s.Verify("H", code)
	if err := s.Verify("H", code); err == nil {
		t.Error("同一验证码用了第二次")
	}
}

func TestVerifyRejectsWrongAndExpired(t *testing.T) {
	s, f, clk := newSMS(t)
	s.Issue(context.Background(), "13800008888", "H", "1.2.3.4")
	if err := s.Verify("H", "000000"); err == nil {
		t.Error("错误验证码应被拒")
	}
	*clk = clk.Add(6 * time.Minute)
	if err := s.Verify("H", f.sent[0]); err == nil {
		t.Error("过期验证码应被拒（TTL 5 分钟）")
	}
}

// 单个验证码最多试 5 次，超了整条作废——防暴力猜 6 位数字。
func TestVerifyLocksAfterFiveAttempts(t *testing.T) {
	s, f, _ := newSMS(t)
	s.Issue(context.Background(), "13800008888", "H", "1.2.3.4")
	for i := 0; i < 5; i++ {
		s.Verify("H", "000000")
	}
	if err := s.Verify("H", f.sent[0]); err == nil {
		t.Error("超过尝试次数后连正确验证码也该被拒")
	}
}

// 闸一：同号码 60 秒内不得重发。
func TestIssueRateLimitPerPhone(t *testing.T) {
	s, _, clk := newSMS(t)
	ctx := context.Background()
	if err := s.Issue(ctx, "13800008888", "H", "1.2.3.4"); err != nil {
		t.Fatal(err)
	}
	if err := s.Issue(ctx, "13800008888", "H", "1.2.3.4"); err == nil {
		t.Error("60 秒内重发应被拒")
	}
	*clk = clk.Add(61 * time.Second)
	if err := s.Issue(ctx, "13800008888", "H", "1.2.3.4"); err != nil {
		t.Errorf("61 秒后应允许重发：%v", err)
	}
}

// 闸二：同号码每天最多 10 条。
func TestIssueDailyCapPerPhone(t *testing.T) {
	s, _, clk := newSMS(t)
	ctx := context.Background()
	for i := 0; i < 10; i++ {
		if err := s.Issue(ctx, "13800008888", "H", "1.2.3.4"); err != nil {
			t.Fatalf("第 %d 条就被拒了：%v", i+1, err)
		}
		*clk = clk.Add(61 * time.Second)
	}
	if err := s.Issue(ctx, "13800008888", "H", "1.2.3.4"); err == nil {
		t.Error("第 11 条应被拒")
	}
	// 跨过一天后重新计数
	*clk = clk.Add(25 * time.Hour)
	if err := s.Issue(ctx, "13800008888", "H", "1.2.3.4"); err != nil {
		t.Errorf("跨天后应重新允许：%v", err)
	}
}

// 闸三：同 IP 每小时最多 20 条——挡住换号码刷的。
func TestIssueRateLimitPerIP(t *testing.T) {
	s, _, clk := newSMS(t)
	ctx := context.Background()
	for i := 0; i < 20; i++ {
		h := fmt.Sprintf("H%02d", i)
		if err := s.Issue(ctx, "1380000"+h, h, "9.9.9.9"); err != nil {
			t.Fatalf("第 %d 条就被拒了：%v", i+1, err)
		}
		*clk = clk.Add(time.Second)
	}
	if err := s.Issue(ctx, "13900000000", "HX", "9.9.9.9"); err == nil {
		t.Error("同 IP 第 21 条应被拒")
	}
	// 别的 IP 不受影响
	if err := s.Issue(ctx, "13900000000", "HX", "8.8.8.8"); err != nil {
		t.Errorf("换个 IP 被误伤：%v", err)
	}
}

// 被限流时绝不能真发短信——那是在烧钱。
func TestRateLimitedDoesNotSend(t *testing.T) {
	s, f, _ := newSMS(t)
	ctx := context.Background()
	s.Issue(ctx, "13800008888", "H", "1.2.3.4")
	n := len(f.sent)
	s.Issue(ctx, "13800008888", "H", "1.2.3.4")
	if len(f.sent) != n {
		t.Error("被限流了却还是发了短信")
	}
}

// 校验成功后必须保留限流计数器。删掉整行的话，用户登录一次就能重置
// 每日 10 条的上限，反复登录反复刷，闸二形同虚设。
func TestVerifyKeepsRateLimitCounters(t *testing.T) {
	s, f, clk := newSMS(t)
	ctx := context.Background()

	// 发一条、验一条，重复到用满当日 10 条
	for i := 0; i < 10; i++ {
		if err := s.Issue(ctx, "13800008888", "H", "1.2.3.4"); err != nil {
			t.Fatalf("第 %d 条被拒：%v", i+1, err)
		}
		if err := s.Verify("H", f.sent[len(f.sent)-1]); err != nil {
			t.Fatalf("第 %d 次校验失败：%v", i+1, err)
		}
		*clk = clk.Add(61 * time.Second)
	}
	if err := s.Issue(ctx, "13800008888", "H", "1.2.3.4"); err == nil {
		t.Error("登录成功清掉了当日计数——第 11 条本该被拒")
	}
}

// 作废后同一验证码不能再用（expires 归零挡住它）。
func TestConsumedCodeCannotBeReused(t *testing.T) {
	s, f, _ := newSMS(t)
	s.Issue(context.Background(), "13800008888", "H", "1.2.3.4")
	code := f.sent[0]
	if err := s.Verify("H", code); err != nil {
		t.Fatal(err)
	}
	if err := s.Verify("H", code); err == nil {
		t.Error("作废后仍能再用一次")
	}
	if err := s.Verify("H", ""); err == nil {
		t.Error("空验证码匹配上了被清空的 code_hash")
	}
}

// 固定手机号：不发短信、不受限流，验证码恒定。
func TestDevLoginBypass(t *testing.T) {
	s, f, _ := newSMS(t)
	s.SetDevLogin("DEVH", "424242")
	ctx := context.Background()

	// 连续发三次都不该被 60 秒闸挡住——测试要能连续登录
	for i := 0; i < 3; i++ {
		if err := s.Issue(ctx, "13800000000", "DEVH", "1.2.3.4"); err != nil {
			t.Fatalf("第 %d 次被拒：%v", i+1, err)
		}
	}
	if len(f.sent) != 0 {
		t.Errorf("固定号码发了 %d 条真短信", len(f.sent))
	}
	if err := s.Verify("DEVH", "424242"); err != nil {
		t.Errorf("固定验证码校验失败：%v", err)
	}
}

// 旁路只对那一个号码生效，别的号码照常限流、照常发短信。
func TestDevLoginDoesNotLeakToOthers(t *testing.T) {
	s, f, _ := newSMS(t)
	s.SetDevLogin("DEVH", "424242")
	ctx := context.Background()

	if err := s.Issue(ctx, "13900009999", "OTHER", "1.2.3.4"); err != nil {
		t.Fatal(err)
	}
	if len(f.sent) != 1 {
		t.Fatalf("普通号码没走真发送路径，sent=%v", f.sent)
	}
	if err := s.Verify("OTHER", "424242"); err == nil {
		t.Error("固定验证码在别的号码上也能用——旁路漏了")
	}
	if err := s.Issue(ctx, "13900009999", "OTHER", "1.2.3.4"); err == nil {
		t.Error("普通号码不受 60 秒限流了")
	}
}
