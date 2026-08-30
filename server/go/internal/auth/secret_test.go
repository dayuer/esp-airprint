package auth

import (
	"strings"
	"testing"
)

func TestNewTokenFormat(t *testing.T) {
	keyID, secret, token := NewToken()
	if len(keyID) != 12 {
		t.Errorf("keyID 长度 %d，期望 12", len(keyID))
	}
	if len(secret) < 32 {
		t.Errorf("secret 长度 %d，太短", len(secret))
	}
	if token != keyID+"."+secret {
		t.Errorf("token = %q，期望 keyID.secret", token)
	}
	if _, _, tok2 := NewToken(); tok2 == token {
		t.Error("两次生成的令牌相同——随机源有问题")
	}
}

func TestSplitToken(t *testing.T) {
	id, sec, err := SplitToken("abc123.deadbeef")
	if err != nil || id != "abc123" || sec != "deadbeef" {
		t.Errorf("id=%q sec=%q err=%v", id, sec, err)
	}
	for _, bad := range []string{"", "nodot", ".onlysecret", "onlyid.", "a.b.c"} {
		if _, _, err := SplitToken(bad); err == nil {
			t.Errorf("SplitToken(%q) 应当报错", bad)
		}
	}
}

func TestHashVerifyRoundTrip(t *testing.T) {
	h, err := HashSecret("s3cret-value")
	if err != nil {
		t.Fatal(err)
	}
	if !strings.HasPrefix(h, "$argon2id$") {
		t.Errorf("哈希格式 = %q，期望 argon2id 编码串", h)
	}
	if !VerifySecret("s3cret-value", h) {
		t.Error("正确密钥校验失败")
	}
	if VerifySecret("wrong", h) {
		t.Error("错误密钥竟然通过")
	}
	if VerifySecret("s3cret-value", "$argon2id$garbage") {
		t.Error("畸形哈希串竟然通过")
	}
}

func TestHashSaltsAreDistinct(t *testing.T) {
	a, _ := HashSecret("same")
	b, _ := HashSecret("same")
	if a == b {
		t.Error("同一密钥两次哈希结果相同——盐没随机")
	}
}
