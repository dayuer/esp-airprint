// Package sms 是短信服务商的具体实现。
//
// 单测和集成测试绝不真发短信——那既花钱又会骚扰真实号码。
// 测试一律用 auth.Sender 的假实现。
package sms

import (
	"context"
	"errors"
	"log/slog"
)

// Logger 是开发环境用的实现：只打日志，不发短信。
type Logger struct{}

func (Logger) Send(ctx context.Context, phone, code string) error {
	slog.Warn("【开发模式】短信未真实发送", "phone", phone, "code", code)
	return nil
}

type AliyunConfig struct {
	AccessKeyID     string
	AccessKeySecret string
	SignName        string
	TemplateCode    string
}

// Aliyun 是生产实现的骨架。
//
// 刻意让它在未接入 SDK 时启动就失败，而不是安静地不发短信——
// 后者会让所有人登不进来，还查不出原因。
type Aliyun struct{ cfg AliyunConfig }

var ErrNotImplemented = errors.New("sms: 阿里云 SDK 尚未接入；开发环境请留空 sms.access_key_id 走 Logger")

func NewAliyun(c AliyunConfig) (*Aliyun, error) {
	if c.AccessKeyID == "" || c.AccessKeySecret == "" ||
		c.SignName == "" || c.TemplateCode == "" {
		return nil, errors.New("sms: aliyun 配置不完整")
	}
	return nil, ErrNotImplemented
}

func (a *Aliyun) Send(ctx context.Context, phone, code string) error {
	return ErrNotImplemented
}
