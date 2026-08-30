package main

import (
	"fmt"
	"os"
	"path/filepath"
	"strings"
	"time"

	"github.com/dayuer/esp-airprint/server/go/internal/auth"
	"github.com/dayuer/esp-airprint/server/go/internal/config"
	"github.com/dayuer/esp-airprint/server/go/internal/store"
)

const cliUsage = `用法：
  airprintd device add <dev> [name]     手工签发 device 密钥（调试用）
  airprintd device list                 列出全部密钥
  airprintd device revoke <key_id>      吊销一把密钥
  airprintd device unbind <dev>         强制解绑（原持有人不配合时用）
  airprintd user list                   列出用户（只显示尾号）
  airprintd user phone <user_id>        解出完整号码，记审计日志

正常用户走 App 的 enroll，不需要这些命令。`

// runCLI 是运维和排障用的。
func runCLI(cfg *config.Config, args []string) error {
	st, err := store.Open(cfg.DBPath())
	if err != nil {
		return err
	}
	defer st.Close()
	v := auth.NewVerifier(st)

	cmd := strings.Join(args[:min(2, len(args))], " ")
	switch cmd {
	case "device add":
		dev := arg(args, 2)
		if dev == "" {
			return fmt.Errorf("%s", cliUsage)
		}
		token, err := auth.IssueDeviceKey(st, "", dev, arg(args, 3))
		if err != nil {
			return err
		}
		fmt.Printf("设备密钥（只显示这一次）：\n  %s\n", token)
		return nil

	case "device list":
		keys, err := st.ListKeys()
		if err != nil {
			return err
		}
		for _, k := range keys {
			state := "启用"
			if k.Disabled {
				state = "已吊销"
			}
			fmt.Printf("%-12s %-7s dev=%-14s user=%-32s %-6s %s\n",
				k.KeyID, k.Role, k.Dev, k.UserID, state, k.Name)
		}
		return nil

	case "device revoke":
		if arg(args, 2) == "" {
			return fmt.Errorf("%s", cliUsage)
		}
		return auth.RevokeSession(st, v, arg(args, 2))

	// 抢绑防护的逃生门：原持有人不配合时用。
	// 绕过所有权检查，所以只能在服务器上执行，不暴露为 API。
	case "device unbind":
		if arg(args, 2) == "" {
			return fmt.Errorf("%s", cliUsage)
		}
		return auth.RevokeDeviceKeys(st, v, arg(args, 2))

	case "user list":
		users, err := st.ListUsers()
		if err != nil {
			return err
		}
		for _, u := range users {
			// 只打尾号——列表场景不需要完整号码
			fmt.Printf("%-32s ***%s  创建=%s  最近登录=%s\n", u.ID, u.PhoneTail,
				ts(u.Created), ts(u.LastLogin))
		}
		return nil

	// 唯一能解出完整号码的入口。每次调用记审计日志——得知道谁什么时候看过。
	case "user phone":
		userID := arg(args, 2)
		if userID == "" {
			return fmt.Errorf("%s", cliUsage)
		}
		pb, err := auth.NewPhoneBox(cfg.PhonePepper, cfg.PhoneKey)
		if err != nil {
			return err
		}
		enc, ok, err := st.GetPhone(userID)
		if err != nil {
			return err
		}
		if !ok {
			return fmt.Errorf("该用户没有号码记录")
		}
		phone, err := pb.Open(enc)
		if err != nil {
			return fmt.Errorf("解密失败（phone_key 换过？）：%w", err)
		}
		audit(cfg, "解密查看完整号码 user=%s", userID)
		fmt.Println(phone)
		return nil
	}
	return fmt.Errorf("未知命令 %q\n\n%s", strings.Join(args, " "), cliUsage)
}

func arg(a []string, i int) string {
	if i < len(a) {
		return a[i]
	}
	return ""
}

func ts(v int64) string {
	if v == 0 {
		return "-"
	}
	return time.Unix(v, 0).Format("2006-01-02 15:04")
}

// audit 记录敏感操作。写失败只打日志不中断——审计失败不该挡住运维干活，
// 但要留下痕迹。
func audit(cfg *config.Config, format string, a ...any) {
	path := filepath.Join(cfg.Root, "audit.log")
	f, err := os.OpenFile(path, os.O_APPEND|os.O_CREATE|os.O_WRONLY, 0o600)
	if err != nil {
		fmt.Fprintf(os.Stderr, "警告：审计日志写不进 %s：%v\n", path, err)
		return
	}
	defer f.Close()
	fmt.Fprintf(f, "%s %s\n", time.Now().Format(time.RFC3339),
		fmt.Sprintf(format, a...))
}
