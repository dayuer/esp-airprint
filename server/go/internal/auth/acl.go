package auth

import "strings"

// Membership 回答「这个 dev 属于这个 user 吗」。
// 实现方负责缓存——ACL 判定在 MQTT 热路径上，不能每次查库。
type Membership interface {
	HasDevice(userID, dev string) bool
}

// ACLAllowed 判定某身份能否读/写某 topic。
//
// topic 一律是 printer/{dev}/{leaf} 三段。不做通配符展开——
// 设备和 App 都不需要跨设备订阅，把口子焊死比事后审计便宜。
func ACLAllowed(id Identity, topic string, write bool, m Membership) bool {
	parts := strings.Split(topic, "/")
	if len(parts) != 3 || parts[0] != "printer" {
		return false
	}
	dev, leaf := parts[1], parts[2]

	switch id.Role {
	case RoleDevice:
		if dev != id.Dev {
			return false
		}
		if write {
			return leaf == "status" || leaf == "ident"
		}
		return leaf == "job" || leaf == "cmd" || leaf == "profile"

	case RoleApp:
		// App 只旁听自己名下设备的状态。
		// 能写 status 就能伪造打印机状态；能读 job 就能截胡作业信令；
		// 能读 profile 也没有意义——那是设备侧的 USB 层配置。
		if write || leaf != "status" {
			return false
		}
		return m != nil && m.HasDevice(id.UserID, dev)
	}
	return false
}
