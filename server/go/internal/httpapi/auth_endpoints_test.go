package httpapi

import (
	"encoding/json"
	"testing"
	"time"
)

func TestSMSThenVerifyCreatesUser(t *testing.T) {
	h, dep := newTestAPI(t)

	if rr := post(h, "/api/auth/sms", `{"phone":"13800008888"}`, nil); rr.Code != 200 {
		t.Fatalf("发送验证码 = %d %s", rr.Code, rr.Body)
	}
	rr := post(h, "/api/auth/verify",
		`{"phone":"13800008888","code":"`+dep.sender.last+`","device":"iPhone 15"}`, nil)
	if rr.Code != 200 {
		t.Fatalf("校验 = %d %s", rr.Code, rr.Body)
	}
	var out struct {
		Token   string `json:"token"`
		UserID  string `json:"user_id"`
		Tail    string `json:"phone_tail"`
		NewUser bool   `json:"new_user"`
	}
	json.Unmarshal(rr.Body.Bytes(), &out)
	if out.Token == "" || out.UserID == "" || !out.NewUser {
		t.Fatalf("响应缺字段：%s", rr.Body)
	}
	if out.Tail != "8888" {
		t.Errorf("phone_tail = %q", out.Tail)
	}

	// 完整号码必须能解回来——客服核验、换号迁移要用
	enc, ok, _ := dep.store.GetPhone(out.UserID)
	if !ok {
		t.Fatal("完整号码没落库")
	}
	phone, err := dep.phone.Open(enc)
	if err != nil || phone != "13800008888" {
		t.Errorf("解出 %q err=%v", phone, err)
	}
}

// 同一号码第二次登录不该再建用户。
func TestVerifyTwiceSameUser(t *testing.T) {
	h, dep := newTestAPI(t)
	first := login(t, h, dep, "13800008888")
	dep.clock = dep.clock.Add(2 * time.Minute)
	second := login(t, h, dep, "13800008888")
	if first == second {
		t.Error("两次登录拿到同一把 token——应当各签一把")
	}
	idA, _ := dep.v.Verify(first)
	idB, _ := dep.v.Verify(second)
	if idA.UserID != idB.UserID {
		t.Errorf("同一号码拿到两个 user：%s vs %s", idA.UserID, idB.UserID)
	}
}

func TestVerifyRejectsWrongCode(t *testing.T) {
	h, _ := newTestAPI(t)
	post(h, "/api/auth/sms", `{"phone":"13800008888"}`, nil)
	rr := post(h, "/api/auth/verify", `{"phone":"13800008888","code":"000000"}`, nil)
	if rr.Code != 401 {
		t.Errorf("错误验证码 = %d，期望 401", rr.Code)
	}
}

func TestSMSRejectsBadPhone(t *testing.T) {
	h, _ := newTestAPI(t)
	if rr := post(h, "/api/auth/sms", `{"phone":"12345"}`, nil); rr.Code != 400 {
		t.Errorf("非法号码 = %d，期望 400", rr.Code)
	}
}

// 60 秒内重发要被挡住，且不能真发第二条短信。
func TestSMSRateLimited(t *testing.T) {
	h, dep := newTestAPI(t)
	post(h, "/api/auth/sms", `{"phone":"13800008888"}`, nil)
	first := dep.sender.last
	rr := post(h, "/api/auth/sms", `{"phone":"13800008888"}`, nil)
	if rr.Code != 429 {
		t.Errorf("60 秒内重发 = %d，期望 429", rr.Code)
	}
	if dep.sender.last != first {
		t.Error("被限流了却还是发了短信——在烧钱")
	}
}

func TestRequireAuthRejectsMissingAndBadToken(t *testing.T) {
	h, _ := newTestAPI(t)
	for _, hdr := range []map[string]string{
		nil,
		{"Authorization": "Bearer garbage"},
		{"Authorization": "not-bearer xxx"},
		{"Authorization": "Bearer "},
	} {
		if rr := get(h, "/api/status", hdr); rr.Code != 401 {
			t.Errorf("头 %v → %d，期望 401", hdr, rr.Code)
		}
	}
}

// 401 不能区分「不存在」和「密钥错」——不给探测者提供信息。
func TestUnauthorizedBodyIsUniform(t *testing.T) {
	h, _ := newTestAPI(t)
	a := get(h, "/api/status", bearer("aaaaaaaaaaaa.xxx"))
	b := get(h, "/api/status", bearer("bbbbbbbbbbbb.yyy"))
	if a.Body.String() != b.Body.String() {
		t.Errorf("401 响应体不一致，泄露了信息：%q vs %q", a.Body, b.Body)
	}
}

// 角色隔离：app 令牌不能走设备端点，反之亦然。
func TestRoleIsolation(t *testing.T) {
	h, dep := newTestAPI(t)
	appTok := login(t, h, dep, "13800008888")
	devKey := enroll(t, h, appTok, "f412fa87c9e0")

	if rr := get(h, "/api/job/xxx/data", bearer(appTok)); rr.Code != 401 {
		t.Errorf("app 令牌走取件端点 = %d，期望 401", rr.Code)
	}
	if rr := post(h, "/api/device/enroll", `{"dev":"aaaaaaaaaaaa"}`, bearer(devKey)); rr.Code != 401 {
		t.Errorf("device 密钥走 enroll = %d，期望 401", rr.Code)
	}
}

func TestLogoutRevokesToken(t *testing.T) {
	h, dep := newTestAPI(t)
	token := login(t, h, dep, "13800008888")

	if rr := post(h, "/api/auth/logout", `{}`, bearer(token)); rr.Code != 200 {
		t.Fatalf("登出 = %d %s", rr.Code, rr.Body)
	}
	if rr := get(h, "/api/status", bearer(token)); rr.Code != 401 {
		t.Errorf("登出后 token 仍可用（%d）", rr.Code)
	}
}

// 注销要把个人数据清干净。
func TestAccountDeleteWipesPersonalData(t *testing.T) {
	h, dep := newTestAPI(t)
	token := login(t, h, dep, "13800008888")
	enroll(t, h, token, "f412fa87c9e0")

	rr := post(h, "/api/account/delete", `{}`, bearer(token))
	if rr.Code != 200 {
		t.Fatalf("注销 = %d %s", rr.Code, rr.Body)
	}
	if _, ok, _ := dep.store.UserByHMAC(dep.phone.HMAC("13800008888")); ok {
		t.Error("users 行还在")
	}
	if rr := get(h, "/api/status", bearer(token)); rr.Code != 401 {
		t.Error("注销后 token 仍可用")
	}
	// 设备解绑后可被重新 enroll
	if owner, ok, _ := dep.store.OwnerOfDevice("f412fa87c9e0"); ok {
		t.Errorf("注销后设备仍属于 %q", owner)
	}
}
