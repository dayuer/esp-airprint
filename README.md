# esp-airprint

用一块 **ESP32-S3-USB-OTG** 开发板，把只有 USB 口的打印机变成 iPhone / iPad / Mac
可直接使用的 AirPrint 网络打印机。实测机型：**HP Laser MFP 136a**。

```
iPhone/iPad/Mac ──Wi-Fi / IPP──▶ ESP32-S3 ──USB bulk──▶ USB 打印机
                   (URF 文档流)    纯透传，零渲染
```

## 为什么可行：打印机自己就懂 AirPrint

关键发现来自打印机自报的 IEEE-1284 设备 ID：

```
MFG:HP;CMD:SPL,URF,FWV,PIC,RDS,AMPV,PWGRaster,EXT;PRN:4ZB85A;
MDL:HP Laser MFP 131 133 135-138;CID:HPLJPCLMSMV1
```

`CMD:` 里有 **URF**（Apple Raster）。HP 给 13x 整个家族共用一份固件，而其中的
136w / 136nw 是 AirPrint 认证机型——**URF 解释器本来就在盒子里，136a 只是没长网口**。

所以这座桥不做任何图像处理：mDNS 把自己广播成 AirPrint 打印机，iPhone 渲染好 URF
发过来，桥把字节流原样灌进 USB bulk 端点。全程流式转发，无 PSRAM 的板子绰绰有余。

> ⚠️ 别被搜索结果误导：网上查 "HP Laser 13x" 会得到「三星 SPL 引擎 / HPLIP 不支持 /
> 需要闭源 rastertospl」的结论。那只对 `CMD:` 列表里 SPL 那一项成立。据此判定
> 「必须在 MCU 上重写栅格编码器」是错的——打印机自报的能力清单才是真相。

## 当前状态

| 链路 | 状态 |
|---|---|
| USB host 直推 URF（固化在 flash） | ✅ |
| Mac → Wi-Fi → 桥 → 打印机 | ✅ |
| iPad → Wi-Fi → 桥 → 打印机 | ✅ |
| **连续多份作业** | ✅（关键在 UEL，见下） |
| iPhone | ⚠️ 文档能到桥并完成，但设备端有时显示「打印机离线」——疑为 iOS 缓存了早期版本的
故障状态（同系统的 iPad 正常）。桥侧零拒绝、零失败。 |
| 477KB 图形页 CRC 端到端一致 | ✅ |

## 踩过的坑（这部分才是这个仓库的价值）

### 1. 连打只出第一份 —— 必须发 UEL 作业结束符

**症状**：第一份正常出纸，之后全部卡死或打出 `URF Decoding Fail 11-1113` 报错单，
**必须手动按打印机上的取消键才能打下一份**。

**根因**：USB 短包只表示「传输结束」，不表示「作业结束」。裸灌 URF 时打印机一直在等
后续数据。HP/三星系的作业分隔符是 **UEL（Universal Exit Language）**——作业末尾追加
9 字节 `\x1b%-12345X` 即解决。

**被证伪的假设**（别再走一遍）：

- data toggle 失配 —— 试过 `CLEAR_FEATURE(ENDPOINT_HALT)` 两侧归零，无效
- bulk IN 未排空 —— 读了，打印机根本不回传数据
- 打印机类 SOFT_RESET —— 该机型直接 STALL。且 CUPS 的 `soft-reset` quirk 只针对
  VID `0x04e8`（三星原厂），不含 HP 的 `0x03f0`
- VBUS 断电重枚举 —— 自供电打印机不会因此重连，必然超时失败

### 2. 作业尾巴被自己掐掉

`job_end` 里做 `endpoint_halt/flush` 会把**还没物理发完的最后一个短包**丢掉，打印机
报 Decoding Fail 且**位置固定在流末尾附近**（如 164239 字节的作业固定报 0x27d12）。

**正确做法**：接口重置放在**下一份作业开始前**（`job_begin`），作业刚结束时绝不碰端点。

### 3. iOS 的并发连接会榨干内存

iOS 打印一页会开约 6~10 条并发 TCP 连接轮询 `Get-Printer-Attributes`。每连接一个
FreeRTOS 任务的模型下，算上 lwip 缓冲每条约 8~10KB，可用堆只有 ~80KB。

- **绝不能 reject accept**：发 RST 会让 iOS 直接判定「打印机不可用」。名额紧张时应
  在响应里带 `Connection: close` 主动卸载空闲长连接
- **保护门槛必须覆盖「创建一个任务真正要花的内存」**（栈 + TCB ≈ 5.7KB）。门槛设得比
  这还低的话，放行后 `xTaskCreate` 必然失败，连接同样被无声关掉
- 属性响应预生成一次共享只读，每连接应答缓冲从 3KB 降到 1KB

### 4. IPP / AirPrint 的挑剔之处

- `printer-uuid` **必须是合法十六进制**。曾经写成 `e5p32b71-...`，`p`/`s` 不是 hex，
  iOS 直接拒绝并陷入无限重试
- 声明了 `image/pwg-raster` 就必须补齐 `pwg-raster-document-*` 全家，否则不如只声明
  `image/urf`
- iOS 走 **Create-Job(0x0005) + Send-Document(0x0006)**，不是 Print-Job。必须实现完整
  的 job 状态机（pending → processing → completed）和 `Close-Job`
- 收到 `Expect: 100-continue` 必须立即回 `HTTP/1.1 100 Continue`
- 请求体是 **chunked** 且长度未知，`esp_http_server` 不解 chunked 请求体，所以本项目
  用裸 socket 自己写了 HTTP 层
- mDNS 必须带 `_universal._sub._ipp._tcp` 子类型，端口必须 631，TXT 里 `URF` 不能为空

### 5. URF 流不能断流

打印机的 URF 解码器受不了页内断流。「读 8K → 写 8K」串行交替加上小 TCP 窗口，实测
只有 ~25KB/s 且打打停停，打印机报错。解法是**网络收与 USB 写解耦**：中间放一个
StreamBuffer 蓄水池 + 独立的 USB 泵任务。

### 6. 板子本身的坑

- 固件切入 USB host 模式后，板子的 DEV 口会从电脑上消失（`MODE_SEL` 把 D+/D− 在两个
  连接器之间二选一），**重刷必须 BOOT+RST 进下载模式**
- 原生 USB 口的 DTR/RTS 是虚拟捆绑到 GPIO0/EN 的。pyserial 打开端口默认拉高两者，
  会把芯片按进下载模式且复位不出来。读日志必须 `serial_for_url(do_not_open=True)`
  先设 `dtr=False, rts=False` 再 open
- LVGL 9.5 内置的 CJK 字库只有 1293 字且偏日文向，「印/业/网/连」等常用字缺失显方块。
  本项目自建 GB2312 一级字库（3769 字，16px），**在 flash 里不占 RAM**

## 组成

| 文件 | 职责 |
|---|---|
| `main/usb_printer.c` | USB host 打印机类驱动：枚举 / claim 7-x-x 接口 / bulk OUT / UEL 作业结束符 / 作业边界接口重置 / GET_PORT_STATUS 诊断 / CRC32 审计 |
| `main/ipp_server.c` | 裸 socket HTTP + IPP 服务器：chunked 请求体、100-continue、9 个 IPP 操作、job 状态机、连接卸载策略 |
| `main/main.c` | Wi-Fi station + mDNS `_ipp._tcp` / `_universal` + TXT 记录 |
| `main/lcd_ui.c` | 板载 ST7789 中文状态屏（网络 / 打印机 / 作业 / 日志） |
| `main/netlog.c` | UDP 网络日志（队列解耦——绝不在 vprintf 钩子里 sendto，会和 lwip 自死锁） |
| `main/font_cn_16.c` | 自建 GB2312 一级字库，3769 字 |

## 构建

需要 ESP-IDF v5.5：

```bash
cp main/wifi_creds.h.example main/wifi_creds.h   # 填入 2.4GHz SSID/密码
idf.py set-target esp32s3
idf.py build
idf.py -p <PORT> flash
```

刷机前把板子按进下载模式：**按住 BOOT/OK → 点一下 RST → 松开 BOOT/OK**。

日志有两条路：板子的 Micro-USB「USB-UART0」口，或 UDP 广播到 5140 端口
（`netlog.c` 里改成开发机 IP 可走单播，路由器常滤广播）。

## 调试工具

`ios_sim.py`（见 git 历史 / scratchpad）模拟 iOS 客户端行为：N 条并发 keep-alive
长连接轮询 + 完整的 Validate → Create-Job → Send-Document 作业流程。**先在本地复现，
再动真机**——这个项目里大量时间浪费在盲刷上。

## 待办

- iPhone 显示「打印机离线」的问题（iPad 同系统正常，疑为设备端缓存）
- 连接模型改为 select() 单任务事件循环，彻底消除每连接 4.6KB 栈的开销
- 作业落盘缓冲（断电续打）
