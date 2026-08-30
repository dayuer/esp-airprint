# 云打印服务端 接口文档 v2

面向：固件开发（ESP32 侧）、App 开发（上传侧）。

对应服务端：`airprintd`（Go 重写版，设计见
`docs/superpowers/specs/2026-08-30-go-print-server-design.md`）。

**与 v1（`jobsrv.py`）不兼容**，差异清单在第 8 节。固件按本文实现即可，
不需要等服务端写完——协议已经冻结。

---

## 0. 约定

- 所有时间戳是 Unix 秒（UTC）。
- 所有 JSON 用 UTF-8，不带 BOM。
- `{dev}` 是设备 ID：**ESP32 的 STA MAC 去掉冒号的小写十六进制**，12 个字符，
  例如 `f412fa87c9e0`。全文出现的 `{dev}` 都指这个。
- 服务端主机：`mqtt.silkline.id`。MQTT `:8883`，HTTPS `:9443`。

---

## 1. 身份与凭证

系统里有两类主体、三种凭证。**固件只关心 device 密钥**，其余是给 App 看的。

| 主体 | 凭证 | 怎么来的 | 能做什么 |
|---|---|---|---|
| 用户（手机 App） | **session token** | 手机号 + 短信验证码登录 | 管自己名下所有设备：上传作业、查状态、enroll、解绑 |
| 桥（ESP32） | **device 密钥** | App 在配网时通过 enroll 拿到并写进设备 | 收作业信令、发状态、取件、上报机型档案 |

### 1.1 令牌格式

session token 和 device 密钥**格式完全一样**，服务端用同一套校验：

```
{key_id}.{secret}
```

- `key_id`：12 个小写十六进制字符
- `secret`：32 个字符，Base64 URL 变体（`A-Z a-z 0-9 - _`，无 `=` 填充）
- 完整 45 字符，例如：

```
a3f91c04bd77.kJ8xQ2mN4pL7vR1sT6wY9zA0bC3dE5fH
```

**当不透明字符串处理，不要解析、不要截断、不要打进日志。** 固件分配 64 字节缓冲。

### 1.2 设备 ID

`{dev}` 是 **ESP32 的 STA MAC 去掉冒号的小写十六进制**，12 个字符，
例如 `f412fa87c9e0`。全文出现的 `{dev}` 都指这个。

### 1.3 固件怎么拿到密钥

**用户不再手抄密钥。** 流程是 App 拿自己的 session 去换一把 device 密钥，
再连同 Wi-Fi 凭据一起写进设备：

```
App 登录 → POST /api/auth/sms → POST /api/auth/verify → 拿到 session token
用户配网 → App 连设备的 SoftAP
        → POST /api/device/enroll（带 session + 设备 MAC）
        → 服务端签发 device 密钥，绑到当前用户，返回给 App
        → App 通过 SoftAP 把 Wi-Fi 凭据和 device 密钥一起写进设备
```

设备侧存储：

- NVS namespace：`cloud`
- key：`devkey`
- 类型：字符串，最长 64 字节

**密钥为空时固件不要尝试连接**——直接停在配网态并在 LCD 上提示，
否则会陷入「连接被拒 → 重连」的死循环。

### 1.4 重置

**给一个已属于本账号的 dev 重新 enroll，就是重置**：旧 device 密钥被吊销，
新的立即生效，用户的作业历史和设备绑定关系都保留。

用户按住按键恢复出厂设置时设备清了 NVS，服务端全程不知情——所以重置不靠设备
上报，落在 enroll 上。固件侧不需要任何配合。

### 1.5 抢绑防护

`enroll` 一台**已属于其他账号**的设备会被拒（409）。转让二手设备要原持有人
先在 App 里解绑。

## 2. TLS

- MQTT 和 HTTPS 共用同一份 Let's Encrypt 证书（`mqtt.silkline.id`）。
- 固件校验服务端证书链，**不要用 `skip_cert_common_name_check`**。
- 根证书用 ISRG Root X1，编进固件（`esp_crt_bundle` 也可以，体积换省心）。
- 不使用客户端证书（mTLS 已被否决，理由见 spec 第 11 节）。

---

## 3. MQTT 接口

### 3.1 连接参数

| 参数 | 值 |
|---|---|
| URI | `mqtts://mqtt.silkline.id:8883` |
| client_id | `{dev}` |
| username | `{dev}` |
| password | 完整令牌 `{key_id}.{secret}` |
| clean_session | `true` |
| keepalive | 60 秒 |
| 协议 | MQTT 3.1.1（v5 也接受，但不使用 v5 特性） |

**认证失败**：服务端回 CONNACK `0x05`（Not authorized）后断开。固件应当停止
重连并在 LCD 报错——密钥错了，重试一万次也是错的。**只有网络类断开才重连。**

### 3.2 遗嘱（LWT）

必须设置，否则服务端只能靠 5 分钟静默超时判离线。

- topic：`printer/{dev}/status`
- QoS 1，retain **1**
- payload：

```json
{"dev":"f412fa87c9e0","state":"offline"}
```

### 3.3 Topic 表

`{dev}` 必须是自己的 ID。**订阅或发布别人的 topic 会被 ACL 拒绝**，
且会记录在服务端日志里。不要用通配符订阅。

| Topic | 方向 | QoS | retain | 说明 |
|---|---|---|---|---|
| `printer/{dev}/job` | 服务端 → 设备 | 1 | 0 | 派发作业 |
| `printer/{dev}/cmd` | 服务端 → 设备 | 1 | 0 | 下发命令（探针、测试） |
| `printer/{dev}/profile` | 服务端 → 设备 | 1 | **1** | USB 层怪癖档案，设备存 NVS |
| `printer/{dev}/status` | 设备 → 服务端 | 1 | 0 | 心跳与回执 |
| `printer/{dev}/ident` | 设备 → 服务端 | 1 | **1** | 精简机型身份 |

连接后订阅 `job`、`cmd`、`profile` 三个。

### 3.4 服务端 → 设备：`printer/{dev}/job`

```json
{"id":"7a3f91c04bd7","size":284160}
```

| 字段 | 类型 | 说明 |
|---|---|---|
| `id` | string(12) | 作业 ID，取件时要用 |
| `size` | int | URF 字节数，用于显示进度和预判 |
| `hooks` | object | **可选**。本次作业的一次性钩子覆盖，见下 |

带 `hooks` 时形如：

```json
{"id":"7a3f91c04bd7","size":81920,"hooks":{"job_end":[]}}
```

上面这一条就是「本次不发作业结束符」——**空数组，不是布尔开关**。
测试变体和 profile 用同一套模型，格式、原语、上限全部一致，
固件不需要第二套解析代码。

**只对本次作业生效，绝不写 NVS。** 这是适配测试用的：服务端要试
「不发 UEL 会怎样」，但测完设备必须回到原状态。作业结束即销毁这份覆盖，
**失败也要销毁**——否则用户测一次就打不了印了。

只覆盖出现在 `hooks` 里的那些钩子；没提到的仍按 NVS 里的档案执行。
完全没有 `hooks` 字段时全按档案走。

**服务端保证同一时刻只派一件**（设备没有本地队列，堆过去等于丢件）。
收到即应开始取件；正在打的时候收到新作业，说明服务端认为上一件已结束——
以服务端为准。

### 3.5 服务端 → 设备：`printer/{dev}/cmd`

```json
{"probe":"@PJL INFO ID"}
```

目前只有 `probe` 一个字段。执行后把结果发到 `status`（见 3.6 的 `probe` 字段）。
未识别的字段一律忽略，不要报错。

### 3.6 设备 → 服务端：`printer/{dev}/status`

一条 payload 兼作心跳和回执。

```json
{"dev":"f412fa87c9e0","job":"","state":"ready","bytes":0,"heap":120708,
 "serial":"CNB9K1P2X4",
 "prn":{"code":10001,"display":"Ready","online":true,"asleep":false,
        "paper_out":false,"error":false}}
```

`serial` 是**换打印机场景的关键字段**。服务端靠它知道现在插的是哪台，
从而只派属于这台机器的作业。`ident` 是 retain 且只在枚举完成时发一次，
跟不上拔插；心跳跟得上。**用户拔掉打印机后 `serial` 必须立刻变成空串。**

| 字段 | 类型 | 必填 | 说明 |
|---|---|---|---|
| `dev` | string | 是 | 自己的 ID |
| `job` | string | 是 | 当前作业 ID，空闲时空串 |
| `state` | string | 是 | 见下表 |
| `bytes` | int | 是 | 本作业已处理字节数 |
| `heap` | int | 否 | 可用堆，服务端只用于日志 |
| `serial` | string | **是（插着打印机时）** | 当前插着的打印机序列号。没插打印机时空串 |
| `prn` | object | 否 | 打印机面板状态，原样转给 App |
| `probe` | string | 否 | 探针回包（响应 `cmd` 时才带） |
| `skipped_ops` | array | 否 | 本固件不认识、已跳过的 profile 原语名。服务端据此知道该机型的档案用了新原语而这台设备还没跟上 |
| `profile_rev` | int | 否 | 当前生效档案的 `rev`。对不上说明下发没到位 |

`state` 取值：

| 值 | 含义 | 服务端动作 |
|---|---|---|
| `ready` | 空闲可接活 | 若有排队作业则派发 |
| `downloading` | 正在取件 | 续 180 秒超时 |
| `printing` | 正在送进 USB | 续 180 秒超时 |
| `done` | 作业完成 | 落库 `done`，**立即派下一件** |
| `failed` | 作业失败 | 落库 `failed`，**立即派下一件** |
| `offline` | 桥离线（LWT），**或桥在线但没插打印机** | 标记离线，不派件 |

**关于 `offline`**：设备除了 LWT，在**没插打印机时也会主动发 `offline`**。
从派件的角度看「桥断了」和「桥在但没打印机」是同一件事——都不能接活。
服务端要区分二者很容易：`seen` 新鲜就说明桥是活的，`prn` 还能说明具体原因。
这样不必为「活着但打不了」再加一个状态。

`done` / `failed` 时 `job` 字段**必须**填对应作业 ID，否则服务端不知道
是哪一件结束了，会一直等到 180 秒超时才重传。`failed` 时可带 `err` 字段
（字符串，≤200 字节）说明原因，会存进作业记录。

### 3.7 心跳节奏（这条是硬要求）

**状态一变立刻推（2 秒内），没变也每 30 秒推一次。**

固件早期只在状态变化时上报，结果板子好好的、打印机就绪，服务端却说
「没有在线的打印桥」——状态一直是 `ready` 没变过，它就再也不吭声了。
离线队列、断点续传、App 上的「设备离线」全部建立在判活之上。

服务端判离线的阈值是 **5 分钟无消息**。

### 3.7b 服务端 → 设备：`printer/{dev}/profile`

USB 层怪癖档案。retain=1，设备一连上就能拿到。

**它不是参数表，是一份可编排的动作序列。** 那 9 个字节的 UEL 不再是固件里的
常量，而是下面 `data` 字段里的一串十六进制。

```json
{
  "rev": 3,
  "match": "03F0:F22A",
  "serial": "CNB9K1P2X4",
  "src": "model",
  "flags": {"unidir": false, "pjl_ok": true},
  "hooks": {
    "job_begin": [{"op": "iface_reset"}],
    "job_end":   [{"op": "send_hex", "data": "1b252d313233343558"}],
    "wake":      [{"op": "send_hex", "data": "1b252d313233343558"},
                  {"op": "delay_ms", "ms": 300}]
  }
}
```

| 字段 | 说明 |
|---|---|
| `rev` | 档案版本号，单调递增。用于日志与排障 |
| `match` | `VID:PID`，仅供人看 |
| `serial` | 这份档案对应哪台打印机。**与当前插着的不符时忽略整份** |
| `src` | 来源层级：`serial`/`model`/`authoritative`/`quirks`/`default` |
| `flags` | 模式开关，不是动作 |
| `hooks` | 三个钩子，各是一串按顺序执行的步骤 |

#### 原语白名单

| op | 参数 | 行为 |
|---|---|---|
| `send_hex` | `data` | 往 bulk OUT 写这串字节 |
| `delay_ms` | `ms` | 等待 |
| `iface_reset` | — | 复位 USB 接口 |
| `read_status` | — | 读一次 IN 端点，结果并入下次心跳 |

`flags.unidir` 停用 IN 端点且不发 PJL；`flags.pjl_ok` 允许第 1 层 PJL 探针。

#### 三个钩子

| hook | 何时执行 |
|---|---|
| `job_begin` | 作业开始前 |
| `job_end` | 作业结束后 |
| `wake` | 从休眠唤醒时 |

**`job_end` 里不要放 `iface_reset`。** 实测会在距流尾几 KB 处 `Decoding Fail`
——最后一个短包还没物理冲出去就被 halt/flush 掉了。端点复位只在 `job_begin` 做。

#### 硬上限，越界拒绝整份

| 项 | 上限 |
|---|---|
| profile JSON 总长 | 1024 字节 |
| 每个 hook 的步数 | 8 |
| 单个 `send_hex` | 64 字节 |
| 单个 `delay_ms` | 5000 |
| 单个 hook 内 delay 总和 | 10000 |

越界**整份拒绝并退回内置兜底**，不要截断——半份 profile 比没有 profile 更危险。
服务端下发前也会校验一遍，但固件必须自己再验，不能假设上游一定对。

#### 未知原语

| 步骤 | 固件不认识时 |
|---|---|
| 默认（可选） | **跳过**，并在下次心跳里报 `"skipped_ops":["新原语名"]` |
| 标了 `"required": true` | **拒绝整份 profile**，退回内置兜底，并上报原因 |

这条让服务端可以先行支持新机型：新固件立即生效，老固件安全退回并报告，
不需要全网 OTA。

静默跳过一个关键的 `send_hex` 会让打印机行为错乱，而症状（第二份不出、乱码纸）
最难归因到「固件跳过了一步」——所以可跳过的步骤必须显式声明，其余按必需处理。

#### 处理规则

1. 收到后**先校验再存**，任何一条不过就整份丢弃
2. 存 NVS（namespace `cloud`，key `profile`），立即生效
3. `serial` 与当前插着的打印机不符时**忽略整份**——用户换了打印机，
   retain 的旧档案会先到，套上去就是错的
4. 查找顺序：NVS 档案 → 内置 `printer_profile.c` → `usb_quirks_db.h`。
   编译进固件的那张表从「真相」降级为「连不上服务端时的兜底」

### 3.8 设备 → 服务端：`printer/{dev}/ident`### 3.8 设备 → 服务端：`printer/{dev}/ident`

精简机型身份，retain=1，只在打印机枚举完成后发一次。

```json
{"dev":"f412fa87c9e0","vid":"03F0","pid":"F22A",
 "make":"HP","model":"HP Laser MFP 136a","serial":"CNB9K1P2X4",
 "proto":2,"cmd":"URF,PCL,PJL,PWGRaster"}
```

**上限 512 字节。** 全量机型档案（4~16KB）走 HTTPS，不走这里——一份十几 KB 的
retain 消息会让每个订阅者一连上就吃一大口。

### 3.9 重连

指数退避，起步 2 秒，上限 60 秒，带 ±20% 抖动（避免全网设备同时重连把服务端的
密钥校验压垮）。认证失败（CONNACK `0x05`）不退避——直接停。

---

## 4. HTTP 接口

### 4.1 通用

Base URL：`https://mqtt.silkline.id:9443/api`

除登录相关的两个端点外，**每个请求都要带 `Authorization`**：

```
Authorization: Bearer a3f91c04bd77.kJ8xQ2mN4pL7vR1sT6wY9zA0bC3dE5fH
```

**`X-Device` 只在 App 侧需要**——一个用户可能有多个桥，得说清楚操作哪一台。
设备侧的身份已经在密钥里，不需要这个头（带了也会被校验一致性）。

通用错误响应（`Content-Type: application/json`）：

| 状态码 | body | 含义 |
|---|---|---|
| 400 | `{"e":"bad request"}` | 参数或大小非法 |
| 401 | `{"e":"unauthorized"}` | 密钥错、已吊销、或角色不对 |
| 403 | `{"e":"forbidden"}` | 密钥有效但作业不属于这台设备 |
| 404 | `{"e":"not found"}` | 作业不存在或尚未渲染完 |

| 413 | `{"e":"too large"}` | 超过大小上限 |
| 415 | `{"e":"unsupported media type"}` | 上传的不是 URF / PWG-Raster |
| 429 | `{"e":"rate limited"}` | 短信发送被限流 |
| 500 | `{"e":"...","detail":"..."}` | 服务端故障，可重试 |

401 **不区分**「设备不存在」和「密钥错」——不给探测者提供信息。

### 4.1b 认证类端点（App 专用）

#### `POST /api/auth/sms` —— 发送验证码

无需身份。

```json
{"phone": "13800008888"}
```

只接受中国大陆手机号（`+86` / `86` 前缀和空格会被规整掉）。

**响应 200**：`{"ok":1,"ttl":300}` —— 验证码 5 分钟内有效。

**响应 429**：被限流。四道闸：同号码 60 秒间隔、同号码每日 10 条、
同 IP 每小时 20 条、单个验证码 5 次尝试。`detail` 说明撞的是哪一条。
**App 收到 429 不要自动重试**，把倒计时显示给用户。

#### `POST /api/auth/verify` —— 校验并登录

无需身份。用户不存在则自动创建。

```json
{"phone": "13800008888", "code": "123456", "device": "iPhone 15"}
```

`device` 是这台手机的描述，显示在「已登录设备」列表里，可省略。

**响应 200**

```json
{"token":"a3f91c04bd77.kJ8x...","user_id":"9f2c...","phone_tail":"8888","new_user":true}
```

`token` **无过期时间**——打印是低频操作，强制重新登录只会激怒用户。
要下线靠吊销。App 应存进 Keychain / Keystore。

**响应 401**：验证码错误或已失效（用过一次即作废）。

#### `POST /api/auth/logout` —— 登出

角色 `app`。默认只吊销当前这一把；`?all=1` 踢掉该用户的全部 token。

**响应 200**：`{"ok":1}`

#### `POST /api/account/delete` —— 注销账号

角色 `app`。**立即执行，没有冷静期。**

删除：用户记录、完整手机号、全部 session、设备绑定关系、全部作业记录与文件。

保留：按打印机序列号存的适配档案——那绑的是硬件不是人，里面没有能关联到
自然人的字段。

**响应 200**：`{"ok":1,"jobs_deleted":12}`

#### `POST /api/device/enroll` —— 为设备签发密钥

角色 `app`。配网时用。

```json
{"dev": "f412fa87c9e0", "name": "工位打印机"}
```

**响应 200**

```json
{"device_key":"7b1e...","dev":"f412fa87c9e0","reset":false}
```

`reset: true` 表示这台设备之前就绑在本账号上，**旧密钥已被吊销**——
也就是重置。App 应提示用户「已重新绑定」。

**响应 409**：`{"e":"device bound"}` —— 该设备属于其他账号，需原持有人先解绑。

#### `POST /api/device/{dev}/unbind` —— 解绑

角色 `app`。转让二手设备前用。吊销该设备的全部 device 密钥，设备变成未绑定
状态，可被其他账号 enroll。

**响应 200**：`{"ok":1}`。设备本来就没绑定时也返回 200（幂等）。

### 4.2 `GET /api/job/{id}/data` —— 设备取件

角色：`device`。这是设备侧最重要的端点。

**请求**

```
GET /api/job/7a3f91c04bd7/data HTTP/1.1
Host: mqtt.silkline.id:9443
X-Device: f412fa87c9e0
Authorization: Bearer a3f91c04bd77.kJ8xQ2mN...
```

**响应 200**

```
Content-Type: application/octet-stream
Content-Length: 284160
Accept-Ranges: bytes
```

body 是 URF 光栅数据。**流式读取，不要整包进内存**——一份作业 100~500KB，
可用堆只有 70~120KB。按 4096 字节一块读出来直接喂给 USB。

**校验**：服务端确认 `job.dev == X-Device`。不是你的作业返回 403。

**断点续传**：支持 `Range: bytes=N-`，返回 206 加 `Content-Range`。
固件当前实现不用，但服务端已支持，将来做续传不用改服务端。

**服务端副作用**：请求到达即把作业标记为 `downloading` 并把 180 秒超时
重新计时。所以只要在传，就不会被误判超时。

**错误**：

- 404 —— 作业不存在（已被清理，或已被服务端退回队列后又删除）。
  固件应当回一条 `failed` 并等下一次派发，**不要反复重试同一个 ID**。
- 403 —— 作业不属于你。不要重试。

### 4.3 `POST /api/device/{id}/ident` —— 上报全量机型档案

角色：`device`。`{id}` 必须等于 `X-Device`，否则 403。

**请求**

```
POST /api/device/f412fa87c9e0/ident HTTP/1.1
Content-Type: application/json
Content-Length: 8842
X-Device: f412fa87c9e0
Authorization: Bearer ...
```

body 是 `usb_printer_describe()` 加 PJL 探针的完整 JSON。
**上限 256KB**，超了返回 413。

**响应 200**：`{"ok":1}`

时机：打印机枚举完成 → 等 3 秒让打印机稳定 → 第 0 层 describe → 第 1 层 PJL
探针 → 本请求 → 再发 MQTT 精简身份（3.8）。

分不到缓冲时重试：最多 8 次，间隔 15 秒。

### 4.4 `POST /api/print` —— App 上传作业

角色：`app`。**固件不用这个端点**，列在这里是给 App 开发看的。

服务端**不渲染**。上传的必须已经是打印机原生格式，服务端只校验、排队、转发。

**请求**

```
POST /api/print HTTP/1.1
Content-Type: image/urf
X-Device: f412fa87c9e0
X-Printer-Serial: CNB9K1P2X4
X-Filename: UTF-8''%E6%8A%A5%E5%91%8A.pdf
Authorization: Bearer <app 角色的令牌>
```

`X-Printer-Serial` **必填**，值取自 `render-profile` 响应里的 `serial`。

这个字段是防废纸的：URF 是按某台打印机的 dpi 和像素尺寸光栅的，派给另一台
就是一沓废纸，而且服务端不解析文档、设备不认识格式，**没有任何环节会发现**。
服务端据此只在对应打印机插着时才派发。

| `Content-Type` | 格式 | 魔数 |
|---|---|---|
| `image/urf` | Apple URF | `UNIRAST\0`（8 字节） |
| `image/pwg-raster` | PWG-Raster | `RaS2`（4 字节） |

其它 Content-Type 一律 415。**PDF、PNG、JPEG、文本都不接受**——这台打印机的
`CMD:` 字段是 `URF,PCL,PJL,PWGRaster`，里面没有 PDF，送进去只会吐乱码纸。

body 是光栅数据。**上限 200MB**（URF 是光栅，整页照片单页就能到 15MB）。

`X-Filename` 用 RFC 5987 编码，仅用于显示。

**响应 200**

```json
{"job":"7a3f91c04bd7","size":1048576,"pages":2,"state":"queued","printer_attached":true}
```

`pages` 是服务端从 URF 头里读出来的页数，回给 App 做核对——**和你以为的页数
不一致就说明编码器有问题**，别等用户收到纸才发现。

入队即返回，设备在线的话信令已经发出去了。

`printer_attached: false` 表示目标打印机当前没插在这个桥上（用户换了机器）。
**作业不失败也不删**，留在队列里，等那台打印机插回来自动继续。App 应当据此
提示「已排队，插上 XXX 后开始打印」。

**响应 400 —— 校验不通过**

```json
{"e":"bad raster","detail":"魔数不匹配：期望 UNIRAST\\0，实际 %PDF-1.7"}
```

服务端校验三条，任何一条不过都不入队：

1. **魔数**与 `Content-Type` 匹配
2. **页数字段非 0**。URF 头第 9~12 字节是大端页数。写 0 的话打印机认为文档为空，
   什么都不打。见第 6 节
3. **首页页头宽高**在合理区间（0 < w,h < 30000），且与该设备的 render-profile 一致

第 3 条不一致时返回 400 并在 `detail` 里给出期望值——尺寸错了会打出错位或半页，
比直接拒绝更难排查。

### 4.5 `GET /api/device/{id}/render-profile` —— 取光栅参数

角色：`app`。**App 在光栅之前必须先拉这个**，不要硬编码。

参数来自设备自己上报的 `ident`（URF 能力串），换打印机改服务端，App 不用发版。

**响应 200**

```json
{
  "dev": "f412fa87c9e0",
  "serial": "CNB9K1P2X4",
  "format": "urf",
  "urf_caps": "CP1,IS1-4-5-19,MT1-2-3-4-5-6,RS600,V1.4,W8",
  "dpi": 600,
  "color": "gray8",
  "pages": {
    "a4":     {"w_px": 4962, "h_px": 7014},
    "letter": {"w_px": 5100, "h_px": 6600}
  },
  "margins_mm": [4, 4, 4, 4],
  "max_job_bytes": 209715200,
  "updated": 1756500000
}
```

| 字段 | 说明 |
|---|---|
| `urf_caps` | 打印机自述的 URF 能力串原文。解析不了的字段以它为准 |
| `dpi` | 从 `RS600` 解析。这台机器**只支持 600**，不能降到 300 省流量 |
| `color` | 从 `W8` 解析。`gray8` = 8 位灰度 |
| `pages` | 各纸张的像素尺寸，已按 dpi 算好 |
| `serial` | 当前插着的打印机序列号。**光栅前记下它，上传时用 `X-Printer-Serial` 带回来** |
| `margins_mm` | **实测值，不是从能力串推的**。打印机的不可打印区必须出纸才测得出（HANDOFF 第 3.6 节第 2 层）。默认 `[4,4,4,4]`，服务端可配置覆盖 |

**响应 404**：该设备从未上报过 `ident`（没插打印机，或还没枚举完）。
App 应提示「打印机未就绪」，不要用默认值蒙——尺寸蒙错就是废纸。

### 4.5b `GET /api/devices` —— 列出我名下的设备

角色：`app`。**不需要 `X-Device`**——这个端点就是用来得知有哪些 `dev` 的。

其余所有设备端点都要求调用方已经知道 `{dev}`，而 `dev` 唯一的来源是配网时从
SoftAP 读到的 MAC。没有这个端点，App 的设备列表只能存在手机本地，用户换手机或
重装 App 后账号还在、设备还绑着，但 App 再也找不到它，只能重新配网一遍。

**响应 200**

```json
{
  "devices": [
    {"dev":"f412fa87c9e0","name":"工位打印机",
     "online":true,"seen":1756500000,"state":"ready","bound":1756400000,
     "printer":{"serial":"CNB9K1P2X4","make":"HP","model":"HP Laser MFP 136a",
                "attached":true},
     "queued_jobs":0}
  ]
}
```

| 字段 | 说明 |
|---|---|
| `name` | enroll 时传的名字，用户可改 |
| `bound` | 绑定时间戳 |
| `printer` | 当前插着的打印机。没插时为 `null` |
| `queued_jobs` | 该设备名下排队中的作业总数 |

一台设备都没有时返回 `{"devices":[]}`，**不是 404**。

### 4.6 `GET /api/status` —— 查设备与作业

两种角色都可以调。只返回 `X-Device` 那一台的数据。

**响应 200**

```json
{
  "device": {
    "dev": "f412fa87c9e0",
    "online": true,
    "seen": 1756500000,
    "state": "ready",
    "prn": {"code":10001,"display":"Ready","online":true,"asleep":false,
            "paper_out":false,"error":false}
  },
  "jobs": [
    {"id":"7a3f91c04bd7","name":"报告.pdf","size":284160,
     "state":"downloading","bytes":131072,"err":"",
     "created":1756499900,"updated":1756499950}
  ]
}
```

最多返回最近 15 件作业。设备从未上线过时 `device.online` 为 `false`，
`prn` 为 `null`。

### 4.7 `GET /api/device/{dev}/printer` —— 打印机完整信息

角色：`app`。App 展示「这台打印机是什么、当前用的什么档案、档案从哪来」。

**响应 200**

```json
{
  "printer": {
    "serial": "CNB9K1P2X4",
    "vid": "03F0", "pid": "F22A",
    "make": "HP", "model": "HP Laser MFP 136a",
    "cmd": "URF,PCL,PJL,PWGRaster",
    "urf_caps": "CP1,IS1-4-5-19,MT1-2-3-4-5-6,RS600,V1.4,W8",
    "proto": 2,
    "first_seen": 1756400000, "last_seen": 1756500000
  },
  "profile": {
    "uel_job_end": true, "uel_wake": false, "wake_delay_ms": 0,
    "iface_cycle": false, "unidir": false,
    "margins_mm": [4, 4, 4, 4],
    "src": "model", "votes": 3, "disputed": false
  }
}
```

`profile.src` 告诉用户这份配置的可信度：

| src | 含义 | App 该怎么说 |
|---|---|---|
| `serial` | 这台机器自己测过 | 「已针对你的打印机校准」 |
| `model` | 同型号 ≥3 台一致 | 「已验证的机型配置」 |
| `authoritative` | 开发者标记 | 「官方配置」 |
| `quirks` | CUPS usb-quirks 表 | 「通用配置，建议做一次测试」 |
| `default` | 全保守默认值 | 「未适配，建议做一次测试」 |

`disputed: true` 时同型号出现过相反结论，已回退到下一层。App 应提示用户测一次。

**响应 404**：设备从未上报过 `ident`（没插打印机，或还没枚举完）。

### 4.7b `GET /api/device/{dev}/printers` —— 这个桥见过的所有打印机

角色：`app`。用户换过几台打印机时，App 用它列出历史并显示各自的适配状态。

**响应 200**

```json
{
  "attached": "CNB9K1P2X4",
  "printers": [
    {"serial":"CNB9K1P2X4","make":"HP","model":"HP Laser MFP 136a",
     "attached":true,"profile_src":"serial","queued_jobs":0,
     "first_seen":1756400000,"last_seen":1756500000},
    {"serial":"VNC7J2Q9Y1","make":"Brother","model":"HL-L2350DW",
     "attached":false,"profile_src":"quirks","queued_jobs":2,
     "first_seen":1756100000,"last_seen":1756300000}
  ]
}
```

`attached` 是当前插着的那台的序列号，没插时为空串。

`queued_jobs` 是**为那台打印机排着、但它现在没插着**的作业数。
App 应当提示「插上 Brother HL-L2350DW 后会自动打印 2 份」——
这些作业不会失败也不会丢，但用户不知道就会以为打印失败了。

### 4.8 `GET /api/device/{dev}/tests` —— 测试清单

角色：`app`。

**响应 200**

```json
{
  "tests": [
    {"id":"double_print","name":"连打两份","required":true,
     "jobs_needed":2,"state":"untested","affects":["uel_job_end"]},
    {"id":"wake","name":"休眠唤醒","required":false,
     "jobs_needed":1,"state":"passed","affects":["uel_wake","wake_delay_ms"]},
    {"id":"margins","name":"页边距","required":false,
     "jobs_needed":1,"state":"untested","affects":["margins_mm"]}
  ]
}
```

`state`：`untested` / `running` / `passed` / `failed` / `unclear`。

`double_print` 的 `required` 是 `true`——**单份作业成功不能说明任何事**，
不发作业结束符时症状是「第二份不出」，只有连打才测得出来。

### 4.9 `POST /api/device/{dev}/tests/{test}/start` —— 开始一轮测试

角色：`app`。

**响应 200**

```json
{
  "run_id": "3f9a1c04bd77",
  "jobs_needed": 2,
  "variant": {"uel_job_end": false},
  "baseline_ok": true,
  "instruction": "接下来会打印 2 页。请把打印机纸盒装好，然后看实际出了几张纸。"
}
```

拿到 `run_id` 后，App 生成对应份数的测试页，逐份 `POST /api/print`，
**每份都带 `X-Test-Run: 3f9a1c04bd77`**。服务端据此按 `variant` 下发一次性
quirk 覆盖。

`baseline_ok: false` 表示当前生效配置本身就打不出来。此时**不要继续测变体**——
在一个本来就坏的链路上测，得到的全是噪音。响应会带 `hint` 说明先修什么。

**响应 409**：该设备已有一轮测试在进行中。

### 4.10 `POST /api/tests/{run_id}/answer` —— 提交用户判断

角色：`app`。用户看完纸后回答。

**请求**

```json
{"verdict":"fail","detail":{"pages_printed":1}}
```

| test | `detail` 字段 |
|---|---|
| `double_print` | `{"pages_printed": 1\|2}` |
| `wake` | `{"printed": true\|false, "delay_s": 12}` |
| `iface_cycle` | `{"pages_printed": 0..5}` |
| `margins` | `{"cut_mm": [上, 右, 下, 左]}` |

`verdict` 取 `pass` / `fail` / `unclear` / `aborted`。用户不确定时**必须让他选
`unclear`**，不要逼他二选一——猜出来的数据比没有数据更糟。

**响应 200**

```json
{
  "applied": {"uel_job_end": true},
  "scope": "serial",
  "votes": 1,
  "promoted": false,
  "message": "已针对你的打印机保存。再有 2 台同型号得出相同结论后，将成为该机型的默认配置。"
}
```

结论写入后服务端立即通过 `printer/{dev}/profile` 重新下发，无需重启设备。

---

## 5. 完整时序

### 5.1 正常打印一份

```
App                        服务端                 设备
 | GET /render-profile      |                      |
 |------------------------->|                      |
 |<-- {dpi,尺寸,色彩} -------|                      |
 | 本地光栅成 URF            |                      |
 |                          |                      |
 | POST /api/print (URF)    |                      |
 |------------------------->| 校验魔数/页数/尺寸     |
 |                          | 落盘、入库 queued     |
 |<-- {job,size,queued} ----|                      |
 |                          | publish printer/…/job|
 |                          |--------------------->|
 |                          |                      | GET /job/{id}/data
 |                          |<---------------------|
 |                          | → downloading        | 流式喂 USB
 |                          |<-- status: printing -|
 |                          |<-- status: done -----|
 |                          | → done，派下一件       |
```

服务端全程没有解析过文档内容，只看了头部几十个字节。

### 5.0 首次配网

```
App                          服务端                    设备(SoftAP)
 | POST /api/auth/sms         |                          |
 |--------------------------->|  发短信                   |
 | POST /api/auth/verify      |                          |
 |--------------------------->|                          |
 |<-- session token ----------|                          |
 |                            |                          |
 | 连上设备的 SoftAP，读到 MAC   |                          |
 | POST /api/device/enroll    |                          |
 |--------------------------->|  签发 device 密钥，绑到本用户 |
 |<-- device_key -------------|                          |
 |                            |                          |
 | 把 Wi-Fi 凭据 + device_key 写进设备                      |
 |------------------------------------------------------>|
 |                            |          设备重启，连 MQTT  |
 |                            |<-------------------------|
```

**用户一个字符都不用输。**

### 5.1b 插上打印机到完成适配

```
设备                    服务端                        App
 | 枚举完成              |                              |
 | 第 0/1 层探测          |                              |
 | POST /device/…/ident  |                              |
 |---------------------->| upsert printers              |
 |                       | 查 profile（分层）            |
 |<-- printer/…/profile -| retain=1                     |
 | 存 NVS，立即生效       |                              |
 |                       |<-- GET /device/…/printer ----|
 |                       |--- 机型信息 + 档案 + 来源 ---->|
 |                       |<-- POST …/tests/double_print/start
 |                       |--- {run_id, jobs_needed:2} ->|
 |                       |<-- POST /api/print ×2 -------|  (X-Test-Run)
 |<-- job (quirks 覆盖) --|                              |
 | 按变体打两份            |                              |
 |                       |            用户看纸           |
 |                       |<-- POST /tests/{id}/answer --|
 |                       | 判定 → 写 serial 级档案        |
 |<-- printer/…/profile -| 重新下发                      |
```

### 5.2 设备离线期间投递

作业照常入库为 `queued`，**不丢件**。设备重连后发第一条 `ready` 心跳，
服务端立即派发。固件侧不需要任何特殊处理。

### 5.2b 用户换了一台打印机

```
设备                       服务端                      App
 | 拔掉打印机                |                            |
 | status: serial=""        |                            |
 |------------------------->| A 机的作业停止派发           |
 | 插上另一台，枚举           |                            |
 | 第 0/1 层探测             |                            |
 | POST /device/…/ident     |                            |
 |------------------------->| upsert printers            |
 |                          | 按新 serial 分层查 profile   |
 |<-- printer/…/profile ----| 带新 serial                 |
 | 存 NVS，立即生效           |                            |
 | status: serial=B         |                            |
 |------------------------->| actor.currentSerial = B     |
 |                          | 只派 serial=B 的作业         |
```

**A 机的作业留在队列里**，不失败也不删——用户很可能只是临时换台机器打点别的，
插回来就接着打。这是「离线不丢件」的自然延伸，只是「不在线」的粒度从桥
细化到了打印机。

App 侧用 `GET /api/device/{dev}/printers` 的 `queued_jobs` 告诉用户
「插上 XXX 后会自动打印 N 份」，否则用户会以为打印失败了。

### 5.3 传输中断

设备掉线或卡住，服务端 180 秒无进展就把作业退回 `queued`，
设备恢复后会**从头重传**（不是续传）。固件收到同一个 `job.id` 第二次是正常的，
应当重新走一遍取件流程。

---

## 6. App 必须遵守的规则（光栅侧）

服务端不渲染，所以下面每一条错了都会变成废纸，没有中间环节能拦住。

1. **先拉 `render-profile`，不要硬编码尺寸和 dpi。** 换打印机时服务端会改这个
   端点的返回值，硬编码的 App 要发版才能跟上。
2. **页数字段必须填真实页数。** URF 头第 9~12 字节，大端 32 位。填 0 打印机认为
   文档为空，什么都不打。参考实现见 `tools/reference/render.py` 的 `fix_page_count`
   ——那段代码就是为这个坑写的，移植它，不要重新发现它。
3. **这台机器只支持 600dpi**（能力串 `RS600`），不能降到 300 省流量。
4. **色彩是 8 位灰度**（`W8`）。传彩色数据打印机不认。
5. **算好流量再传**。600dpi 8bpp A4 单页未压缩 34MB，RLE 后纯文字页
   200KB~1MB、整页照片 5~15MB。移动网络下要给用户进度和取消。
6. **设备没上报过 ident 时 `render-profile` 返回 404**，此时提示「打印机未就绪」，
   **不要用默认值蒙**——尺寸蒙错就是废纸。

## 6b. 固件必须遵守的规则

1. **文档只走 HTTPS，永远不走 MQTT。** 作业现在是光栅，200KB~15MB，
   而可用堆只有 70~120KB。MQTT 消息必须整包进内存，差了两个数量级。
2. **取件流式处理**，4096 字节一块，不缓冲整份。
3. **`done` / `failed` 必须带 `job` 字段。** 不带的话服务端要等 180 秒超时。
4. **心跳 30 秒一拍，状态变化 2 秒内推。** 判活是离线队列的地基。
5. **认证失败不重连。** CONNACK `0x05` 说明密钥错，重试没有意义。
6. **只碰自己的 topic。** ACL 会拒，且留日志。
7. **密钥当不透明字符串。** 不解析、不截断、不打进日志。
8. **`profile` 的 `serial` 与当前打印机不符时忽略这份档案。** retain 的旧档案
   会在换打印机后先到，套上去就是错的。
9. **作业信令里的 `hooks` 只对本次作业生效，绝不写 NVS。** 作业结束即销毁，
   失败也要销毁——残留会把设备留在坏状态，用户测一次就打不了印。
9b. **profile 越界就整份拒绝，不要截断。** 半份 profile 比没有 profile 更危险。
9c. **未知原语：可选的跳过并上报，标了 `required` 的拒绝整份。** 静默跳过一个
    关键的 `send_hex` 会让打印机行为错乱，而症状最难归因回这一步。
10. **心跳必须带当前打印机的 `serial`，拔掉后立刻变成空串。** 服务端靠它决定
    派哪些作业。报得不准 = 把为 A 机光栅的作业打到 B 机上，出一沓废纸。

---

## 7. 自测

服务端起来后不用等固件，也不用等 App，先用命令行把全链路跑通。

设两个变量，后面都用：

```bash
HOST=mqtt.silkline.id
DEV=f412fa87c9e0
```

**第一步：登录拿 session token。** 开发环境没配短信服务商时，验证码只打日志，
从 `journalctl -u airprintd` 里取。

```bash
curl -s -X POST https://$HOST:9443/api/auth/sms -H 'Content-Type: application/json' -d '{"phone":"13800008888"}'
```

```bash
curl -s -X POST https://$HOST:9443/api/auth/verify -H 'Content-Type: application/json' -d '{"phone":"13800008888","code":"从日志里抄的 6 位","device":"curl"}'
```

把返回的 `token` 存成 `APPTOK`。

**第二步：enroll 一台设备，拿 device 密钥。**

```bash
curl -s -X POST https://$HOST:9443/api/device/enroll -H "Authorization: Bearer $APPTOK" -H 'Content-Type: application/json' -d "{\"dev\":\"$DEV\",\"name\":\"工位打印机\"}"
```

把返回的 `device_key` 存成 `DEVKEY`。

**第三步：模拟设备订阅作业信令。** 另开一个终端挂着：

```bash
mosquitto_sub -h $HOST -p 8883 --capath /etc/ssl/certs -q 1 -u $DEV -P "$DEVKEY" -t "printer/$DEV/job" -t "printer/$DEV/profile" -v
```

**第四步：发一条带 serial 的心跳。** 没有这一步服务端不知道插的是哪台打印机，
不会派任何活。

```bash
mosquitto_pub -h $HOST -p 8883 --capath /etc/ssl/certs -q 1 -u $DEV -P "$DEVKEY" -t "printer/$DEV/status" -m "{\"dev\":\"$DEV\",\"job\":\"\",\"state\":\"ready\",\"bytes\":0,\"serial\":\"TESTPRINTER1\"}"
```

**第五步：造一份最小 URF 并上传。** 8 字节魔数 + 大端页数 + 32 字节页头：

```bash
python3 -c "import struct,sys; h=b'UNIRAST\x00'+struct.pack('>I',1)+b'\x00'*12+struct.pack('>II',4962,7014)+b'\x00'*12; sys.stdout.buffer.write(h+b'\x00'*512)" > /tmp/test.urf
```

```bash
curl -s -X POST https://$HOST:9443/api/print -H "Authorization: Bearer $APPTOK" -H "X-Device: $DEV" -H 'X-Printer-Serial: TESTPRINTER1' -H 'Content-Type: image/urf' --data-binary @/tmp/test.urf
```

第三步的终端里应当立刻出现一条 `printer/{dev}/job` 信令。

**第六步：取件。**

```bash
curl -f -o /tmp/got.urf https://$HOST:9443/api/job/替换成上一步返回的job/data -H "Authorization: Bearer $DEVKEY"
```

**第七步：回执。** 发完这条，作业状态应变成 `done`。

```bash
mosquitto_pub -h $HOST -p 8883 --capath /etc/ssl/certs -q 1 -u $DEV -P "$DEVKEY" -t "printer/$DEV/status" -m "{\"dev\":\"$DEV\",\"job\":\"替换成job\",\"state\":\"done\",\"bytes\":552,\"serial\":\"TESTPRINTER1\"}"
```

### 三条必须为「失败」的检查

这三条对应本次重写补的三个洞。**它们没有用户可见的症状，坏了也不会有人报障**，
所以要主动验。

```bash
# 1. 错误密钥必须连不上（退出码非 0 才对）
mosquitto_pub -h $HOST -p 8883 --capath /etc/ssl/certs -u $DEV -P 'bogus.key' -t "printer/$DEV/status" -m '{}'; echo "退出码=$?"
```

```bash
# 2. ACL：不能碰别人的 topic（退出码非 0 才对）
mosquitto_pub -h $HOST -p 8883 --capath /etc/ssl/certs -u $DEV -P "$DEVKEY" -t 'printer/aaaaaaaaaaaa/status' -m '{}'; echo "退出码=$?"
```

```bash
# 3. 入口校验：PDF 冒充 URF 必须 400（否则用户会收到几十张乱码纸）
curl -s -o /dev/null -w '%{http_code}\n' -X POST https://$HOST:9443/api/print -H "Authorization: Bearer $APPTOK" -H "X-Device: $DEV" -H 'X-Printer-Serial: TESTPRINTER1' -H 'Content-Type: image/urf' --data-binary '%PDF-1.7 fake'
```

---

## 8. 相对 v1 的差异（固件改动清单）

| # | 项 | v1（`jobsrv.py`） | v2（`airprintd`） | 固件改动 |
|---|---|---|---|---|
| 1 | MQTT 认证 | 全网共用 `MQTT_USER`/`MQTT_PASS` 常量 | 每设备一密钥，username=`{dev}` | `cloud_creds.h` 的两个常量 → 从 NVS 读 `cloud/devkey` |
| 1b | 密钥怎么进设备 | 服务器上签发，用户**手抄** 45 个字符 | App 登录后 enroll，连同 Wi-Fi 凭据一起写进设备 | 配网协议要能接收 `devkey` 字段 |
| 2 | HTTP 鉴权 | **无** | `X-Device` + `Authorization: Bearer` | 取件和 ident POST 各加两个头 |
| 3 | 配网 | 只填 Wi-Fi | 多一个「设备密钥」 | `provision.c` / `portal_html.h` 加输入框，存 NVS |
| 4 | 作业状态 | `queued→downloading→done/failed` | **不变** | 无 |
| 5 | 取件 404 | 作业不存在 | 同左（作业已被清理） | 404 时回一条 `failed`，不要反复重试 |
| 5b | 渲染 | 服务端跑 `cupsfilter` | **服务端不渲染**，App 上传已光栅的 URF | 无——设备本来就不认识格式 |
| 5c | 作业体积 | 100~500KB | **200KB~15MB** | 无（本来就是流式），但进度显示要改量纲 |
| 5d | USB 层怪癖 | 编译进 `printer_profile.c`，按 VID/PID 查表 | 服务端下发的动作序列，存 NVS 立即生效 | 写一个小解释器（四个原语、三个钩子）；内置表降级为兜底 |
| 6 | 失败原因 | 丢弃 | 存进作业 `err` 列 | `failed` 时可带 `err` 字段（≤200 字节） |
| 7 | topic | 同左 | **不变** | 无 |
| 8 | job / status payload | 同左 | **不变**（仅新增可选 `err`） | 无 |
| 9 | 端口 | 8883 / 9443 | **不变** | 无 |

7~9 是刻意保持的：协议主体不动，改动集中在认证。**固件可以先做第 1、2、3 项，
其余照旧就能跑通。**

第 5b 条对固件是**零改动**——设备一直就是根管子，不认识文档格式（HANDOFF 第 1 节）。
变的只是管子里流的东西从「服务端渲染出的 URF」换成「手机渲染出的 URF」。

---

## 9. 尚未定义的

诚实标注，别当成已有能力：

- **密钥轮换**。目前只能 `revoke` 旧的、`add` 新的，设备要重新配网。
- **同时接多台打印机**。协议支持一个桥**先后**插过多台（按序列号区分，各自
  记住档案，插回来即认），但**同时**挂两台以上不支持——需要 USB Hub 支持和
  设备侧多路驱动，ESP32 的堆撑不住。
- **仲裁界面**。`disputed` 的机型档案只能直接改库，没有管理页。
- **更换手机号**。用新号登录会创建新账号，老账号的设备和历史不跟过去。
- **国际号码**。只处理中国大陆号码。
- **多人共享一台打印机**。一台设备只能绑一个账号。
- **二手转让的自助流程**。原持有人不解绑就只能走申诉，由运维强制解绑。
- **断点续传**。服务端支持 `Range`，固件未实现，超时是从头重传。
- **作业取消**。App 无法撤回已派发的作业。
- **服务端渲染**。已明确不做，见 spec 第 11 节。App 是唯一的光栅方。
- **profile 里的条件分支与循环**。四个原语是线性执行的，没有 if 也没有 while。
  已知的怪癖都不需要——真需要时再说，不预先造解释器。
- **PCL 上传**。打印机认 PCL，但服务端目前只放行 URF 和 PWG-Raster
  （魔数可校验）。PCL 没有可靠的魔数，放行等于放弃入口校验。
