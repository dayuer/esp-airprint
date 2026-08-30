# stickboxd 部署

Go 重写版的部署说明。设计见
`docs/superpowers/specs/2026-08-30-go-print-server-design.md`，
接口契约见 `docs/API-cloud-print.md`。

**部署物只有两样：一个 Go 二进制和一个 sqlite 文件。**
不需要 CUPS、PPD、字体、Python——服务端不渲染，光栅由 App 完成。

---

## 1. 构建

```bash
cd server/go && CGO_ENABLED=0 go build -ldflags "-X github.com/dayuer/stickbox/server/go/internal/version.Version=$(git describe --tags --always)" -o stickboxd ./cmd/stickboxd
```

`CGO_ENABLED=0` 可行是因为 sqlite 用的是纯 Go 的 `modernc.org/sqlite`。
产物是静态二进制，扔到服务器上就能跑。

## 2. 首次部署

```bash
sudo useradd -r -s /usr/sbin/nologin stickbox
sudo mkdir -p /opt/stickbox/{bin,jobs,idents}
sudo chown -R stickbox:stickbox /opt/stickbox
```

生成两把密钥：

```bash
openssl rand -hex 32
```

照 `server/config.example.json` 写 `/opt/stickbox/config.json`，权限 `600`。

## 3. 两把密钥的运维含义（这一节最重要）

| 密钥 | 换掉的后果 |
|---|---|
| `phone_pepper` | **全体用户无法登录**（手机号 HMAC 全部对不上）。**绝不轮换** |
| `phone_key` | 旧的加密号码解不开，但**登录不受影响**。真要轮换得写一次全表重加密 |

两者都在 `config.json` 里，`chmod 600`，不入库。**备份 `config.json` 时它们
一起走，丢了等于用户全体失联。**

## 4. 证书

certbot 照旧。**deploy hook 从「必需」降级为「可选」**——进程按文件 mtime
自己热重载，不再需要靠重启换证书。

### ⚠ 但旧 hook 必须改，不然会在续签时把停掉的服务拉起来

原来的 `/etc/letsencrypt/renewal-hooks/deploy/mosquitto.sh` 最后一行是：

```
systemctl reload mosquitto || systemctl restart mosquitto; systemctl restart airprint-job
```

切到 stickboxd 之后 mosquitto 已经停用，**这行会在下次续签时把它拉起来抢占
8883**。而且是几十天后才发作，现场没人会想到是证书续签干的。

改成只留改权限的两行——stickbox 用户要靠它们读到私钥：

```sh
#!/bin/sh
chgrp -R ssl-cert /etc/letsencrypt/live /etc/letsencrypt/archive 2>/dev/null
chmod -R g+rX /etc/letsencrypt/live /etc/letsencrypt/archive 2>/dev/null
```

### 私钥读不到时先看补充组

`usermod -aG ssl-cert stickbox` 只改了用户的组，**systemd 服务默认不带补充组**。
单元文件里必须有 `SupplementaryGroups=ssl-cert`，否则服务起来读不到 privkey。

验证热重载生效：

```bash
sudo touch /etc/letsencrypt/live/mqtt.silkline.id/fullchain.pem && sudo journalctl -u stickboxd -n 20 | grep 热重载
```

## 5. 备份

**`jobs.db` 现在含个人信息**（加密的手机号）。备份文件必须加密且限权，
不能像以前那样随手 `scp` 到本地。

作业文件（`jobs/*.urf`）不用备份——URF 是光栅，单份 200KB~15MB，且
App 随时可以重新生成。

## 5b. ⚠ 别用 root 试跑二进制

`sqlite: attempt to write a readonly database (8)` 十有八九是这么来的：
先用 root 手动跑了一次 `stickboxd`，它把 `jobs.db` 建成了 root 属主，
之后服务以 `stickbox` 身份启动就写不进去。

排查一眼就够：

```bash
ls -la /opt/stickbox/*.db
```

修法：

```bash
chown -R stickbox:stickbox /opt/stickbox && systemctl restart stickboxd
```

要试跑就带上身份：`sudo -u stickbox /opt/stickbox/bin/stickboxd -conf ...`

## 6. 切换

```bash
sudo systemctl stop stickbox-job.service mosquitto.service
```

```bash
sudo systemctl disable mosquitto.service
```

```bash
sudo cp server/stickboxd.service /etc/systemd/system/ && sudo systemctl daemon-reload && sudo systemctl enable --now stickboxd.service
```

**mosquitto 直接下线，不设并行期。** broker 已内嵌，设备侧要同步改密钥，
并行期只会让两条认证路径同时半生不熟。

## 7. 回滚

反过来即可。数据库双向兼容——Python 版忽略新增的表和列。

**回滚的前提是 CUPS 还在**，所以先别急着卸载 `cups` / `fonts-noto-cjk` /
`python3-gi`，跑稳一个月再清。卸载是不可逆的。

## 8. 上线验证清单

切换后逐条执行。`$DEV` / `$DEVKEY` / `$APPTOK` 按
`docs/API-cloud-print.md` 第 7 节拿。

```bash
ss -lntp | grep -E ':(8883|9443)'
```

```bash
openssl s_client -connect mqtt.silkline.id:9443 -servername mqtt.silkline.id </dev/null 2>/dev/null | openssl x509 -noout -dates
```

```bash
curl -s -o /dev/null -w '%{http_code}\n' https://mqtt.silkline.id:9443/api/status
```

### 三条必须为「失败」的检查

这三条对应本次重写补的三个洞。**它们没有用户可见的症状，坏了也不会有人报障**，
所以必须主动验。

```bash
mosquitto_pub -h mqtt.silkline.id -p 8883 --capath /etc/ssl/certs -u $DEV -P 'bogus.key' -t "printer/$DEV/status" -m '{}'; echo "退出码=$? （非 0 才对）"
```

```bash
mosquitto_pub -h mqtt.silkline.id -p 8883 --capath /etc/ssl/certs -u $DEV -P "$DEVKEY" -t 'printer/aaaaaaaaaaaa/status' -m '{}'; echo "退出码=$? （非 0 才对）"
```

```bash
curl -s -o /dev/null -w '%{http_code}\n' -X POST https://mqtt.silkline.id:9443/api/print -H "Authorization: Bearer $APPTOK" -H "X-Device: $DEV" -H 'X-Printer-Serial: PA' -H 'Content-Type: image/urf' --data-binary '%PDF-1.7 fake'
```

依次应当是：连接被拒（非 0）、订阅被拒（非 0）、`400`。

## 9. 运维子命令

```bash
sudo -u stickbox /opt/stickbox/bin/stickboxd -conf /opt/stickbox/config.json device list
```

| 命令 | 用途 |
|---|---|
| `device add <dev> [name]` | 手工签发 device 密钥（调试用；正常走 App 的 enroll） |
| `device list` | 列出全部密钥 |
| `device revoke <key_id>` | 吊销一把 |
| `device unbind <dev>` | **强制解绑**，处理「二手转让但原持有人不配合」 |
| `user list` | 列出用户，只显示手机尾号 |
| `user phone <user_id>` | 解出完整号码，**每次调用写 `/opt/stickbox/audit.log`** |

`device unbind` 绕过所有权检查，所以只能在服务器上执行，不暴露为 API。
`user phone` 是唯一能解出完整号码的入口。

## 10. 还没退场的东西

- `server/bin/jobsrv.py` —— Python 版，切换后不再运行。**当前有未提交的本地
  改动，删除前先确认。** 回滚要用它
- `tools/reference/render.py`、`text2pdf.py` —— 已标注不再部署。
  `fix_page_count` 是 App 端 URF 编码器的参考实现，别删
- `/opt/stickbox/ppd/hp136a.ppd` —— 不再被任何代码读取，留作 render-profile
  参数的人工核对依据
