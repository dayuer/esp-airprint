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

**为什么 ESP32 不认识格式**：格式差异全部收敛在服务端的 `printer_profile` 里。今天这台
激光机说 URF + PJL；换成热敏小票机就是 ESC/POS。设备侧代码一行不用改。

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

## 4. 服务端

机器：`43.165.196.84`（印尼），域名 `mqtt.silkline.id`（Cloudflare 托管）。

```
/opt/airprint/
  bin/jobsrv.py      作业服务：收文件 / 渲染 / 排队 / 派发 / TLS 9443
  bin/render.py      PDF、图片、文本 → URF
  bin/text2pdf.py    文本 → PDF（PangoCairo）
  web/index.html     上传页
  config.json        口令与证书路径（600，不入库）
  jobs/              渲染产物 <jid>.urf
  jobs.db            sqlite 作业表
```

- 服务单元 `airprint-job.service`，`Requires=mosquitto.service`
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
  printer_profile.c 每型号一份配置：纸张、分辨率、作业结束符、唤醒策略
  provision.c       首次配网（SoftAP + 强制门户）
  joblog.c          作业各阶段落 NVS，用于复位后倒查
  lcd_ui.c          LCD 显示
  netlog.c          UDP 日志广播
  cloud_creds.h     云端地址与口令（不入库，见 .example）
```

编译烧录：

```bash
source ~/esp/esp-idf-v5.5/export.sh
cd ~/sproot/esp-airprint
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
| 页面显示「等打印桥上线」 | `/api/status` 里 `seen` 多久前；超 120 秒判离线 |
| 只能打第一份，第二份要按取消键 | UEL 没发出去，看日志有无「已发送 UEL 作业结束符」 |
| 距流尾几 KB 处 Decoding Fail | 作业收尾时碰了端点 |
| 中文变方框 | `pdffonts` 看字体嵌没嵌进去；八成是 render 路径没走 text2pdf.py |
| 状态卡在 Printing | 基础态兜底没生效；打印机不会主动补 Ready |
| `idf.py flash` 打不开串口 | 按 BOOT+RST 进下载模式 |
| 打印机枚举不到 | 查 USB 电源时序，尤其 `LIMIT_EN` 是不是被置 0 了 |

设备日志：板子通过 UDP 5514 广播，`nc -ul 5514`。
服务端日志：`journalctl -u airprint-job -f`。
复位后倒查上一份作业卡在哪个阶段：`joblog_boot_report()` 开机时打印。

---

## 8. 遗留与下一步

**遗留**
- 打印机耗材余量（碳粉）取不到。`@PJL INFO STATUS` 和 `INFO SUPPLIES` 都返回空，
  只有 `INFO ID` 有效。这台机器大概率不支持，需要换 SNMP / Printer MIB（RFC 3805）验证。
- 状态从 `Printing` 退回基础态实测约 88 秒才发生，比设计的 20 秒慢，原因未查。
- 只有一台设备在跑，多设备路由没验证过。

**下一步（如果要往「社交打印」方向走）**
最缺的不是硬件，是**身份**。现在系统里只有设备 MAC，没有 User ID、没有关系链。
建议先做最小的「扫码绑定 + 我发给我自己」，跑通之后一对多、双向白名单、
配额、免打扰都只是它上面的策略层，不用重做架构。

两件必须提前想清楚的事：
- **配额不是反骚扰，是成本**。消耗的是接收方的纸和碳粉，所以默认值要极紧
  （陌生人 0 页），而不是默认宽松再收紧。
- **内容审核必须在渲染前拦**。渲染完就是光栅字节流，审核接口读不懂了。
  而且物理打印没有撤回。
