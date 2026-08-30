# esp-airprint

用一块 **ESP32-S3-USB-OTG** 开发板，把只有 USB 口的打印机变成**可以从任何地方投递**
的网络打印机。实测机型：**HP Laser MFP 136a**。

```
浏览器/App ──HTTPS──▶ 云服务器 ────MQTT/TLS(信令)────▶ ESP32 ──USB──▶ 打印机
                    (渲染成 URF)  ◀──MQTT/TLS(状态)───┘
                         └────────HTTPS(文档流式)─────▶
```

设备侧只做一件事：**当一根管子**。把字节搬进 USB，把打印机状态搬出来。
它不认识文档格式、不跑 IPP、不跑 mDNS，只有一条出站长连接。

## 为什么可行：打印机自己就懂

关键发现来自打印机自报的 IEEE-1284 设备 ID：

```
MFG:HP;CMD:SPL,URF,FWV,PIC,RDS,AMPV,PWGRaster,EXT;PRN:4ZB85A;
MDL:HP Laser MFP 131 133 135-138;CID:HPLJPCLMSMV1
```

`CMD:` 里有 **URF**（Apple Raster）——打印机原生就能解码，不需要闭源光栅器。
**打印机的自述能力表才是真相，网上的型号资料不是。**

## 为什么走云、不在设备上跑 AirPrint

本地 AirPrint 版本真的做出来过、能打印，但有三个结构性问题：mDNS 在 iOS 高频轮询下
被饿死、并发连接吃光内存、持续射频负载导致掉电复位。而且 **mDNS 是链路本地的，
跨不了互联网**——「在公司打印到家里」本地方案无论怎么调优都做不到。

云方案把这些一次消掉。实测设备空闲堆稳定在 72~120KB。

## 快速上手

**设备端**

```bash
source ~/esp/esp-idf-v5.5/export.sh
cp main/cloud_creds.h.example main/cloud_creds.h   # 填自己的服务器和口令
idf.py build
idf.py -p /dev/cu.usbmodemXXXX flash
```

> ⚠️ 固件一启动就把 USB 数据线切到 host 口去驱动打印机，串口会消失。
> 第二次之后烧录都要先**按住 BOOT → 点 RST → 松开 BOOT** 进下载模式。

首次上电会开一个 `AirPrint-Setup-XXXX` 热点，手机连上后自动弹配网页。
长按 MENU 键（GPIO14）开机可擦除 Wi-Fi 配置。

**服务端**

```bash
cp server/config.example.json /opt/airprint/config.json   # 填口令，chmod 600
apt install fonts-noto-cjk python3-gi python3-gi-cairo gir1.2-pango-1.0 python3-cairo
cp server/airprint-job.service /etc/systemd/system/ && systemctl enable --now airprint-job
```

## 特性

- **离线不丢件**：设备离线时作业排队，上线自动续打，一次只派一件
- **中文正常**：文本走 PangoCairo 排版，字体真嵌进 PDF（CUPS 的 texttopdf 和
  Debian 的 paps 0.6.7 都会输出方框）
- **打印机状态回传**：面板文字、缺纸、开盖、故障、休眠
- **多机型**：`printer_profile.c` 一个型号一份配置

## 文档

**[docs/HANDOFF-cloud-print.md](docs/HANDOFF-cloud-print.md)** —— 完整移交文档：
架构决策、已验证的硬事实（URF / UEL / USB 时序 / PJL 状态）、服务端部署、
排障手册、遗留问题。接手先读它。
