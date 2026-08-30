#!/usr/bin/env python3
"""安全地读板子的串口日志。

  ./serial_log.py [端口] [波特率]

**为什么不能直接用 idf.py monitor / screen / 裸 pyserial：**

这块 ESP32-S3-USB-OTG 的 DTR/RTS 是虚拟捆绑到 GPIO0/EN 的。pyserial 打开端口时
默认把两者都拉高，效果等于「按住 BOOT 再按 RST」——芯片被按进下载模式，
而且复位不出来，表现为一片死寂。

正确做法是 open 之前就把两条线摁下去：
    serial_for_url(..., do_not_open=True)  →  dtr=False, rts=False  →  open()
顺序不能反，构造时就 open 的话已经晚了。

本工具只读不写，不会复位板子。想看开机日志就在它跑起来之后手动点一下 RST。
"""
import sys, time

try:
    import serial
except ImportError:
    sys.exit('缺 pyserial：pip install pyserial')

port = sys.argv[1] if len(sys.argv) > 1 else None
baud = int(sys.argv[2]) if len(sys.argv) > 2 else 115200

if not port:
    from serial.tools import list_ports
    cand = [p.device for p in list_ports.comports()
            if 'usbmodem' in p.device or 'usbserial' in p.device
            or 'SLAB' in p.device or 'wchusb' in p.device]
    if not cand:
        sys.exit('没找到板子的串口，把端口名当参数传进来')
    port = cand[0]
    print(f'# 自动选择 {port}', file=sys.stderr)

s = serial.serial_for_url(port, baudrate=baud, do_not_open=True)
s.dtr = False          # ← 必须在 open 之前
s.rts = False
s.timeout = 0.2
s.open()
print(f'# 已打开 {port} @ {baud}，只读不复位。想看开机日志请点一下 RST。'
      f'  Ctrl-C 退出', file=sys.stderr)

buf = b''
try:
    while True:
        d = s.read(4096)
        if not d:
            continue
        buf += d
        while b'\n' in buf:
            line, buf = buf.split(b'\n', 1)
            sys.stdout.write(line.decode('utf-8', 'replace').rstrip('\r') + '\n')
            sys.stdout.flush()
except KeyboardInterrupt:
    pass
finally:
    s.close()
