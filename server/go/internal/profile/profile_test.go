package profile

import (
	"strings"
	"testing"
)

// 默认档案必须发 UEL——不发只能打第一份，第二份要人去按取消键。
func TestDefaultSendsUEL(t *testing.T) {
	p := Default("PA")
	if len(p.Hooks.JobEnd) != 1 {
		t.Fatalf("job_end = %+v", p.Hooks.JobEnd)
	}
	s := p.Hooks.JobEnd[0]
	if s.Op != OpSendHex || s.Data != UEL {
		t.Errorf("默认档案没发 UEL：%+v", s)
	}
	if !s.Required {
		t.Error("UEL 必须标 required——固件不认识就该整份拒绝，不能静默跳过")
	}
	if err := Validate(p); err != nil {
		t.Errorf("默认档案自己都过不了校验：%v", err)
	}
}

func TestValidateRejectsUnknownOp(t *testing.T) {
	p := Default("PA")
	p.Hooks.Wake = []Step{{Op: "reboot_the_printer"}}
	if err := Validate(p); err == nil {
		t.Error("未知原语应被拒")
	}
}

// job_end 里做接口复位会在距流尾几 KB 处 Decoding Fail——这是实测结论。
func TestValidateRejectsIfaceResetInJobEnd(t *testing.T) {
	p := Default("PA")
	p.Hooks.JobEnd = append(p.Hooks.JobEnd, Step{Op: OpIfaceReset})
	err := Validate(p)
	if err == nil {
		t.Fatal("job_end 里的 iface_reset 应被拒")
	}
	if !strings.Contains(err.Error(), "流尾") {
		t.Errorf("错误信息该说清原因，得到：%v", err)
	}
	// 放在 job_begin 就没问题
	p2 := Default("PA")
	p2.Hooks.JobBegin = []Step{{Op: OpIfaceReset}}
	if err := Validate(p2); err != nil {
		t.Errorf("job_begin 里的 iface_reset 被误拒：%v", err)
	}
}

func TestValidateEnforcesLimits(t *testing.T) {
	cases := map[string]func(*Profile){
		"步数超限": func(p *Profile) {
			for i := 0; i <= MaxSteps; i++ {
				p.Hooks.Wake = append(p.Hooks.Wake, Step{Op: OpReadStatus})
			}
		},
		"send_hex 过长": func(p *Profile) {
			p.Hooks.Wake = []Step{{Op: OpSendHex, Data: strings.Repeat("41", MaxSendBytes+1)}}
		},
		"send_hex 为空": func(p *Profile) {
			p.Hooks.Wake = []Step{{Op: OpSendHex, Data: ""}}
		},
		"send_hex 非十六进制": func(p *Profile) {
			p.Hooks.Wake = []Step{{Op: OpSendHex, Data: "zzzz"}}
		},
		"delay 超单次上限": func(p *Profile) {
			p.Hooks.Wake = []Step{{Op: OpDelayMS, MS: MaxDelayMS + 1}}
		},
		"delay 为 0": func(p *Profile) {
			p.Hooks.Wake = []Step{{Op: OpDelayMS, MS: 0}}
		},
		"delay 合计超限": func(p *Profile) {
			for i := 0; i < 3; i++ {
				p.Hooks.Wake = append(p.Hooks.Wake, Step{Op: OpDelayMS, MS: MaxDelayMS})
			}
		},
	}
	for name, mutate := range cases {
		p := Default("PA")
		mutate(&p)
		if err := Validate(p); err == nil {
			t.Errorf("%s：应被拒", name)
		}
	}
}

func TestValidateEnforcesTotalSize(t *testing.T) {
	p := Default("PA")
	// 8 步 × 64 字节的 hex，加上外层结构，必然超 1024
	for i := 0; i < MaxSteps; i++ {
		p.Hooks.Wake = append(p.Hooks.Wake,
			Step{Op: OpSendHex, Data: strings.Repeat("41", MaxSendBytes)})
	}
	if err := Validate(p); err == nil {
		t.Error("超过总长上限应被拒")
	}
}

func TestRoundTrip(t *testing.T) {
	p := Default("CNB9K1P2X4")
	p.Hooks.Wake = []Step{
		{Op: OpSendHex, Data: UEL},
		{Op: OpDelayMS, MS: 300},
	}
	b, err := Marshal(p)
	if err != nil {
		t.Fatal(err)
	}
	got, err := Unmarshal(b)
	if err != nil {
		t.Fatal(err)
	}
	if len(got.Hooks.Wake) != 2 || got.Hooks.Wake[1].MS != 300 {
		t.Errorf("往返后 = %+v", got.Hooks)
	}
	if got.Serial != "CNB9K1P2X4" {
		t.Errorf("serial 丢了：%q", got.Serial)
	}
}
