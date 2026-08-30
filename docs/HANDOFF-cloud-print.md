# 云打印桥 — 移交文档

把一台只能插 USB 的打印机（HP Laser MFP 136a）变成可以从任何地方投递的网络打印机。
ESP32-S3-USB-OTG 板子插在打印机 USB 口上，通过 MQTT 长连接挂在云服务器上。

本文只写**结论和已验证的事实**。走过的弯路写在「踩过的坑」一节，是为了让接手的人
不要再走一遍，不是流水账。

---

## 1. 架构

```
浏览器/App ──HTTPS──> 云服务器 ────MQTT/TLS(信令)────> ESP32 ──USB──> 打印机
                    (渲染成 URF)  ←──MQTT/TLS(状态)──┘
                        └────────HTTPS(文档流式)──────>
```

职责划分是这套设计的全部要点：

| 部件 | 干什么 | 明确不干什么 |
|---|---|---|
| 云服务器 | 收文件、渲染成打印机原生格式、排队、派发 | — |
| MQTT | 只传信令（几十字节的「有活了」「我好了」） | **不传文档** |
| HTTPS | 传文档，设备流式拉取 | — |
| ESP32 | 一根管子：把字节搬进 USB，把打印机状态搬出来 | **不认识文档格式**、不跑 IPP、不跑 mDNS |

**为什么文档不走 MQTT**：MQTT 消息必须整包进内存才能交给应用，而设备可用堆只有
70~120KB，一份作业 100~500KB，塞不下。HTTPS 流式拉取自带 TCP 反压，设备内存恒定。

**为什么 ESP32 不认识格式**：格式差异收敛在服务端。渲染由 `bin/render.py` 调
`cupsfilter -P <PPD> -m image/urf` 完成，**机型知识存在那份 CUPS PPD 里**
（当前是 `/opt/stickbox/ppd/hp136a.ppd`），不在代码里。换机型 = 换 PPD。

别和固件里的 `main/printer_profile.c` 搞混，两者管的不是一回事：

| | 在哪 | 管什么 | 换机型要改吗 |
|---|---|---|---|
| CUPS PPD | 服务端 `/opt/stickbox/ppd/` | 文档怎么渲染成光栅 | **是** |
| `printer_profile.c` | 固件 | USB 层怎么伺候：UEL、唤醒、接口复位、单双向 | 是（只有怪癖字段生效） |

`printer_profile.c` 里的身份/能力字段（`make_and_model` / `urf` / `media_*` /
`resolution` …）是本地 IPP 时代用来拼属性应答的，**现在没有任何代码读它们**，
改了不会有任何效果。保留是当实测记录。真正生效的是怪癖那一半：`uel_job_end`、
`uel_wake`、`wake_delay_ms`、`iface_cycle`、`unidir`（`cups_quirks` 只作上报）。

---

## 2. 为什么放弃了「ESP32 自己当 AirPrint 打印机」

前一版真的做出来过，Mac 和 iPad 能打。放弃是因为三个结构性问题，不是没调通：

1. **mDNS 饿死**。iOS 轮询到 28 req/s 时，mDNS 任务（默认优先级 1）抢不到 CPU，
   主机名解析失败，iOS 报「无法联系打印机」。把优先级提到 5 能缓解，但这是在和
   调度器博弈，不是解决。
2. **连接内存**。每条 IPP 连接一个任务栈，iOS 会开 6~8 条并发保活连接，空闲堆从
   100KB 掉到 9KB，随后堆损坏。
3. **持续射频负载导致掉电复位**。coredump 是空的、复位原因是 POWERON——软件无辜。

而且本地方案有个天花板：**mDNS 是链路本地的，跨不了互联网**。要「在公司打印到家里」，
本地 AirPrint 无论如何调优都做不到。

云方案把这三个问题一次性消掉：设备只有一条出站长连接，没有服务端口，没有 mDNS，
没有并发连接。今天实测空闲堆稳定在 72~120KB。

### 这次转向留下了什么残骸

删干净了会丢掉实测记录，留着又会误导接手的人。折中是**留着但标注**。清单如下，
看到这些东西时不用再去考证它们还生不生效：

#### 一、AirPrint 时代留下的

| 残骸 | 现状 |
|---|---|
| `main/ipp_server.c` | 已删除，不在 `CMakeLists.txt` 里 |
| `tools/devtest.py` | **跑不通**，对着 631 端口讲 IPP。留档，文件头已标注 |
| `printer_profile_t` 的身份/能力字段 | 无人读取，改了没效果。留作实测记录 |
| `sdkconfig.defaults` 的 `CONFIG_MDNS_*` | 已删除——mDNS 组件已不编译，那两行是空配置 |
| `CONFIG_LWIP_TCP_WND_DEFAULT=2880` | **前提已失效**：当年是为省内存给 iOS 并发连接，现在只剩一条下载连接，这个值可能反而在限速。待验证，见第 8 节 |

`esp_http_server` 仍在 `CMakeLists.txt` 的 `REQUIRES` 里，**这个不是残骸**——
配网门户 `provision.c` 还在用它。

#### 二、服务端 Go 重写留下的

服务端已用 Go 重写（`server/go/`，二进制 `stickboxd`），设计见
`docs/superpowers/specs/2026-08-30-go-print-server-design.md`。
**但它还没上线**——第 4 节描述的 Python 版仍是当前在跑的那套。

| 东西 | 现状 |
|---|---|
| `server/bin/jobsrv.py` | **仍在生产运行**，不是残骸。切换到 Go 版之后才退役，且回滚要用它 |
| `tools/reference/render.py`、`text2pdf.py` | 已从 `server/bin/` 移出并标注不再部署。**留档不是念旧**：`fix_page_count` 是 App 端 URF 编码器的参考实现，那个坑客户端一样会踩 |
| `server/web/index.html` | 已删除。新版是纯 API，上传由 App 负责 |
| `ppd/hp136a.ppd` | Go 版不读它——光栅搬到手机，服务端不再渲染。留作 `render-profile` 参数的人工核对依据 |
| CUPS / `cupsfilter` / `fonts-noto-cjk` / `python3-gi` | Go 版不需要。**但先别卸载**——回滚到 Python 版要用，跑稳一个月再清 |

#### 三、已经不是残骸的（清单里删掉了，写在这里免得有人再去考证）

| 曾经的残骸 | 现在 |
|---|---|
| 仓库名 `esp-airprint` / 工程名 `airprint_bridge` | **已改名**：仓库 `dayuer/stickbox`，工程 `stickbox`，二进制 `stickboxd`。GitHub 自动重定向旧 URL，但**本地目录仍叫 `esp-airprint`** |
| `main/portal_html.h` 里的「AirPrint 桥配网」字样 | 已改为「StickBox 配网」 |

#### 四、即将变成残骸的

`main/printer_profile.c` 里怪癖那一半（`uel_job_end` / `uel_wake` /
`wake_delay_ms` / `iface_cycle` / `unidir`）——Go 版会通过
`printer/{dev}/profile` 下发怪癖档案，那之后这张编译进固件的表**从「真相」
降级为「连不上服务端时的兜底」**。

服务端侧已经实现（`server/go/internal/profile`），**固件侧还没接**。
接上之前，固件仍完全按这张表走。

---

## 3. 已验证的硬事实（这些是资产）

### 3.1 打印机会自己说它能吃什么

一开始查资料得出「HP Laser 13x 是三星 SPL 内核，需要闭源光栅器」——**错的**。
正确做法是读打印机自己的 IEEE-1284 device-id 字符串，里面明明白白写着
`CMD:...URF...PWGRaster`。**打印机的自述能力表才是真相，网上的型号资料不是。**

- VID/PID：`0x03F0` / `0xF22A`
- 接口：class 7 / subclass 1 / **protocol 2（双向）**
- 端点：bulk OUT `0x04`、bulk IN `0x83`，MPS 64
- URF 能力：`CP1,IS1-4-5-19,MT1-2-3-4-5-6,RS600,V1.4,W8`

### 3.2 UEL 是整个项目最关键的 9 个字节

```c
static const uint8_t UEL[] = { 0x1b, '%', '-', '1', '2', '3', '4', '5', 'X' };
```

Universal Exit Language，HP/三星系的作业分隔符。**不发它只能打第一份**，第二份要
人跑到打印机前按取消键才动。

根因：USB 短包只表示「本次传输结束」，不表示「作业结束」。打印机会一直等后续数据。

这个结论是用户的一句观察定位的——「打印一次后，需要在打印机上按下取消键，才能打印
下一个」。被证伪的假设（都试过，都不是）：data toggle 不同步、bulk IN 没排空、
SOFT_RESET（打印机直接 STALL；CUPS 那条 quirk 只对三星 VID `0x04e8` 有效，
HP 的 `0x03f0` 不适用）、VBUS 断电重枚举（打印机自供电，根本不掉线）。

### 3.3 作业收尾期间绝对不能碰端点

曾经在 `job_end` 里做接口复位，结果稳定在距流尾几 KB 处 `Decoding Fail`——最后一个
短包还没物理冲出去就被 halt/flush 掉了。

**规则：端点复位只在下一份作业开始时做，作业结束时只发 UEL。**

### 3.4 状态获取有两条通道，都是标准的

- `@PJL USTATUS DEVICE=ON` 开启后打印机**主动推**：
  `CODE=10001 DISPLAY="Ready"` / `10023 Printing` / `35078 Power Save`
- USB 打印机类标准请求 `GET_PORT_STATUS`（bmRequestType `0xA1`, bRequest `1`）
  返回 1 字节：bit5 缺纸、bit4 选中、bit3 无错。**这是类标准，对所有 USB 打印机通用**，
  不用一家一家逆向。

两个坑：
- 一个包里可能连着多条 USTATUS，且**同时包含基础态和瞬时态**
  （如 `Ready` + `Printing`）。取最后一条作当前态，同时把非 `10023` 的那条记为基础态。
- 打印机**只在状态变化时推**。我们把活发完它常常不再补一条 `Ready`，状态会永远卡在
  `Printing`。修法：作业结束 20 秒后仍是 `Printing`，就退回它自己声明的基础态。
- `GET_PORT_STATUS` 在休眠时照样返回 `0x18`，**判断不了休眠**，休眠只能靠 USTATUS 推送。

### 3.5 USB 电源时序必须照抄厂商 BSP

顺序错了会枚举不到打印机：

```c
gpio_set_level(PIN_USB_MODE_SEL,   1);   /* ① 先把数据线切到 host 连接器 */
gpio_set_level(PIN_USB_LIMIT_EN,   1);   /* ② 必须为 1——它同时是负载开关使能 */
gpio_set_level(PIN_USB_DEV_VBUS_EN, 0);  /* ③ */
gpio_set_level(PIN_BATTERY_BOOST_EN, 0);
vTaskDelay(pdMS_TO_TICKS(10));
gpio_set_level(PIN_USB_DEV_VBUS_EN, 1);  /* ④ */
```

`LIMIT_EN` 名字看着像「限流使能」，一度以为关掉能解决掉电复位。**关掉的结果是打印机
彻底枚举不到**——它同时是负载开关的使能脚。

---

## 3.6 机型识别：设备侧现在能自己读到什么

枚举时按风险分层探测，**层级不能乱**——探错的代价是打印机把探针当正文打出来，
用户开箱第一件事是收到一张乱码纸。

**第 0 层：零风险，只走控制传输，不碰打印通道。无条件执行。**

| 读什么 | 怎么读 |
|---|---|
| VID / PID / 厂商 / 型号串 | 设备描述符 |
| 接口 7/1/x、protocol（1=单向 2=双向 3=1284.4）、端点、MPS | 配置描述符 |
| **IEEE-1284 设备 ID** | `GET_DEVICE_ID`（类请求 `0xA1` / `bRequest=0`） |
| 缺纸 / 选中 / 出错 | `GET_PORT_STATUS`（`0xA1` / `bRequest=1`） |

`GET_DEVICE_ID` 是整条兼容性链路的地基，两个坑：

- 头两字节是**大端**长度，且**含这两字节自身**
- `wIndex` 是 `(接口号<<8) | alt`，**不是**接口号本身——写错直接 STALL

拿到的 `CMD:` 字段决定第 1 层能不能做。

**第 1 层：条件性。必须由第 0 层的 `CMD:` 授权。**

`CMD:` 里出现 PJL 或 PCL 才发 PJL（`@PJL INFO ID / CONFIG / SUPPLIES …`）。
代码里这个判断在 `enum_task()`，盲发已经堵死。ESC/POS 那类机器要另一套探针，
目前没有。

**第 2 层：必须出纸才测得出来的。无法自动化。**

UEL 到底需不需要、唤醒延时够不够、页边距对不对。尤其 UEL——它的症状是
「第二份不出」，所以**单份作业成功不能说明任何事，必须连打两份**。

### 按需探针 `usb_printer_probe()`

以前 PJL 探针会把整机搞成重启循环，原因不是 PJL 本身：

`usb_printer_job_write()` 用的是**全局唯一**的 `s_xfer` + `s_xfer_done`，而它
**自己不持 `s_job_mutex`**（锁由 `job_begin` 拿、`job_end` 放）。老的 `pjl_send()`
绕过作业流程直接调 `job_write`，于是探针和正在传的作业同时操作同一个
`usb_transfer_t`——对还在飞的 transfer 二次 submit，ESP-IDF 直接 assert。
重启后 MQTT 重投那份作业，再撞一次，就成了循环。

现在拆成三层，探针可以随时调：

- `pjl_send_locked()` —— 裸发，调用者必须已持锁
- `pjl_send_safe()` —— 自己抢锁，抢不到就放弃（探针优先级低于作业）
- `usb_printer_probe()` —— 持锁 + 收回包；**回包由现役的 `status_reader_task`
  顺带抄给它，不另起读 IN 端点的任务**（两个任务在同一 IN 端点上 submit
  是另一条 panic 路径）

云端下发 `printer/{dev}/cmd` → `{"probe":"@PJL INFO ID"}` 即可触发，
结果回到 `printer/{dev}/status`。

**捕获缓冲按调用方给的 `cap` 动态分配**——`INFO VARIABLES` 有好几 KB，
固定大小要么不够要么白占 RAM。想拿大回包就传个大的 `out`。

#### 修掉的三处静默截断（这三条以前让探针数据不可信）

1. **每包截到 128 字节**。`status_reader_task` 用 `char txt[129]`，而一次 bulk IN
   实际读 **512** 字节。`INFO CONFIG` / `VARIABLES` 的回包被切掉且**不报错**——
   看起来像是打印机就回了这么多。现在按 `IN_READ_MAX` 全量处理，日志仍只显示
   前 128 字节避免刷屏。
2. **换行被吃掉**。以前所有非可打印字符一律换成 `.`，包括 `\r\n`。PJL 回包是
   按行的 `key=value`，吃掉换行就没法解析。现在捕获走原始字节，`\r \n \t` 保留。
3. **探针缓冲固定 512 字节**。见上，改成动态。

顺带把 `status_reader_task` 的栈从 4096 提到 6144——`txt[IN_READ_MAX+1]` 是栈上的。

### 插上打印机时的全量采集

`usb_printer_describe()` 出第 0 层 JSON，`ident_task`（cloud_client.c）串起全流程：

```
打印机枚举完成 → watch_task 发信号 → ident_task 等 3 秒让打印机稳定
  → 第 0 层 describe()（控制传输，零风险）
  → 第 1 层 PJL 探针 ×7（仅当 usb_printer_pjl_allowed()）
  → HTTPS POST  /api/device/{dev}/ident
  → MQTT publish printer/{dev}/ident（精简身份，retain=1）
```

**为什么全量走 HTTPS 而不是 MQTT**：全量 4~16KB，而信令通道的原则是「只传
几十字节」（第 1 节）。一份十几 KB 的 retain 消息会让每个订阅者一连上就吃一大口。
MQTT 上只留几百字节的精简身份。

第 1 层的探针清单在 `PJL_PROBES[]`。顺序是**先便宜先成功**，`INFO VARIABLES`
放最后——它最大也最可能超时。缓冲剩余不足 512 字节时主动跳过剩余探针并打日志，
**不静默截断**。

服务端落点：`/api/device/{dev}/ident`（`jobsrv.py`），每台设备存
`/opt/stickbox/idents/{dev}/latest.json` 加按时间戳的历史——机型档案会随探针改进
而变化，覆盖掉就没法比对了。

#### 序列号是主键，不是 MAC

`usb_device_info_t.str_desc_serial_num` 是**每台打印机唯一**的。设备 MAC 标识的是
桥，不是打印机。要做机型库，区分「同一个桥换了打印机」和「同一台打印机换了桥」
只能靠它。

### ⚠ 曾经有个静默失效：`s_prof` 从未被赋值

`profile_lookup()` 写好了却一次都没被调用，`s_prof` 恒为 NULL。后果是
`s_prof && s_prof->uel_wake` 和 `s_prof && s_prof->iface_cycle` 恒假——
**休眠唤醒和作业间接口重置这两条实测怪癖，实际上一直没生效**。
UEL 侥幸活着，因为那行写的是 `(!s_prof || s_prof->uel_job_end)`。

已在 `enum_task()` 里接上。**这意味着连打行为发生了变化，换机器后要重测连打。**

---

## 3.7 兼容性数据来源：CUPS usb-quirks

`main/usb_quirks_db.h` 是**自动生成**的，别手改。重新生成：

```bash
curl -sL https://raw.githubusercontent.com/OpenPrinting/cups/master/backend/org.cups.usb-quirks -o /tmp/q.txt
python3 tools/gen_usb_quirks.py /tmp/q.txt main/usb_quirks_db.h
```

来源是 OpenPrinting CUPS 的 `backend/org.cups.usb-quirks`，**Apache License 2.0**，
可直接使用，保留出处即可。（foomatic-db 多为 GPL，要用之前先确认许可证。）

原表 122 条，导入 96 条。丢弃的两类：

- `no-reattach`（30 条）—— Linux usblp 内核模块专用，ESP32 上没有这个概念
- `whitelist` —— 只是「确认可用」的注记，不产生任何行为

导入的位里，真正会改写行为的只有三条，且**仅在没有手写档案时生效**——手写档案
是实测出来的，优先级高于第三方表：

| 位 | 行为 |
|---|---|
| `QK_UNIDIR` | 停用 IN 端点、不发 PJL。对单向机器读 IN 会一直超时，严重时拖垮 host 栈 |
| `QK_SOFT_RESET` | 打印后做 SOFT_RESET |
| `QK_DELAY_CLOSE` | 释放接口前留延时（映射到 `iface_cycle`） |

`QK_BLACKLIST` / `QK_USB_INIT` / `QK_VENDOR_CLASS` 只打警告不改行为。其中
`QK_VENDOR_CLASS` 值得注意：`find_endpoints()` 只认 `bInterfaceClass == 0x07`，
厂商私有 class 的机器会被判成「不是可用打印机」——这条日志就是排查线索。

### 别高估这张表

**它是冷启动的起点，不是兼容性库。**

- 96 条里 59 条是佳能（`0x04a9`），高度偏向老喷墨
- 你手上这台 HP Laser MFP 136a（`0x03F0:0xF22A`）**不在表里**
- 真正让本项目吃苦头的那些怪癖——作业结束符要不要发、唤醒等多久、
  URF 能不能断流——CUPS 里**一条都没有**

最后一点值得记住原因：**在 Linux 上这些问题不发生。** CUPS 的过滤链自己会吐
PJL 作业包头和 UEL，内存管够，USB 栈完整。所以这类知识不存在于任何公开数据库，
不是因为它稀有，而是因为只有在我们这种约束下它才显形。

反过来说，这也意味着它**不构成对跑 Linux 的竞品的壁垒**——对方根本遇不到。

---

## 4. 服务端

⚠ **本节描述的是当前在跑的 Python 版。** Go 重写版（`server/go/`，二进制
`stickboxd`）已实现但尚未上线，切换步骤见 `server/DEPLOY.md`。两者的差异
（每设备密钥、服务端不渲染、内嵌 broker）见第 2 节残骸清单的第二部分。

机器：`43.165.196.84`（印尼），域名 `mqtt.silkline.id`（Cloudflare 托管）。

```
/opt/stickbox/
  bin/jobsrv.py      作业服务：收文件 / 渲染 / 排队 / 派发 / TLS 9443
  bin/render.py      PDF、图片、文本 → URF
  bin/text2pdf.py    文本 → PDF（PangoCairo）
  ppd/hp136a.ppd     机型 PPD——渲染的机型知识全在这里，换打印机就换它
  idents/{dev}/      设备上报的机型档案：latest.json + 按时间戳的历史
  web/index.html     上传页
  config.json        口令与证书路径（600，不入库）
  jobs/              渲染产物 <jid>.urf
  jobs.db            sqlite 作业表
```

- 服务单元 `stickbox-job.service`，`Requires=mosquitto.service`
- 端口 **9443**（不是 443/8443——那两个被现有的 xray 占着）
- MQTT：`8883` 对外 TLS，`1883` 仅 `127.0.0.1` 给服务端自己用；`allow_anonymous false`

### 证书

Let's Encrypt，`certbot.timer` 自动续签。续签钩子
`/etc/letsencrypt/renewal-hooks/deploy/mosquitto.sh` 负责改权限并重启 mosquitto 和
作业服务——**没有这个钩子，证书续了但 mosquitto 还拿着旧的**。

### 中文字体：唯一能用的路子是 PangoCairo

- CUPS 的 `texttopdf`：不带 CJK 字体 → 方框
- Debian 的 `paps` 0.6.7：输出 PostScript 时**写死 `/Helvetica findfont`**，
  `--font` 参数形同虚设 → 照样方框

`bin/text2pdf.py` 用 PangoCairo 直接排版，字体真嵌进 PDF。验证方法：

```bash
pdffonts out.pdf   # 要看到 NotoSansCJKsc-Regular / CID Type 0C / emb=yes
```

依赖：`fonts-noto-cjk python3-gi python3-gi-cairo gir1.2-pango-1.0 python3-cairo`

### 离线队列

设备离线**不丢件**：渲染好的 URF 留在盘上，作业入队，设备心跳一报到就续上。

- **一次只派一件**。设备没有本地队列、堆只有几十 KB，一次性堆过去等于丢件。
  队列的真相全在服务端 `jobs` 表里。
- 传输中超过 180 秒无进展 → 退回队列重传
- 20 秒一轮后台巡检，兜底 MQTT 派发丢失
- 作业状态：`queued → downloading → done / failed`

---

## 5. 心跳与判活（踩过的坑，别再犯）

固件最初只在**状态变化时**上报。结果：板子好好的、打印机就绪，服务器却说
「没有在线的打印桥」——因为状态一直是 `ready` 没变过，它就再也不吭声了。

**规则：状态一变立刻推（2 秒内），没变也每 30 秒推一次。**
离线队列、断点续传、界面上的「对方设备离线」全部建立在判活之上；判活是假的，
上面三层都是假的。

心跳载荷：

```json
{"dev":"f412fa87c9e0","job":"","state":"ready","bytes":0,"heap":120708,
 "prn":{"code":10001,"display":"Ready","online":true,"asleep":false,
        "paper_out":false,"error":false}}
```

---

## 6. 固件

```
main/
  main.c            引导、Wi-Fi、USB 电源时序、任务编排
  cloud_client.c    MQTT 信令 + HTTPS 流式取件 + 心跳
  usb_printer.c     USB Host 打印机驱动（本项目核心资产）
  printer_profile.c 每型号一份 USB 层怪癖配置（作业结束符、唤醒、接口复位）
                    ——身份/能力字段是 IPP 时代遗留，已无人读，见第 1 节
  usb_quirks_db.h   CUPS usb-quirks 导入表，自动生成勿手改，见第 3.7 节
  provision.c       首次配网（SoftAP + 强制门户）
  joblog.c          作业各阶段落 NVS，用于复位后倒查
  lcd_ui.c          LCD 显示
  netlog.c          UDP 日志广播
  cloud_creds.h     云端地址与口令（不入库，见 .example）
```

编译烧录：

```bash
source ~/esp/esp-idf-v5.5/export.sh
cd ~/sproot/stickbox
cp main/cloud_creds.h.example main/cloud_creds.h   # 首次，填自己的
idf.py build
idf.py -p /dev/cu.usbmodemXXXX flash
```

### 烧录必踩的坑

**固件一启动就把 USB 数据线切到 host 口去驱动打印机，Mac 上的串口随之消失。**
所以第二次之后烧录都要先把板子按进下载模式：

> 按住 **BOOT** → 点一下 **RST** → 松开 BOOT

不这么做，`idf.py flash` 会报 `Could not open /dev/cu.usbmodemXXXX`。

### 其它固件级教训

- **LVGL 只能在自己的任务里调**。曾经从网络任务栈（4608 字节）直接调 `lcd_ui_job()`，
  栈溢出崩溃。现在走队列 + 专用 UI 任务（6144 栈）。
- **netlog 的 vprintf 钩子里绝不能直接 sendto**，会和 lwip 死锁。必须走队列解耦。
- 首次配网：长按 **MENU 键（GPIO14）** 开机可擦除 Wi-Fi 配置。

---

## 7. 排障手册

| 症状 | 先查什么 |
|---|---|
| 机型档案没上来 | 服务端 `journalctl -u stickbox-job -f` 看有没有 `[ident]`；设备日志看「机型档案已上传 HTTP 200」 |
| 第 1 层探针全空 | `usb_printer_pjl_allowed()` 是否为真——`CMD:` 里没 PJL/PCL 就不发，这是有意的 |
| 探针回包明显不全 | 看是不是又有人把收包缓冲改小了，见第 3.6 节那三处截断 |
| 页面显示「等打印桥上线」 | `/api/status` 里 `seen` 多久前；超 120 秒判离线 |
| 只能打第一份，第二份要按取消键 | UEL 没发出去，看日志有无「已发送 UEL 作业结束符」 |
| 距流尾几 KB 处 Decoding Fail | 作业收尾时碰了端点 |
| 中文变方框 | `pdffonts` 看字体嵌没嵌进去；八成是 render 路径没走 text2pdf.py |
| 状态卡在 Printing | 基础态兜底没生效；打印机不会主动补 Ready |
| `idf.py flash` 打不开串口 | 按 BOOT+RST 进下载模式 |
| 打印机枚举不到 | 查 USB 电源时序，尤其 `LIMIT_EN` 是不是被置 0 了 |

设备日志：板子通过 UDP 5140 广播，`nc -ul 5140`。
服务端日志：`journalctl -u stickbox-job -f`。
复位后倒查上一份作业卡在哪个阶段：`joblog_boot_report()` 开机时打印。

---

## 8. 遗留与下一步

**遗留**
- 打印机耗材余量（碳粉）取不到。`@PJL INFO STATUS` 和 `INFO SUPPLIES` 都返回空，
  只有 `INFO ID` 有效。这台机器大概率不支持，需要换 SNMP / Printer MIB（RFC 3805）验证。
- 状态从 `Printing` 退回基础态实测约 88 秒才发生，比设计的 20 秒慢，原因未查。
- 只有一台设备在跑，多设备路由没验证过。
- **取件速度没量过**。`CONFIG_LWIP_TCP_WND_DEFAULT=2880` 是本地 IPP 时代为省内存
  定的，云架构下只剩一条 HTTPS 下载连接，这个窗口很可能是当前的吞吐瓶颈。
  先量一份 500KB 作业的端到端耗时，再试 5760 / 11520，对比空闲堆。
- **设备凭据是全局共用的一组**。`cloud_creds.h` 里的 MQTT 账号密码所有设备相同，
  主题只按 MAC 分。拆一台机器就能订阅所有人的作业主题。自己用无所谓，
  一旦发给第二个人用，这是必须先换成一机一凭证的阻塞项。

**下一步（如果要往「社交打印」方向走）**
最缺的不是硬件，是**身份**。现在系统里只有设备 MAC，没有 User ID、没有关系链。
建议先做最小的「扫码绑定 + 我发给我自己」，跑通之后一对多、双向白名单、
配额、免打扰都只是它上面的策略层，不用重做架构。

两件必须提前想清楚的事：
- **配额不是反骚扰，是成本**。消耗的是接收方的纸和碳粉，所以默认值要极紧
  （陌生人 0 页），而不是默认宽松再收紧。
- **内容审核必须在渲染前拦**。渲染完就是光栅字节流，审核接口读不懂了。
  而且物理打印没有撤回。
