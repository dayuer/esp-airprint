package auth

import "testing"

const testKeyHex = "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f"

func newPhoneBox(t *testing.T) *PhoneBox {
	t.Helper()
	pb, err := NewPhoneBox("pepper-value", testKeyHex)
	if err != nil {
		t.Fatal(err)
	}
	return pb
}

func TestHMACIsStableAndPeppered(t *testing.T) {
	pb := newPhoneBox(t)
	a := pb.HMAC("13800008888")
	if a != pb.HMAC("13800008888") {
		t.Error("同一号码两次 HMAC 不一致")
	}
	if a == pb.HMAC("13800008889") {
		t.Error("不同号码 HMAC 相同")
	}
	other, _ := NewPhoneBox("different-pepper", testKeyHex)
	if a == other.HMAC("13800008888") {
		t.Error("换了 pepper 结果不变——pepper 没参与运算")
	}
}

func TestTail(t *testing.T) {
	pb := newPhoneBox(t)
	if got := pb.Tail("13800008888"); got != "8888" {
		t.Errorf("Tail = %q，期望 8888", got)
	}
	if got := pb.Tail("123"); got != "123" {
		t.Errorf("短号码不该 panic，得到 %q", got)
	}
}

func TestSealOpenRoundTrip(t *testing.T) {
	pb := newPhoneBox(t)
	enc, err := pb.Seal("13800008888")
	if err != nil {
		t.Fatal(err)
	}
	if string(enc) == "13800008888" {
		t.Fatal("密文等于明文——没有加密")
	}
	got, err := pb.Open(enc)
	if err != nil || got != "13800008888" {
		t.Fatalf("解出 %q err=%v", got, err)
	}
}

func TestSealUsesRandomNonce(t *testing.T) {
	pb := newPhoneBox(t)
	a, _ := pb.Seal("13800008888")
	b, _ := pb.Seal("13800008888")
	if string(a) == string(b) {
		t.Error("同一号码两次加密结果相同——nonce 没随机")
	}
}

func TestOpenRejectsTamperedCiphertext(t *testing.T) {
	pb := newPhoneBox(t)
	enc, _ := pb.Seal("13800008888")
	enc[len(enc)-1] ^= 0xff
	if _, err := pb.Open(enc); err == nil {
		t.Error("被篡改的密文竟然解开了——GCM 认证没生效")
	}
}

func TestNewPhoneBoxRejectsBadKey(t *testing.T) {
	for _, c := range []struct{ pepper, key string }{
		{"", testKeyHex},
		{"p", "tooshort"},
		{"p", "zz" + testKeyHex[2:]},
	} {
		if _, err := NewPhoneBox(c.pepper, c.key); err == nil {
			t.Errorf("NewPhoneBox(%q, %q) 应当报错", c.pepper, c.key)
		}
	}
}

func TestNormalizePhone(t *testing.T) {
	for _, in := range []string{"13800008888", "+8613800008888", "8613800008888", "138 0000 8888"} {
		got, err := NormalizePhone(in)
		if err != nil || got != "13800008888" {
			t.Errorf("NormalizePhone(%q) = %q err=%v", in, got, err)
		}
	}
	for _, in := range []string{"", "12345", "23800008888", "abcdefghijk", "138000088888888"} {
		if _, err := NormalizePhone(in); err == nil {
			t.Errorf("NormalizePhone(%q) 应当报错", in)
		}
	}
}
