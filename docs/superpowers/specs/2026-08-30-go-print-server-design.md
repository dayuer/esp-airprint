# 云打印服务端 Go 重写 — 设计

日期：2026-08-30
状态：已评审，待实现

把 `server/bin/jobsrv.py`（265 行 Python：MQTT 客户端 + TLS HTTP + sqlite 队列）
重写为单个 Go 二进制 `stickboxd`，内嵌 MQTT broker，取代
`stickbox-job.service` + `mosquitto.service` 两个单元。

本文只写设计决策和它们的理由。已被否决的方案写在第 11 节，是为了让接手的人
不要再走一遍。

---

## 1. 为什么重写

现有 Python 版能跑，重写解决的是四个结构性问题，不是性能焦虑：

| 问题 | 现状 | 新版 |
|---|---|---|
| 无设备认证 | 全部设备共用一个 MQTT 口令；`/api/print` 完全无鉴权，任何人知道地址就能让打印机出纸 | 每设备一密钥，MQTT 与 HTTP 共用 |
| 作业可被任意下载 | `GET /api/job/{id}/data` 不校验任何身份和归属 | 必须持有该作业所属设备的密钥 |
| 派发逻辑靠锁和时间戳模拟串行 | `pump()` 用 `_pub` 字典防重发、用 `now-updated<180` 判忙、开临时线程调用自己 | 每设备一个 goroutine，状态私有，天然串行 |
| 目标设备靠猜 | `target_device()` 取「最近见过的那台」 | 上传方必须指名设备 |

顺带修掉的：证书续签后进程仍持旧证书（第 3 节）、同步渲染阻塞 HTTP 请求
（第 6 节）、进程重启后作业永久卡在 `downloading`（第 7 节）。

---

## 2. 范围

**做**：服务端重写、每设备密钥、ACL、纯管道化（不渲染）、纯 API 化。

**不做**（明确排除）：

- **不做网页上传页**。`server/web/index.html` 及 `/` 路由删除，服务端只有 JSON
  端点，上传由 App 负责。
- **不渲染**。服务端**完全没有渲染能力**。光栅由 App 用手机算力完成，上传的就是
  打印机原生格式（URF / PWG-Raster）。CUPS、PPD、cupsfilter、PangoCairo、
  `fonts-noto-cjk`、Python 全部退出部署依赖。详见第 7 节「纯管道」。
- **做用户体系**（手机号 + 短信验证码，见第 6b 节）。身份锚在用户不在设备，
  设备 enroll 时绑当前登录用户。**多人共享同一台打印机仍不在这一版范围内**——
  用户表是它将来的挂靠点，但共享要另设一套授权模型。
- **不做打印机适配测试**。用户参与的第 2 层适配、兼容性库、profile 下发是
  独立子系统，见 `2026-08-30-printer-compat-testing-design.md`，
  在本设计落地之后再做。
- **不做水平扩展**。目标是百台设备单机跑到头。

**目标规模**：数十到数百台设备长连，单机单进程。

单机容量的现实边界（供将来扩容参考，不是本次目标）：

| 项 | 单位成本 | 1 万台 | 10 万台 | 100 万台 |
|---|---|---|---|---|
| TLS 长连（Go） | 40~80KB/连接 | 0.5~1GB | 5~8GB | 50~80GB，不现实 |
| actor goroutine | 4~8KB | 忽略 | ~0.5GB | ~6GB |
| 心跳（30 秒一拍） | — | 330 msg/s | 3.3k msg/s | 33k msg/s |

服务端不渲染之后没有 CPU 密集路径，瓶颈依次是：**磁盘**（URF 是光栅，单份
200KB~15MB，比 PDF 大一到两个数量级，作业目录要按这个量级算容量和清理策略）、
**上行带宽**、然后才是连接数内存。真要扩，先换大盘和加清理，再把 sqlite 换成
Postgres 让多实例共享状态。

---

## 3. 进程与监听

单二进制 `stickboxd`：

- `:8883` MQTT over TLS（内嵌 [mochi-mqtt](https://github.com/mochi-mqtt/server) v2）
- `:9443` HTTPS（纯 JSON API）

不再监听 `1883`。服务端与 broker 同进程，派发信令是一次内存中的函数调用，
不走网络。

### 证书热重载

两个监听共用同一份 Let's Encrypt 证书，经 `tls.Config.GetCertificate` 回调按需
取，证书文件 mtime 变化即重新加载。

现状是靠 `/etc/letsencrypt/renewal-hooks/deploy/mosquitto.sh` 重启进程换证书，
钩子一丢就是「证书续了但服务还拿着旧的」。回调重载后钩子只是可选优化，
不再是正确性的前提。

---

## 4. 包结构

```
server/go/
  cmd/stickboxd/main.go     启动装配、优雅退出、device 子命令
  internal/config/          config.json
  internal/tlsx/            证书热重载
  internal/store/           sqlite：jobs / devices 表（modernc.org/sqlite，纯 Go 无 cgo）
  internal/auth/            密钥校验（argon2id）+ 会话缓存 + ACL 判定
  internal/broker/          内嵌 broker、auth/ACL/publish 钩子
  internal/device/          设备 actor：mailbox + 状态机
  internal/registry/        devid → mailbox 分片表、actor 生命周期
  internal/render/          子进程调 render.py，并发信号量
  internal/httpapi/         四个端点
```

边界规则：`device` 包不知道 HTTP 和 MQTT 的存在，只收 `Msg`、只调两个接口
（`Store` 落库、`Publisher` 发信令）。这是它能被单测的前提，也是这次拆包的
主要目的。

---

## 5. 设备 actor（核心）

每台在线设备一个 goroutine，私有以下状态，**无锁，因为只有它自己碰**：

```go
type actor struct {
    dev      string
    inflight string        // 正在传的作业 id，空 = 空闲
    deadline time.Time     // inflight 的 180s 超时点
    box      chan Msg      // buffered 16
}
```

消息四种：

| 消息 | 来源 | 处理 |
|---|---|---|
| `Heartbeat` | MQTT `printer/{dev}/status` | 更新在线状态与打印机面板状态 |
| `JobDone` / `JobFailed` | 同上 | 落库、清 `inflight`、立即派下一件 |
| `Downloading` | HTTP 取件开始 | 续 `deadline` |
| `Wake` | HTTP 上传完成、20 秒巡检 | 若空闲则派一件 |

主循环：

```
for {
  select {
  case m := <-box:      按类型更新；若空闲则 dispatchOne()
  case <-timer:         inflight 超时 → 退回 queued → dispatchOne()
  case <-idleTimer:     5 分钟无心跳 → 退出，从 registry 摘除
  }
}
```

`jobsrv.py` 的 `_pub` 去重字典、`busy` 判断、`dev != pick_device()` 自检、
`pump()` 开临时线程调用自己——这几样在这里**不是被重写，是不再需要**。
「一次只派一件」从一条要靠 SQL 查询维持的不变量，退化成 `inflight != ""`
这个局部变量。

### 两条硬规则

1. **作业队列的真相永远在 sqlite，不在 actor 内存里。** actor 只缓存 `inflight`
   一件。设备离线 5 分钟后 actor 退出，队列毫发无损；重连时新 actor 从 sqlite
   重建。这是防 goroutine 泄漏，也是防「内存队列随进程重启蒸发」。
2. **actor panic 由 registry recover 并重建**，`inflight` 作业退回 `queued`。
   一台设备的 bug 打不死进程。

「一次只派一件」的原因不变，来自设备侧约束：ESP32 可用堆几十 KB，没有本地队列，
一次性堆过去等于丢件。

---

## 5b. 一个桥，多台打印机

用户换打印机是常态，不是边缘情况。同一个桥先后插过几台机器，每台都要各自记住
自己的档案；插回来就该认出来。

数据模型天然支持：`printers` 表按**打印机序列号**做主键（HANDOFF 3.6：序列号
每台唯一，MAC 标识的是桥，两者混用就分不开「换了打印机」和「换了桥」）。

### 作业必须绑定打印机序列号

**这是正确性问题，不是功能。** App 上传的是按 `render-profile` 光栅好的 URF——
600dpi、A4 的确切像素尺寸、8 位灰度。换一台打印机后，队列里为旧机器排的作业
照样派出去，尺寸色彩全不对，出来就是废纸。而且**静默**：服务端不解析文档、
设备不认识格式，没有任何环节会发现。

所以：

1. `jobs.serial` 记录该作业为哪台打印机光栅。上传时由 App 提供
   （`X-Printer-Serial`，值来自它拉 `render-profile` 时拿到的那个）
2. 设备心跳带**当前插着的** `serial`，actor 维护 `currentSerial`
3. 派发前校验 `job.serial == actor.currentSerial`，不匹配**不派**
4. 队首查询变成 `NextQueued(dev, serial)`——只取当前打印机的活

### 换机后旧作业怎么办

**留在队列里，不失败也不删。** 用户很可能只是临时换台机器打点别的，插回来就该
接着打。这是「设备离线不丢件」这条承诺的自然延伸——只不过「不在线」的粒度
从桥细化到了打印机。

作业超过保留期（第 8 节）才被清理，和普通作业一视同仁。

### 换机的完整动作

```
设备检测到新打印机（serial 变了）
  → 第 0/1 层探测 → POST /ident
  → 服务端 upsert printers，按新 serial 分层查 profile
  → 下发 printer/{dev}/profile（带新 serial）
  → actor 更新 currentSerial，重新挑队首
  → 旧机器的作业留在队列，新机器的活（如果有）开始派
```

设备侧的对应规则已写进接口文档：**`profile` 的 `serial` 与当前打印机不符时
忽略这份档案**。retain 的旧档案会在换机后先到，套上去就是错的。

### actor 的 currentSerial 从哪来

从心跳，不从 `ident`。`ident` 是 retain 消息且只在枚举完成时发一次；用户拔掉
打印机换一台的瞬间，服务端需要立刻知道，靠心跳每 30 秒一拍（状态变化 2 秒内）
才跟得上。`ident` 负责的是「这台机器是什么」，心跳负责「现在插的是哪台」。

---

## 6. 认证、ACL 与密钥管理

### devices 表

```
dev TEXT, key_id TEXT PRIMARY KEY, role TEXT, name TEXT,
key_hash TEXT, created INT, last_seen INT, disabled INT
```

一台设备可以有多把密钥，按角色分：

| role | MQTT 发布 | MQTT 订阅 | HTTP |
|---|---|---|---|
| `device` | `printer/{dev}/status`、`printer/{dev}/ident` | `printer/{dev}/job`、`printer/{dev}/cmd` | 取件、上报 ident |
| `app` | 无 | `printer/{dev}/status`（本用户名下的所有 dev） | 上传作业、查状态、enroll 设备 |

App 与设备共用一把密钥的话，App 就能伪造打印机状态。分角色的代价是表里多一列、
ACL 判定多一个维度，几乎为零，所以做。

### 校验

- 哈希用 **argon2id**。明文密钥只在 `stickboxd device add` 时打印一次，不落盘。
- MQTT CONNECT 钩子：`username=devid`、`password=key`。
- 密钥明文格式为 `{key_id}.{secret}`，校验时按 `key_id` 直接定位到唯一一行，
  **不遍历该设备的所有密钥逐个 argon2**——那会让校验开销随密钥数线性增长。
  HTTP 的 `Authorization: Bearer` 用同一格式。
- **并发信号量**：argon2id 每次校验占 64MB 内存。一万台设备同时重连（服务重启后
  就是这个场景）时并发跑会瞬间吃掉几十 GB。同时进行的校验数限制为 8，超出的排队，
  **不是并发执行**。这条比缓存更要命——缓存冷的时候正是重连风暴。
- **必须有校验缓存**：argon2id 单次几十毫秒是设计目的，但取件路径每次都算会直接
  毁掉低延迟目标。首次校验通过后缓存 `key → sha256`，后续 HTTP 请求走 sha256
  常数时间比较。TTL 1 小时，`device revoke` 立即清除。

### 管理面

子命令直接操作同一个 sqlite，给运维和排障用。正常用户走 App 的 enroll，
不碰这些：

```
stickboxd device add <dev> [--name X]    手工签发 device 密钥（调试用）
stickboxd device list [--user <id>]
stickboxd device revoke <key_id>
stickboxd device unbind <dev>            强制解绑，处理「原持有人不配合」
stickboxd user list
stickboxd user find <手机号>              按 HMAC 查，用于申诉核验
stickboxd user phone <user_id>           解出完整号码。每次调用记审计日志
```

`device unbind` 是抢绑防护的逃生门。它绕过所有权检查，所以只能在服务器上执行，
不暴露为 API。

---

## 6b. 用户体系：手机号 + 短信验证码

### 身份锚在用户，不在设备

设备密钥不再是账号。锚是**用户**，App 登录后拿 session token，设备 enroll 时
绑到当前登录用户。

带来的直接好处：

- 配网时用户**一个字符都不用输**——App 拿自己的 session 去 enroll，
  服务端签发 device 密钥，App 连同 Wi-Fi 凭据一起写进设备
- 换手机、重装 App 都能登回来，账号可找回
- 设备重置后历史不丢
- 二手转让被拒时用户能走申诉，不是死路

### 表

```sql
CREATE TABLE users(
  id         TEXT PRIMARY KEY,  -- 随机 16 字节 hex
  phone_hmac TEXT UNIQUE,       -- HMAC-SHA256(手机号, 服务端 pepper)，登录按它查
  phone_tail TEXT,              -- 尾 4 位，界面展示
  created    INTEGER,
  last_login INTEGER,
  disabled   INTEGER DEFAULT 0
);

-- 完整号码单独一张表，热路径（登录、ACL、派发）永远不碰它
CREATE TABLE user_phones(
  user_id   TEXT PRIMARY KEY,
  phone_enc BLOB,              -- AES-256-GCM，nonce 前置
  updated   INTEGER
);

CREATE TABLE sms_codes(
  phone_hmac TEXT PRIMARY KEY,
  code_hash  TEXT,      -- 验证码也不存明文
  expires    INTEGER,
  attempts   INTEGER DEFAULT 0,
  sent_at    INTEGER
);

-- devices 表加一列
ALTER TABLE devices ADD COLUMN user_id TEXT NOT NULL DEFAULT '';
```

### 手机号的三种形态

同一个号码在库里以三种形态存在，各有各的用途，不要混：

| 形态 | 在哪 | 干什么 | 可逆 |
|---|---|---|---|
| `phone_hmac` | `users` | 登录查询、唯一性约束 | 否 |
| `phone_tail` | `users` | 界面展示「138****8888」 | 否 |
| `phone_enc` | `user_phones` | 客服核验、换号迁移、通知短信、数据导出 | **是** |

**登录路径只碰 HMAC。** 验证码校验、session 签发、ACL 判定全程不解密，
所以高频路径没有解密开销，也没有明文在内存里长期驻留。

**完整号码单独成表且加密存储。** 加密是 AES-256-GCM，密钥 `phone_key` 放
`config.json`。加密不影响任何正当用途——服务端持有密钥，
`stickboxd user phone <id>` 随时解得出来——但库文件被拷走、或备份泄露时，
拿到的是密文。这个成本接近零，收益是把「一次备份泄露」和「全量手机号泄露」
解耦。

两把密钥的运维含义不同，别搞混：

- **`phone_pepper`** —— 换了它**全体用户无法登录**（HMAC 对不上）。绝不轮换。
- **`phone_key`** —— 换了它旧的 `phone_enc` 解不开，但登录不受影响。
  真要轮换得写一次全表重加密。

两者都放 `config.json`（`600`，不入库）。

**备份策略要跟着改**：`jobs.db` 现在含个人信息，备份文件必须加密且限权，
不能像以前那样随手 `scp` 到本地。

### 短信防刷（必需，不是加固项）

短信每条都有钱，被刷是烧钱加骚扰他人。四道闸：

| 闸 | 限制 |
|---|---|
| 同号码发送间隔 | 60 秒 |
| 同号码每日上限 | 10 条 |
| 同 IP 每小时上限 | 20 条 |
| 单个验证码尝试次数 | 5 次，超了作废重发 |

验证码 6 位数字，TTL **5 分钟**，验证成功立即失效（一次性）。
`sms_codes` 表按 `phone_hmac` 单行覆盖——重发即替换，不留历史。

短信服务商（阿里云 / 腾讯云）抽象成 `SMSSender` 接口，测试用假实现，
**单测和集成测试绝不真发短信**。

### session token

登录成功签发一个 `{key_id}.{secret}`，形式与设备密钥一致，复用同一套
argon2id 校验和缓存。区别只在 `devices` 表里的 `role='app'` 且 `user_id` 非空。

- **一台手机一个 token**，签发时记录设备描述（机型），App 的设置页可以
  「退出其他设备」
- 手机丢了单独吊销那一把，不影响别的
- 无过期时间。打印是低频操作，强制重新登录只会激怒用户；要下线就靠吊销

### ACL 的连带改动

`app` 密钥绑的是 **user**，一个用户可以有多个桥。ACL 判定从
「topic 里的 dev 等于身份里的 dev」变成「topic 里的 dev 属于该 user」，
需要一张 `user_id → devs` 的内存缓存，随 enroll / 解绑失效。

`device` 角色不变，仍然只能碰自己那一个 dev。

### 数据归属

| 数据 | 锚在哪 | 设备重置后 | 注销账号后 |
|---|---|---|---|
| `user_phones` 行（完整号码） | user | 保留 | **删除** |
| 作业记录、`.urf` 文件 | user | 保留 | **删除** |
| 设备绑定关系 | user | 保留 | 删除（设备变成未绑定，可被重新 enroll） |
| session token | user | 保留 | 全部吊销 |
| `users` 行 | user | 保留 | 删除 |
| `printers` / `profiles` / `test_runs` | **打印机序列号** | 保留 | **保留** |

最后一行是刻意的。适配结论按打印机序列号存，绑的是硬件不是人，本来就要跨账号
复用（3 台一致才提升为机型级）。里面没有任何可以关联到自然人的字段——
`test_runs.dev` 是桥的 MAC，注销时一并清空即可。

### 注销账号

个人信息保护法的硬要求，不是可选功能。`POST /api/account/delete` 立即执行
上表右列，**不做「30 天冷静期」**——那需要额外的状态机和定时任务，
而这里没有值得挽回的东西。

### 重置设备

不另设机制：**给一个已绑定到本账号的 dev 重新 enroll，就是重置。**

用户按住按键恢复出厂时，设备清了 NVS，服务端全程不知情——所以不能靠设备上报，
也不能让 App 去点「重置」（那需要旧密钥，已经擦了）。落在 enroll 上就不需要
任何额外协调：吊销该 dev 的旧 device 密钥、签发新的，其余数据不动。

### 抢绑防护

`enroll` 只凭 session 和一个 MAC，服务端无法验证 App 是否真的物理接触了设备。
不设防的话，任何人拿自己的账号加别人的 MAC 就能重置别人的设备——现成的 DoS。

| 情况 | 结果 |
|---|---|
| dev 未被任何用户绑定 | 允许，先到先得 |
| dev 已属于本用户 | 允许（这就是重置） |
| dev 已属于**别的**用户 | **拒绝 409**，提示「该设备已绑定到其他账号」 |

转让二手设备要原持有人先在 App 里解绑。和 Apple 激活锁同一个模型、同一个代价。
区别是有了真实身份之后，用户至少能走申诉——运维用
`stickboxd device unbind <dev>` 强制解绑，该命令绕过所有权检查，
**只能在服务器上执行，不暴露为 API**。

---

## 7. API

四个端点，全部要求 `X-Device` + `Authorization: Bearer <key>`。

| 端点 | 角色 | 说明 |
|---|---|---|
| `POST /api/print` | `app` | 上传**已光栅的** URF / PWG-Raster，校验后直接入队 |
| `GET /api/device/{id}/render-profile` | `app` | 下发该设备打印机的光栅参数 |
| `GET /api/job/{id}/data` | `device` | 设备流式取件。校验 `job.dev == devid` |
| `POST /api/device/{id}/ident` | `device` | 机型档案上报。校验 `id == devid` |
| `GET /api/status` | 两者 | 只返回该设备及其作业 |
| `POST /api/auth/sms` | 无 | 发送短信验证码（四道防刷闸） |
| `POST /api/auth/verify` | 无 | 校验验证码，签发 session token；用户不存在则创建 |
| `POST /api/auth/logout` | `app` | 吊销当前 token（`?all=1` 踢掉全部） |
| `POST /api/device/enroll` | `app` | 为一台设备签发 device 密钥并绑到当前用户 |
| `POST /api/device/{id}/unbind` | `app` | 解绑，转让二手设备用 |
| `POST /api/account/delete` | `app` | 注销账号，立即删除个人数据 |

`pick_device()` / `target_device()` 整个删除——上传方现在必须说出打到哪台。

### 纯管道：渲染搬到手机

打印发起端只有 Android 和 iOS，两边都有足够算力自己光栅。所以服务端不再渲染，
只做校验、排队、派发、流式转发。

**服务端因此少掉的东西**：CUPS 全家、`hp136a.ppd`、`cupsfilter`、PangoCairo、
`fonts-noto-cjk`、Python 运行时。中文字体那个坑（`texttopdf` 出方框、Debian 的
`paps` 写死 `/Helvetica`）随之消失——手机自带 CJK 字体渲染，根本遇不到。
部署物退化成「一个 Go 二进制 + 一个 sqlite 文件」。

**机型知识仍然收敛在服务端**，只是形态变了：从「服务端拿 PPD 去渲染」变成
「服务端把光栅参数下发给客户端」。参数来源是设备自己上报的 `ident`
（URF 能力串如 `RS600,W8` 已经在里面），不是硬编码。换打印机改服务端，
App 不用动——HANDOFF 第 1 节的原则保住了。

**必须做入口校验**，这是这条路线唯一的新风险。客户端可能上传一份 PDF 却标称
URF，设备是根管子不认格式，会把它当正文送进打印机，用户收到几十张乱码纸。校验三条：

1. 魔数：URF 必须以 `UNIRAST\0` 开头，PWG-Raster 必须以 `RaS2` 开头
2. 页数字段非 0——`render.py` 的 `fix_page_count` 就是为这个写的：Debian 版
   `cupsfilter` 写 0，打印机据此认为文档为空。客户端编码器同样会犯
3. 首页页头的宽高在合理区间（0 < w,h < 30000），且与该设备的 render-profile 一致

校验不通过返回 400 并说明哪一条不过，**不入队**。

上传与取件全程 `io.Copy` 流式，不整包进内存。作业体积变大之后这条更要紧了。

### 进度回传

App 直接连 `:8883`，用 `role=app` 的密钥订阅 `printer/{dev}/status`，实时收到
传输进度和打印机面板状态。服务端零新代码——broker 和 ACL 都是现成的。
不做 SSE，那等于把 broker 已有的能力在 HTTP 上再实现一遍。

---

## 8. 存储

sqlite + WAL，`busy_timeout=5000`。写走单一带 `Mutex` 的连接（百台量级写入量极低），
读走连接池。

`jobs` 表沿用现有列，新增：

- `err TEXT` —— 失败原因（设备回报的 USB 错误等）。现在丢在 HTTP 响应里，
  请求一结束就没了。
- `serial TEXT` —— **作业为哪台打印机光栅的**。见第 5b 节，这是正确性问题。

作业状态机是 `queued → downloading → done / failed`。**没有 `rendering` 状态**——
上传校验通过即入队，校验不通过直接 400，不落库。

### 作业文件清理

URF 比 PDF 大一到两个数量级，不清理会很快把盘写满。规则：

- 作业进入 `done` 后保留 **24 小时**，然后删除 `.urf` 文件，数据库记录保留
- `failed` 保留 **72 小时**（要留给排查）
- 每台设备最多保留 **50 件**未删除的作业文件，超出按时间删最旧的
- 后台每小时扫一次

Python 版没有任何清理，因为 PDF 渲染出的 URF 也大但作业量小。现在作业更大、
路径更短，清理必须内建，不能靠 cron 脚本——那是会被忘记的东西。

**启动时把 `state='downloading'` 的作业退回 `queued`。**
Python 版只处理了 `no-device` 一种，进程重启时正在传的作业会永远卡在
`downloading`，没有任何东西会救它。

---

## 9. 错误处理

| 情况 | 处理 |
|---|---|
| 渲染失败 | `state=failed`，错误文本入 `err` 列 |
| 传输 180 秒无进展 | 退回 `queued` 重传（保留现有语义） |
| 设备离线 | actor 退出，作业留 sqlite，重连时恢复 |
| actor panic | registry recover 并重建，`inflight` 退回 `queued` |
| 进程重启 | 见第 8 节 |
| 认证失败 | MQTT 拒绝连接并记日志；HTTP 401，不区分「设备不存在」与「密钥错」 |

---

## 10. 测试

- **store**：状态流转、启动恢复（含 `downloading` 退回）
- **auth**：哈希校验、缓存失效、**ACL 矩阵——设备 A 订阅 B 的 topic 必须被拒，
  `app` 角色发布 status 必须被拒**
- **device**：注入假 `Publisher` 和可控时钟，逐条验证「一次只派一件」
  「done 后立即派下一件」「180s 超时退回队列」「离线退出后队列完好」
  「panic 后作业不丢」
- **render**：用假二进制替身，不依赖 CUPS
- **集成**：起真 broker + 真 HTTP，假设备客户端跑完整链路
  （上传 → 派发 → 取件 → 回执）

---

## 11. 被否决的方案

| 方案 | 否决原因 |
|---|---|
| 保留 mosquitto，Go 只做客户端 | 「一体」就只剩进程合并，认证仍归 mosquitto 管，每设备密钥要维护两套配置 |
| 单体 Hub + 全局锁 | `pump()` 的时序逻辑仍要靠锁和时间戳字典拼，竞态原地不动 |
| 全局事件总线（单 goroutine 顺序消费） | 所有设备挤一条串行流水线，一台设备的慢操作阻塞全局 |
| 每设备一张 mTLS 客户端证书 | ESP32 上多存证书+私钥（flash 和握手内存都涨），还要自建签发/到期/吊销三套流程 |
| Postgres | 百台量级用不上，多一个外部依赖 |
| 删掉 HTTP 服务器（只留 MQTT） | **不可行。** 设备可用堆 70~120KB，作业 100~500KB，MQTT 消息必须整包进内存，塞不下。HTTPS 流式拉取自带 TCP 反压，设备内存恒定。取件端点是整套架构的地基 |
| 用 SSE 回传进度 | broker 已在同进程，等于重复实现 |
| 服务端渲染（保留 CUPS + PPD） | 打印发起端只有 Android 和 iOS，两边都有算力自己光栅。服务端渲染换来的是一整套 CUPS/字体/Python 依赖、一个 CPU 天花板、以及中文字体那类只在服务端才发生的坑 |
| 服务端渲染作为客户端失败时的兜底 | 兜底要求服务端仍装 CUPS 全家，等于「纯净化」一样没做到。没有老版本 App、没有非移动端来源，这条兜底没有服务对象 |
| 客户端硬编码光栅参数 | 机型知识会从服务端漏到 App，换打印机要发版加审核。改为服务端下发 render-profile |
| 身份锚在设备（用户手抄密钥配网） | 45 个字符手抄，对普通用户是灾难。锚在用户之后一个字符都不用输 |
| 匿名账号 + 恢复码 | 把丢失点从设备挪到手机，而手机换得更勤。匿名账号丢了没有任何客服手段能救 |
| 邮箱 + 自建密码 | 要背密码哈希与泄露风险、验证邮件送达率、重置流程、撞库防护。这些和打印一点关系都没有 |
| 完整号码存在 `users` 表里 | 登录和 ACL 是高频路径，把个人信息放在热表里意味着每次查询都可能把它带进内存和日志。单独成表，热路径永不触碰 |
| 完整号码明文存储 | 加密后服务端照样随时解得出来，正当用途一个不少；但库文件或备份被拷走时拿到的是密文。成本接近零 |
| session token 设过期时间 | 打印是低频操作，强制重新登录只会激怒用户。要下线靠吊销 |
| 注销账号设 30 天冷静期 | 需要额外的状态机和定时任务，而这里没有值得挽回的东西 |
| `enroll` 不设抢绑防护 | 任何人拿自己账号 + 别人的 MAC 就能重置别人的设备，是条现成的 DoS |

---

## 12. 固件侧连带改动

本设计只定接口，实现另开一轮：

1. `main/cloud_creds.h` 的 `MQTT_USER` / `MQTT_PASS` 常量 → 改为从 NVS 读设备密钥，
   username 用 MAC
2. `main/cloud_client.c` 的取件（`/job/{id}/data`）和 ident POST 加
   `Authorization: Bearer` 头
3. `main/provision.c` / `portal_html.h` 配网页加「设备密钥」输入框

---

## 13. 迁移与回滚

- 复用同一个 `/opt/stickbox/jobs.db`：加 `devices` 表、给 `jobs` 加 `err` 列，
  旧数据不动
- 端口不变（9443 / 8883）
- **mosquitto 直接下线**，不设并行期。设备侧要同步改密钥，并行期只会让两条
  认证路径同时半生不熟
- 切换：停 `stickbox-job.service` 和 `mosquitto.service`，起 `stickboxd`
- 回滚：反过来。数据库双向兼容——旧版忽略新增的表和列

### 渲染链路退场

服务端不再渲染，以下东西退出部署：

| 东西 | 处置 |
|---|---|
| `server/bin/render.py` | 移到 `tools/reference/`，文件头标注「不再部署」。**留档不是念旧**：它的 `fix_page_count` 是客户端 URF 编码器唯一的参考实现，那个坑客户端一样会踩 |
| `server/bin/text2pdf.py` | 同上。PangoCairo 那套 CJK 处理是踩出来的，删了就没了 |
| `server/web/index.html` | 删除。纯 API，无网页 |
| `/opt/stickbox/ppd/hp136a.ppd` | 服务器上保留但不再被任何代码读取，作为 render-profile 参数的人工核对依据 |
| CUPS / `cupsfilter` / `fonts-noto-cjk` / `python3-gi` 等 | 不再是部署依赖。**先别急着从服务器卸载**——跑稳一个月再清，卸载是不可逆的 |

**回滚的前提**：回滚到 Python 版需要 CUPS 仍在。这就是上面最后一条的理由。

---

## 14. 尚未定义的

诚实标注，别当成已有能力：

- **二手设备转让**。原持有人不在 App 里解绑，新持有人就绑不上，只能走申诉由
  运维 `stickboxd device unbind`。和 Apple 激活锁同一个模型，同一个代价。
- **多人共享一台打印机**。`users` 表是它将来的挂靠点，但共享要另设授权模型
  （谁能打、能不能看别人的作业、怎么撤销），这一版不做。
- **更换手机号**。用新号登录会创建新账号，老账号的设备和历史不跟过去。
  已登录状态下换绑新号的流程没做。
- **国际号码**。只处理中国大陆号码，短信服务商和号码格式校验都按这个假设写。
- **配额与计费**。没有任何限额。
