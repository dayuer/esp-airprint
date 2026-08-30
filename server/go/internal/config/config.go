// Package config 读取 config.json。
//
// 设备口令不再出现在这里——每设备一密钥，存在 sqlite 里，见 internal/store。
package config

import (
	"encoding/json"
	"errors"
	"os"
	"path/filepath"
)

type SMSConfig struct {
	Provider        string `json:"provider"`
	AccessKeyID     string `json:"access_key_id"`
	AccessKeySecret string `json:"access_key_secret"`
	SignName        string `json:"sign_name"`
	TemplateCode    string `json:"template_code"`
}

type Config struct {
	Root     string `json:"root"`
	CertDir  string `json:"cert_dir"`
	HTTPAddr string `json:"http_addr"`
	MQTTAddr string `json:"mqtt_addr"`

	// URF 是光栅，单份 200KB~15MB，不清理很快写满盘。
	JobRetainHours     int `json:"job_retain_hours"`
	JobRetainPerDevice int `json:"job_retain_per_device"`

	// 两把密钥的运维含义不同：
	//   PhonePepper 换了全体用户无法登录（HMAC 对不上），绝不轮换
	//   PhoneKey    换了旧密文解不开，但登录不受影响
	PhonePepper string `json:"phone_pepper"`
	PhoneKey    string `json:"phone_key"`

	SMS SMSConfig `json:"sms"`
}

func Load(path string) (*Config, error) {
	raw, err := os.ReadFile(path)
	if err != nil {
		return nil, err
	}
	var c Config
	if err := json.Unmarshal(raw, &c); err != nil {
		return nil, err
	}
	if c.CertDir == "" {
		return nil, errors.New("config: cert_dir 必填")
	}
	if c.PhonePepper == "" || c.PhoneKey == "" {
		return nil, errors.New("config: phone_pepper 和 phone_key 必填")
	}
	if c.Root == "" {
		c.Root = "/opt/airprint"
	}
	if c.HTTPAddr == "" {
		c.HTTPAddr = ":9443"
	}
	if c.MQTTAddr == "" {
		c.MQTTAddr = ":8883"
	}
	if c.JobRetainHours < 1 {
		c.JobRetainHours = 24
	}
	if c.JobRetainPerDevice < 1 {
		c.JobRetainPerDevice = 50
	}
	return &c, nil
}

func (c *Config) JobsDir() string   { return filepath.Join(c.Root, "jobs") }
func (c *Config) IdentsDir() string { return filepath.Join(c.Root, "idents") }
func (c *Config) DBPath() string    { return filepath.Join(c.Root, "jobs.db") }
func (c *Config) CertPath() string  { return filepath.Join(c.CertDir, "fullchain.pem") }
func (c *Config) KeyPath() string   { return filepath.Join(c.CertDir, "privkey.pem") }
