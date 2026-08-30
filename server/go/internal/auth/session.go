package auth

import "github.com/dayuer/esp-airprint/server/go/internal/store"

// SessionStore 是签发与吊销需要的 store 能力。
type SessionStore interface {
	InsertKey(k store.Key) error
	DisableKey(keyID string) error
	DisableKeysOfUser(userID, role string) ([]string, error)
	DisableKeysOfDevice(dev, role string) ([]string, error)
}

// IssueSession 为一台手机签发一把 app 令牌。
//
// 刻意不设过期：打印是低频操作，强制重新登录只会激怒用户。
// 要下线就靠吊销——一台手机一把，丢了单独踢。
func IssueSession(st SessionStore, userID, deviceName string) (string, error) {
	return issueKey(st, store.Key{
		Role: string(RoleApp), UserID: userID, Name: deviceName,
	})
}

// IssueDeviceKey 为一台桥签发 device 密钥，绑到该用户。
// enroll 走这里，运维的 device add 也走这里。
func IssueDeviceKey(st SessionStore, userID, dev, name string) (string, error) {
	return issueKey(st, store.Key{
		Dev: dev, Role: string(RoleDevice), UserID: userID, Name: name,
	})
}

func issueKey(st SessionStore, k store.Key) (string, error) {
	keyID, secret, token := NewToken()
	hash, err := HashSecret(secret)
	if err != nil {
		return "", err
	}
	k.KeyID, k.Hash = keyID, hash
	if err := st.InsertKey(k); err != nil {
		return "", err
	}
	return token, nil
}

// RevokeSession 吊销一把。必须同时清校验缓存，否则它还能再活一小时。
func RevokeSession(st SessionStore, v *Verifier, keyID string) error {
	if err := st.DisableKey(keyID); err != nil {
		return err
	}
	v.Invalidate(keyID)
	return nil
}

func RevokeAllSessions(st SessionStore, v *Verifier, userID string) error {
	ids, err := st.DisableKeysOfUser(userID, string(RoleApp))
	if err != nil {
		return err
	}
	for _, id := range ids {
		v.Invalidate(id)
	}
	return nil
}

// RevokeDeviceKeys 吊销某台桥的全部 device 密钥。重置设备与解绑都走这里。
func RevokeDeviceKeys(st SessionStore, v *Verifier, dev string) error {
	ids, err := st.DisableKeysOfDevice(dev, string(RoleDevice))
	if err != nil {
		return err
	}
	for _, id := range ids {
		v.Invalidate(id)
	}
	return nil
}
