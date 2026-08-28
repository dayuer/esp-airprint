# esp-airprint

用一块 **ESP32-S3-USB-OTG** 开发板，把只有 USB 口的打印机变成 iPhone 可直接使用的
AirPrint 网络打印机。实测机型：HP Laser MFP 136a。

```
iPhone ──Wi-Fi / IPP──▶ ESP32-S3 ──USB bulk──▶ USB 打印机
         (URF 文档流)     纯透传，零渲染
```

## 原理

HP Laser 13x 系全家共用固件，其中 136w/136nw 是 AirPrint 认证机型——所以 136a
的固件里**本来就带 URF（Apple Raster）解释器**，只是没有网口。设备 ID 实测：

```
CMD: SPL, URF, FWV, PIC, RDS, AMPV, PWGRaster, EXT
```

因此桥不做任何图像处理：mDNS 把自己广播成 AirPrint 打印机，iPhone 渲染好 URF
发过来，桥把字节流原样灌进 USB bulk 端点。全程流式转发，几 KB 缓冲，
无 PSRAM 的 N8 板子绰绰有余。

## 组成

| 文件 | 职责 |
|---|---|
| `main/usb_printer.c` | USB host 打印机类驱动（枚举 / claim 7-x-x 接口 / bulk OUT + ZLP / 20s 超时自愈 / CRC32 审计） |
| `main/ipp_server.c` | 裸 socket HTTP + IPP 服务器（支持 `Transfer-Encoding: chunked` 与 `Expect: 100-continue`，8 个 IPP 操作） |
| `main/main.c` | Wi-Fi station + mDNS `_ipp._tcp` / `_universal` 子类型 + TXT 记录 |
| `main/netlog.c` | UDP 网络日志（队列解耦，绝不在 vprintf 钩子里 sendto——会和 lwip 自死锁） |

## 构建

ESP-IDF v5.5：

```
cp main/wifi_creds.h.example main/wifi_creds.h   # 填入 2.4GHz SSID/密码
idf.py set-target esp32s3
idf.py -p <PORT> flash
```

注意：固件切入 USB host 模式后，板子的 DEV 口会从电脑上消失，属正常现象；
重刷需 BOOT+RST 进下载模式。日志走 UART0 调试口或 UDP 5140 广播。

## 状态

- [x] USB host 直推 URF 出纸（含多页 / 中文文档）
- [x] Mac → Wi-Fi → 桥 → 打印机全链路出纸（CUPS driverless 队列）
- [ ] 修复大文档流传输中的数据腐蚀（URF Decoding Fail @ ~49KB，排查中）
- [ ] mDNS 被 iPhone 发现 + 端到端 AirPrint 联调
