package auth

import (
	"crypto/aes"
	"crypto/cipher"
	"crypto/hmac"
	"crypto/rand"
	"crypto/sha256"
	"encoding/hex"
	"errors"
	"regexp"
	"strings"
)

// PhoneBox 管手机号的三种形态：
//
//	HMAC      —— 登录查询与唯一性，不可逆
//	Tail      —— 界面展示，不可逆
//	Seal/Open —— 完整号码，可逆，存在单独的表里
//
// 两把密钥的运维含义不同：
//
//	pepper 换了全体用户无法登录（HMAC 对不上），绝不轮换
//	key    换了旧密文解不开，但登录不受影响
type PhoneBox struct {
	pepper []byte
	aead   cipher.AEAD
}

func NewPhoneBox(pepper, keyHex string) (*PhoneBox, error) {
	if pepper == "" {
		return nil, errors.New("auth: phone_pepper 必填")
	}
	key, err := hex.DecodeString(keyHex)
	if err != nil || len(key) != 32 {
		return nil, errors.New("auth: phone_key 必须是 32 字节的 hex")
	}
	blk, err := aes.NewCipher(key)
	if err != nil {
		return nil, err
	}
	aead, err := cipher.NewGCM(blk)
	if err != nil {
		return nil, err
	}
	return &PhoneBox{pepper: []byte(pepper), aead: aead}, nil
}

func (p *PhoneBox) HMAC(phone string) string {
	m := hmac.New(sha256.New, p.pepper)
	m.Write([]byte(phone))
	return hex.EncodeToString(m.Sum(nil))
}

func (p *PhoneBox) Tail(phone string) string {
	if len(phone) <= 4 {
		return phone
	}
	return phone[len(phone)-4:]
}

func (p *PhoneBox) Seal(phone string) ([]byte, error) {
	nonce := make([]byte, p.aead.NonceSize())
	if _, err := rand.Read(nonce); err != nil {
		return nil, err
	}
	return p.aead.Seal(nonce, nonce, []byte(phone), nil), nil
}

func (p *PhoneBox) Open(enc []byte) (string, error) {
	n := p.aead.NonceSize()
	if len(enc) < n {
		return "", errors.New("auth: 密文过短")
	}
	out, err := p.aead.Open(nil, enc[:n], enc[n:], nil)
	if err != nil {
		return "", err
	}
	return string(out), nil
}

var reCN = regexp.MustCompile(`^1[3-9]\d{9}$`)

// NormalizePhone 只处理中国大陆号码。国际号码不在范围内（见 spec 第 14 节）。
func NormalizePhone(in string) (string, error) {
	s := strings.NewReplacer(" ", "", "-", "").Replace(in)
	s = strings.TrimPrefix(s, "+")
	s = strings.TrimPrefix(s, "86")
	if !reCN.MatchString(s) {
		return "", errors.New("auth: 手机号格式不合法")
	}
	return s, nil
}
