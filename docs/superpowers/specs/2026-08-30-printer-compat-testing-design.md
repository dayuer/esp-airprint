# 打印机适配与兼容性库 — 设计

日期：2026-08-30
状态：已评审，待实现
依赖：`2026-08-30-go-print-server-design.md`（服务端重写）先落地

让用户参与完成「必须出纸才测得出来」的那层适配，并把结论沉淀成可复用的
兼容性库。

---

## 1. 为什么要做

`docs/HANDOFF-cloud-print.md` 第 3.6 节把机型探测分了三层：

| 层 | 内容 | 现状 |
|---|---|---|
| 第 0 层 | 描述符、IEEE-1284 device-id、端口状态 | 设备自己能做，零风险 |
| 第 1 层 | PJL 探针 | 设备自己能做，需第 0 层授权 |
| **第 2 层** | **UEL 要不要发、唤醒等多久、页边距对不对** | **无法自动化** |

第 2 层是本项目吃过最多苦头的地方，而且**这类知识不存在于任何公开数据库**——
CUPS 的 usb-quirks 表 96 条里一条都没有，因为在 Linux 上这些问题根本不发生
（过滤链自己吐 UEL、内存管够、USB 栈完整）。

所以第 2 层只能靠人看着纸回答。这件事做成产品功能，副产品就是一张别人没有的表。

**尤其 UEL**：症状是「第二份不出」，所以单份作业成功不能说明任何事，
必须连打两份。这条是用血写的，测试设计必须遵守。

---

## 2. 前置改动：profile 改为服务端下发的动作序列

不做这一步，整个功能产不出价值——用户测完得等下一版固件才能用上自己的结果。

现状：`main/printer_profile.c` 编译进固件，`profile_lookup()` 按 VID/PID 查表，
怪癖是几个写死的布尔字段。

### 为什么不是「下发一张参数表」

参数表（`uel_job_end` / `wake_delay_ms` / `iface_cycle` …）能表达的只有**固件
已经实现的那几个开关**。遇到新机型的新怪癖——某台机器要求作业前先发一段 PJL
语言切换、或者作业后要读一次状态才肯出纸——参数表描述不了，还是得发固件。

而这类怪癖恰恰最可能冒出来：现有这几条（UEL、唤醒延时、接口复位）本身就是一条条
撞出来的，没有理由认为已经撞完了。

### profile 是一份可编排的动作序列

固件提供一组原语，profile 用文本编排它们：

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

那 9 个字节的 UEL（`1b252d313233343558`）不再是固件里的常量，而是 profile 里的
一串十六进制。

### 原语白名单

| op | 参数 | 行为 |
|---|---|---|
| `send_hex` | `data`（十六进制串） | 往 bulk OUT 写这串字节 |
| `delay_ms` | `ms` | 等待 |
| `iface_reset` | — | 复位 USB 接口 |
| `read_status` | — | 读一次 IN 端点，结果并入下次心跳 |

`flags` 仍是布尔，因为它们不是「动作」而是「模式」：

- `unidir` —— 停用 IN 端点、不发 PJL
- `pjl_ok` —— 允许第 1 层 PJL 探针（由第 0 层的 `CMD:` 授权）

### 三个钩子

| hook | 何时执行 |
|---|---|
| `job_begin` | 作业开始前 |
| `job_end` | 作业结束后 |
| `wake` | 从休眠唤醒时 |

**`job_end` 里不要放接口复位。** 曾经在这里做过，结果稳定在距流尾几 KB 处
`Decoding Fail`——最后一个短包还没物理冲出去就被 halt/flush 掉了。
端点复位只在下一份作业开始时做（`job_begin`），这是实测结论，不是偏好。

### 硬上限（越界拒绝整份，不截断）

设备可用堆只有 70~120KB，profile 必须有明确的天花板：

| 项 | 上限 |
|---|---|
| profile JSON 总长 | 1024 字节 |
| 每个 hook 的步数 | 8 |
| 单个 `send_hex` 的字节数 | 64 |
| 单个 `delay_ms` | 5000 |
| 单个 hook 内 delay 总和 | 10000 |

**越界就整份拒绝并退回内置兜底**，不做截断——半份 profile 比没有 profile 更危险。
服务端在下发路径上也要做同样的校验，不能让畸形 JSON 传到设备上才发现。

### 前向兼容：未知原语怎么办

这是这个设计最关键的一点，决定了服务端能不能先行支持新机型。

- **可选步骤**（默认）：固件不认识就跳过，并在心跳里报一句
  `"skipped_ops":["new_thing"]`
- **必需步骤**（`"required": true`）：固件不认识就**拒绝整份 profile**、
  退回内置兜底，并上报原因

区分二者是必要的。静默跳过一个关键的 `send_hex` 会让打印机行为错乱，
而错乱的症状（第二份不出、乱码纸）恰恰最难归因到「固件跳过了一步」。
所以：可跳过的步骤显式声明为可跳过，其余一律按必需处理。

有了这条，**加一种新怪癖不需要全网 OTA**：服务端先下发，新固件先生效，
老固件安全地退回兜底并报告，然后按自己的节奏跟上。

### 查找顺序（分层，先命中先用）

| 优先级 | 来源 | 条件 |
|---|---|---|
| 1 | 本机实测 | 按**打印机序列号**匹配，该机自己测过 |
| 2 | 同型号已验证 | 同 VID/PID/model，**≥3 台独立设备结论一致** |
| 3 | 开发者权威档案 | 人工审核标记 |
| 4 | CUPS usb-quirks 表 | `main/usb_quirks_db.h` 的 96 条，由服务端翻译成动作序列 |
| 5 | 默认值 | 保守取值：发 UEL、不做接口复位、双向 |

开发者可以把某份档案 **pin** 为强制，pin 后压过第 1、2 层。这是给「众包数据明显
错了」准备的逃生门，正常不用。

序列号是主键不是 MAC——`usb_device_info_t.str_desc_serial_num` 每台打印机唯一，
设备 MAC 标识的是桥。区分「同一个桥换了打印机」和「同一台打印机换了桥」只能靠它
（HANDOFF 3.6）。

`printer_profile.c` 从「真相」降级为**连不上服务端时的内置兜底**。

## 3. 数据模型

```sql
-- 见过的打印机，按序列号唯一
CREATE TABLE printers(
  serial   TEXT PRIMARY KEY,
  vid TEXT, pid TEXT, make TEXT, model TEXT,
  cmd      TEXT,          -- IEEE-1284 CMD 字段原文
  urf_caps TEXT,          -- URF 能力串，render-profile 的来源
  last_dev TEXT,          -- 最近挂在哪个桥上
  first_seen INT, last_seen INT
);

-- 生效档案。scope 决定匹配方式
CREATE TABLE profiles(
  id INTEGER PRIMARY KEY,
  scope   TEXT,   -- 'serial' | 'model' | 'authoritative'
  match   TEXT,   -- scope=serial 时是序列号；否则 "vid:pid:model"
  body    TEXT,   -- 下发给设备的 JSON 原文（flags + hooks），≤1024 字节
  margins_mm TEXT,        -- "[上,右,下,左]"，只给 App 用，不下发给设备
  votes    INT DEFAULT 0, -- 支持该结论的独立设备数
  disputed INT DEFAULT 0,
  pinned   INT DEFAULT 0,
  updated  INT
);

-- 每一次用户参与的测试
CREATE TABLE test_runs(
  id      TEXT PRIMARY KEY,
  dev     TEXT,   -- 哪个桥
  serial  TEXT,   -- 哪台打印机
  test    TEXT,   -- 'double_print' | 'wake' | 'iface_cycle' | 'bidir' | 'margins'
  variant TEXT,   -- 本次用的参数 JSON
  verdict TEXT,   -- 'pass' | 'fail' | 'unclear' | 'aborted'
  detail  TEXT,   -- 用户填的补充（页边距毫米数等）
  created INT, answered INT
);
```

---

## 4. 测试项

| test | 问用户什么 | 定 profile 的哪一处 | 要打几份 |
|---|---|---|---|
| `double_print` | 「应该出 2 张，实际出了几张？」 | `hooks.job_end` 要不要那条 `send_hex` | 2 |
| `wake` | 「待机后这份出来了吗？多久开始动？」 | `hooks.wake` 的内容与 `delay_ms` | 1（前置等待） |
| `iface_cycle` | 「5 份里有没有卡住的？」 | `hooks.job_begin` 要不要 `iface_reset` | 5 |
| `bidir` | 无需用户看纸，读回包即可 | `flags.unidir` | 0 |
| `margins` | 「标尺页四边各被切掉几毫米？」 | `margins_mm`（不下发给设备） | 1 |

`double_print` 是必测项，其余可选。未测过的部分一律取上层查找结果。

### 测试页从哪来

**App 生成，走正常上传通道。** 服务端不渲染（见服务端 spec 第 7 节），
不可能凭空造出测试页；而 App 为了打印本来就必须实现 URF 编码器，
画一张测试页是顺手的事。

`margins` 测试**只能**由 App 生成——标尺页要按该机的 render-profile 尺寸画刻度，
换台打印机刻度位置就不一样。

上传时带 `X-Test-Run: <run_id>`，服务端据此把作业和测试关联，并按 variant
下发一次性 quirk 覆盖。

### 一次性 hook 覆盖

作业信令增加可选字段，直接覆盖本次作业用的钩子：

```json
{"id":"7a3f91c04bd7","size":81920,"hooks":{"job_end":[]}}
```

上面这一条就是「本次不发作业结束符」——空数组，不是布尔开关。测试变体因此和
profile 用的是同一套模型，不需要第二套表达方式。

**只影响本次作业，绝不写 NVS。** 这条很重要：测试失败不能把设备留在坏状态里，
否则用户测一次就打不了印了。作业结束即销毁这份覆盖，**失败也要销毁**。

只覆盖出现在 `hooks` 里的那些钩子，没提到的仍走 NVS 档案。

### `double_print` 不破坏「一次只派一件」

测试要连打两份，但**不是一次派两件**。App 连续上传两份，服务端照常一件一件派——
第一件 `done` 后立即派第二件，这本来就是正常行为。

判据是「第二份**自动**出来了吗，有没有需要人去按取消键」。所以规则不用破例，
真正在测的是设备侧的作业收尾，不是服务端的派发。

---

## 5. 流程

```
用户插上打印机
   ↓
设备第 0/1 层探测 → POST /api/device/{dev}/ident
   ↓
服务端 upsert printers 表 → 查找 profile → 下发给设备
   ↓
App: GET /api/device/{dev}/printer   看到完整打印机信息 + 当前档案 + 来源层级
   ↓
App: GET /api/device/{dev}/tests     看到哪些测过、哪些没测、结论从哪来
   ↓
用户点「测试连打」
   ↓
App: POST /api/device/{dev}/tests/double_print/start  → {run_id, jobs_needed: 2}
   ↓
App 生成 2 份测试页 → POST /api/print × 2（带 X-Test-Run）
   ↓
服务端按 variant 下发（第一轮 variant 是 uel_job_end=false）
   ↓
用户看纸 → App: POST /api/tests/{run_id}/answer  {"pages_printed": 1}
   ↓
服务端判定：只出 1 张 → 该机需要 UEL → 写 serial 级 profile → 重新下发
   ↓
票数够（≥3 台独立设备同结论）→ 提升为 model 级，新用户开箱即用
```

### 基线先行

每个测试**先跑一遍当前生效配置**作为基线。基线就不过，说明问题不在被测字段上，
直接中止并提示——否则用户会在一个本来就坏的链路上反复测变体，得出的结论全是噪音。

---

## 6. 防污染

用户会看错纸、会随手乱点，这是必然的，设计要假设它发生。

- **一台设备一票**。同一 `(dev, serial, test)` 重复提交只算最新一次。
- **票数门槛 3**，且必须是 **3 个不同的 dev 且 3 个不同的 serial**——
  同一个人拿一台机器换三个桥测，不算三票。
- **分歧即冻结**。同型号出现相反结论时标记 `disputed=1`，
  **不自动采用**，回退到下一层查找结果，等人工仲裁。
- **只有持有该设备 app 密钥的人能提交**。沿用现有鉴权，不另开一套。
- 保守方向优先：判不准时取「更安全」的那个值（发 UEL、不做接口复位），
  代价是多发 9 个字节，收益是不会出现「打不出来」。

---

## 7. API

全部沿用 `X-Device` + `Bearer` 鉴权，角色 `app`。详细定义写进
`docs/API-cloud-print.md`，这里只列清单：

| 端点 | 说明 |
|---|---|
| `GET /api/device/{dev}/printer` | 打印机完整信息 + 当前生效档案 + 来源层级 |
| `GET /api/device/{dev}/tests` | 测试清单与各自状态 |
| `POST /api/device/{dev}/tests/{test}/start` | 开始一轮，返回 `run_id` 和要上传几份 |
| `POST /api/tests/{run_id}/answer` | 提交用户判断 |
| `POST /api/print` | 增加可选头 `X-Test-Run` |

设备侧新增一个下行 topic：

| Topic | 方向 | retain | 说明 |
|---|---|---|---|
| `printer/{dev}/profile` | 服务端 → 设备 | 1 | 生效档案，设备存 NVS |

retain=1 是刻意的：设备重连后不用额外请求就能拿到最新档案。
载荷几十字节，不违反「信令只传几十字节」。

---

## 8. 固件改动

1. 订阅 `printer/{dev}/profile`，收到后**先校验再存**（长度、步数、原语白名单、
   hex 合法性），存 NVS（namespace `cloud`，key `profile`）后立即生效
2. 写一个小解释器（约 150~200 行）：按顺序执行 hook 里的步骤。
   profile ≤1KB，解析进固定大小的结构体，不动态分配
3. `profile_lookup()` 改为：NVS 档案优先 → 内置 `printer_profile.c` 兜底 →
   `usb_quirks_db.h`
4. 作业信令解析可选 `hooks` 字段，**只对本次作业生效**
5. 未知原语：可选的跳过并在心跳里报 `skipped_ops`；标了 `required` 的
   拒绝整份 profile 并上报原因
6. `bidir` 测试要能上报 PJL 回包成功与否（探针已有，只需在 status 里加个字段）

第 4 项要小心：一次性覆盖不能污染 NVS，也不能在作业失败时残留。
建议做成作业结构体里的一个字段，作业结束即销毁。

第 2 项的解释器是这次固件改动的主体，但它很小：四个原语、三个钩子、
一个 for 循环。复杂度全在校验上，不在执行上。

---

## 9. 被否决的方案

| 方案 | 否决原因 |
|---|---|
| 服务端生成测试页 | 服务端不渲染。且 `margins` 标尺页必须按机型尺寸画，服务端没有光栅器 |
| 固件内置测试页 | 占 flash，且尺寸写死，换纸张/机型就不对 |
| 一次派两件来测连打 | 破坏「一次只派一件」这条设备侧硬约束。连续两件本来就能测出同样的东西 |
| 单台结论即全网生效 | 一个用户看错纸就能让同型号所有人打不出东西 |
| 测试变体直接写 NVS | 测试失败会把设备留在坏状态，用户测一次就废了 |
| profile 用固定字段参数表 | 只能表达固件已实现的那几个开关。第六条怪癖冒出来时又要全网 OTA，而这类怪癖是一条条撞出来的，没理由认为已经撞完 |
| profile 里放通用脚本/字节码 | 表达力过剩。四个原语覆盖了所有已知怪癖，而一个真正的解释器要背沙箱、超时、死循环防护——在 70KB 堆上不值得 |
| 未知原语一律静默跳过 | 跳过一个关键的 send_hex 会让打印机行为错乱，而症状（第二份不出、乱码纸）最难归因到「固件跳过了一步」。所以可跳过的必须显式声明，其余按必需处理 |
| 越界时截断 profile | 半份 profile 比没有 profile 更危险。越界一律整份拒绝，退回内置兜底 |
| 在 `job_end` 里做接口复位 | 实测会在距流尾几 KB 处 Decoding Fail——最后一个短包还没物理冲出去就被 halt/flush 掉了 |
| 用 MAC 做打印机主键 | MAC 标识的是桥。换打印机、换桥两种情况分不开（HANDOFF 3.6） |

---

## 10. 尚未定义的

- **仲裁界面**。`disputed` 的档案目前只能直接改库，没有管理页。
- **纸张尺寸**。测试和 render-profile 都假设 A4/Letter 二选一，
  自定义尺寸不在范围内。
- **同时接多台打印机**。先后换机是支持的（按序列号区分，各自记住档案），
  同时挂两台以上不支持——需要 USB Hub 和设备侧多路驱动，ESP32 的堆撑不住。
