// Package profile 生成、校验并分层查找 USB 层怪癖档案。
//
// profile 不是参数表，是一份可编排的动作序列：固件提供四个原语，
// 服务端用文本编排它们。那 9 个字节的 UEL 不再是固件里的常量，
// 而是下面 job_end 里的一串十六进制——新机型的新怪癖改服务端一行文本即可。
package profile

import (
	"encoding/hex"
	"encoding/json"
	"fmt"
)

// 硬上限。设备可用堆只有 70~120KB，越界一律**整份拒绝**，不截断——
// 半份 profile 比没有 profile 更危险。固件侧也会再验一遍。
const (
	MaxBytes     = 1024
	MaxSteps     = 8
	MaxSendBytes = 64
	MaxDelayMS   = 5000
	MaxDelaySum  = 10000
)

// 原语白名单。固件不认识的：可选步骤跳过并上报，标了 required 的拒绝整份。
const (
	OpSendHex    = "send_hex"
	OpDelayMS    = "delay_ms"
	OpIfaceReset = "iface_reset"
	OpReadStatus = "read_status"
)

// UEL 是 Universal Exit Language，HP/三星系的作业分隔符。
// 不发它只能打第一份，第二份要人跑到打印机前按取消键才动。
// 这是整个项目最关键的 9 个字节，现在它住在这里，不在固件里。
const UEL = "1b252d313233343558"

type Step struct {
	Op       string `json:"op"`
	Data     string `json:"data,omitempty"`
	MS       int    `json:"ms,omitempty"`
	Required bool   `json:"required,omitempty"`
}

type Flags struct {
	Unidir bool `json:"unidir"`
	PJLOK  bool `json:"pjl_ok"`
}

type Hooks struct {
	JobBegin []Step `json:"job_begin,omitempty"`
	JobEnd   []Step `json:"job_end,omitempty"`
	Wake     []Step `json:"wake,omitempty"`
}

type Profile struct {
	Rev    int    `json:"rev"`
	Match  string `json:"match,omitempty"`
	Serial string `json:"serial,omitempty"`
	Src    string `json:"src"`
	Flags  Flags  `json:"flags"`
	Hooks  Hooks  `json:"hooks"`
}

// 档案来源层级，也是 GET /api/device/{dev}/printer 里 src 字段的取值。
const (
	SrcSerial        = "serial"        // 这台机器自己测过
	SrcModel         = "model"         // 同型号 ≥3 台一致
	SrcAuthoritative = "authoritative" // 开发者标记
	SrcQuirks        = "quirks"        // CUPS usb-quirks 表
	SrcDefault       = "default"       // 保守默认值
)

// Default 是没有任何实测数据时的保守档案。
//
// 保守的方向是「宁可多发 9 个字节，也不要打不出来」：发 UEL、不做接口复位、
// 保持双向。判不准时永远取更安全的那个值。
func Default(serial string) Profile {
	return Profile{
		Rev: 1, Serial: serial, Src: SrcDefault,
		Flags: Flags{Unidir: false, PJLOK: true},
		Hooks: Hooks{
			JobEnd: []Step{{Op: OpSendHex, Data: UEL, Required: true}},
		},
	}
}

// Validate 在下发前把关。不能让畸形 JSON 传到设备上才发现。
func Validate(p Profile) error {
	hooks := map[string][]Step{
		"job_begin": p.Hooks.JobBegin,
		"job_end":   p.Hooks.JobEnd,
		"wake":      p.Hooks.Wake,
	}
	for name, steps := range hooks {
		if len(steps) > MaxSteps {
			return fmt.Errorf("profile: %s 有 %d 步，上限 %d", name, len(steps), MaxSteps)
		}
		delaySum := 0
		for i, s := range steps {
			switch s.Op {
			case OpSendHex:
				b, err := hex.DecodeString(s.Data)
				if err != nil {
					return fmt.Errorf("profile: %s[%d] 的 data 不是合法十六进制", name, i)
				}
				if len(b) == 0 || len(b) > MaxSendBytes {
					return fmt.Errorf("profile: %s[%d] 要发 %d 字节，须在 1~%d 之间",
						name, i, len(b), MaxSendBytes)
				}
			case OpDelayMS:
				if s.MS <= 0 || s.MS > MaxDelayMS {
					return fmt.Errorf("profile: %s[%d] delay_ms=%d，须在 1~%d 之间",
						name, i, s.MS, MaxDelayMS)
				}
				delaySum += s.MS
			case OpIfaceReset:
				// 端点复位只在下一份作业开始时做。曾经在 job_end 里做过，
				// 结果稳定在距流尾几 KB 处 Decoding Fail——最后一个短包
				// 还没物理冲出去就被 halt/flush 掉了。
				if name == "job_end" {
					return fmt.Errorf("profile: job_end 里不能做 iface_reset（会截断流尾）")
				}
			case OpReadStatus:
				// 无参数
			default:
				return fmt.Errorf("profile: %s[%d] 未知原语 %q", name, i, s.Op)
			}
		}
		if delaySum > MaxDelaySum {
			return fmt.Errorf("profile: %s 的 delay 合计 %dms，上限 %d", name, delaySum, MaxDelaySum)
		}
	}
	b, err := Marshal(p)
	if err != nil {
		return err
	}
	if len(b) > MaxBytes {
		return fmt.Errorf("profile: 编码后 %d 字节，上限 %d", len(b), MaxBytes)
	}
	return nil
}

func Marshal(p Profile) ([]byte, error) { return json.Marshal(p) }

func Unmarshal(b []byte) (Profile, error) {
	var p Profile
	err := json.Unmarshal(b, &p)
	return p, err
}
