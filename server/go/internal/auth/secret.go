// Package auth 管密钥的生成、校验、会话与 ACL 判定。
package auth

import (
	"crypto/rand"
	"crypto/subtle"
	"encoding/base64"
	"encoding/hex"
	"errors"
	"fmt"
	"strings"

	"golang.org/x/crypto/argon2"
)

// argon2id 参数。刻意选得慢——这是防爆破的全部意义。
// 热路径不受影响：校验结果有缓存，且并发数有信号量限制，见 verifier.go。
const (
	argonTime    = 1
	argonMemory  = 64 * 1024 // 64 MB
	argonThreads = 4
	argonKeyLen  = 32
	saltLen      = 16
)

var ErrBadToken = errors.New("auth: 令牌格式非法")

// NewToken 生成一把新密钥，返回 (keyID, secret, 完整令牌)。
// 明文只在签发时出现一次，不落盘。
//
// 令牌格式 {key_id}.{secret}：校验时按 key_id 直接定位到唯一一行，
// 不遍历该设备的所有密钥逐个 argon2——那会让开销随密钥数线性增长。
func NewToken() (keyID, secret, token string) {
	idb := make([]byte, 6)
	sb := make([]byte, 24)
	if _, err := rand.Read(idb); err != nil {
		panic(err) // crypto/rand 失败时继续运行没有意义
	}
	if _, err := rand.Read(sb); err != nil {
		panic(err)
	}
	keyID = hex.EncodeToString(idb)                   // 12 字符
	secret = base64.RawURLEncoding.EncodeToString(sb) // 32 字符
	return keyID, secret, keyID + "." + secret
}

func SplitToken(token string) (keyID, secret string, err error) {
	i := strings.IndexByte(token, '.')
	if i <= 0 || i == len(token)-1 {
		return "", "", ErrBadToken
	}
	keyID, secret = token[:i], token[i+1:]
	if strings.ContainsRune(secret, '.') {
		return "", "", ErrBadToken
	}
	return keyID, secret, nil
}

func HashSecret(secret string) (string, error) {
	salt := make([]byte, saltLen)
	if _, err := rand.Read(salt); err != nil {
		return "", err
	}
	sum := argon2.IDKey([]byte(secret), salt, argonTime, argonMemory, argonThreads, argonKeyLen)
	return fmt.Sprintf("$argon2id$v=%d$m=%d,t=%d,p=%d$%s$%s",
		argon2.Version, argonMemory, argonTime, argonThreads,
		base64.RawStdEncoding.EncodeToString(salt),
		base64.RawStdEncoding.EncodeToString(sum)), nil
}

func VerifySecret(secret, encoded string) bool {
	parts := strings.Split(encoded, "$")
	// ["", "argon2id", "v=19", "m=..,t=..,p=..", salt, hash]
	if len(parts) != 6 || parts[1] != "argon2id" {
		return false
	}
	var version int
	if _, err := fmt.Sscanf(parts[2], "v=%d", &version); err != nil || version != argon2.Version {
		return false
	}
	var m uint32
	var tt, p int
	if _, err := fmt.Sscanf(parts[3], "m=%d,t=%d,p=%d", &m, &tt, &p); err != nil {
		return false
	}
	salt, err := base64.RawStdEncoding.DecodeString(parts[4])
	if err != nil {
		return false
	}
	want, err := base64.RawStdEncoding.DecodeString(parts[5])
	if err != nil {
		return false
	}
	got := argon2.IDKey([]byte(secret), salt, uint32(tt), m, uint8(p), uint32(len(want)))
	return subtle.ConstantTimeCompare(got, want) == 1
}
