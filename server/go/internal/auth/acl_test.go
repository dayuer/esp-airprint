package auth

import "testing"

// fakeMembership: u1 名下有 aaa 和 ccc，没有 bbb。
type fakeMembership struct{}

func (fakeMembership) HasDevice(userID, dev string) bool {
	return userID == "u1" && (dev == "aaa" || dev == "ccc")
}

// 这张矩阵是安全边界，改动必须先改这里。
func TestACLMatrix(t *testing.T) {
	dev := Identity{Dev: "aaa", Role: RoleDevice}
	app := Identity{UserID: "u1", Role: RoleApp}

	cases := []struct {
		name  string
		id    Identity
		topic string
		write bool
		want  bool
	}{
		{"设备发状态", dev, "printer/aaa/status", true, true},
		{"设备发档案", dev, "printer/aaa/ident", true, true},
		{"设备收作业", dev, "printer/aaa/job", false, true},
		{"设备收命令", dev, "printer/aaa/cmd", false, true},
		{"设备收怪癖档案", dev, "printer/aaa/profile", false, true},
		{"设备不能自派作业", dev, "printer/aaa/job", true, false},
		{"设备不能碰别人", dev, "printer/bbb/status", true, false},
		{"设备不能订阅别人", dev, "printer/bbb/status", false, false},
		{"设备不能用通配符", dev, "printer/+/status", false, false},

		{"App 订阅名下设备", app, "printer/aaa/status", false, true},
		{"App 订阅名下另一台", app, "printer/ccc/status", false, true},
		{"App 不能订阅名下之外的", app, "printer/bbb/status", false, false},
		{"App 不能伪造状态", app, "printer/aaa/status", true, false},
		{"App 不能派作业", app, "printer/aaa/job", true, false},
		{"App 不能收作业内容", app, "printer/aaa/job", false, false},
		{"App 不能收怪癖档案", app, "printer/aaa/profile", false, false},

		{"越界前缀", dev, "printer/aaa", false, false},
		{"多余层级", dev, "printer/aaa/status/x", false, false},
		{"完全无关的 topic", dev, "$SYS/broker/uptime", false, false},
	}
	for _, c := range cases {
		if got := ACLAllowed(c.id, c.topic, c.write, fakeMembership{}); got != c.want {
			t.Errorf("%s: ACLAllowed(%+v, %q, write=%v) = %v，期望 %v",
				c.name, c.id, c.topic, c.write, got, c.want)
		}
	}
}
