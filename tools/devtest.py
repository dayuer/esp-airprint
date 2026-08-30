#!/usr/bin/env python3
"""
⚠ 已废弃，跑不通了 —— 留档用，不是当前的测试工具。

本脚本对着设备的 631 端口讲 IPP。设备转向云架构后不再跑 IPP 服务器、
不再监听任何端口（见 docs/HANDOFF-cloud-print.md 第 2 节），所以这里的
每一次连接都会被拒。

当前验证链路的方式：往云服务器的上传页投文件，然后看
  - 设备日志：`nc -ul 5514`
  - 服务端日志：`journalctl -u stickbox-job -f`

保留原因：下面复刻的那套 iOS 行为（并发轮询、Create-Job/Send-Document
两段式、chunked + 100-continue、连接内流水线）是实测总结出来的，
将来若要重新评估本地方案，这份行为清单省一遍逆向。

────────────────────────────────────────────────────────────

dev 测试台：从一个 PDF 出发，完整复刻 iOS 的 AirPrint 行为并逐步校验。

  ./devtest.py <桥的IP> [文件.pdf]

复刻的 iOS 行为：
  - 多条并发 keep-alive 长连接持续轮询 Get-Printer-Attributes
  - 作业走 Validate-Job → Create-Job → Send-Document（不是 Print-Job）
  - 请求体用 chunked 分块 + Expect: 100-continue
  - 同一条连接上连续发多个请求（流水线），这是 Mac 不会做而 iOS 会做的
  - 文档格式 image/urf，由 PDF 经打印机自己的 PPD 渲染而来
"""
import socket, struct, threading, time, sys, os, subprocess, zlib, tempfile

PPD = "/private/etc/cups/ppd/HP_Inc__HP_Laser_MFP_136a.ppd"
G = "\033[32m"; R = "\033[31m"; Y = "\033[33m"; N = "\033[0m"
def ok(m):   print(f"  {G}✓{N} {m}")
def bad(m):  print(f"  {R}✗{N} {m}")
def info(m): print(f"  {Y}·{N} {m}")

# ── PDF → URF（跟 iOS 端渲染同理：用打印机自己的能力描述来渲染）──
def pdf_to_urf(pdf):
    out = os.path.join(tempfile.gettempdir(), "devtest.urf")
    if not os.path.exists(PPD):
        bad(f"找不到 PPD: {PPD}"); sys.exit(1)
    r = subprocess.run(["/usr/sbin/cupsfilter", "-P", PPD, "-m", "image/urf", pdf],
                       capture_output=True)
    if r.returncode != 0 or not r.stdout:
        bad(f"PDF 渲染失败: {r.stderr.decode()[:200]}"); sys.exit(1)
    open(out, "wb").write(r.stdout)
    return out

def make_pdf():
    """没给 PDF 就现做一个，内容带时间戳便于辨认"""
    path = os.path.join(tempfile.gettempdir(), "devtest.pdf")
    stamp = time.strftime("%Y-%m-%d %H:%M:%S")
    txt = path.replace(".pdf", ".txt")
    open(txt, "w").write(
        f"StickBox - dev test\n\n{stamp}\n\n"
        "This page came from a PDF, rendered to URF, and pushed\n"
        "through the bridge exactly the way an iPhone would.\n")
    subprocess.run(["/usr/sbin/cupsfilter", "-m", "application/pdf", txt],
                   stdout=open(path, "wb"), stderr=subprocess.DEVNULL)
    return path

# ── IPP ──
_lk = threading.Lock(); _rid = [0]
def rid():
    with _lk: _rid[0] += 1; return _rid[0]

def attr(tag, name, val):
    v = val.encode() if isinstance(val, str) else val
    return bytes([tag]) + struct.pack('>H', len(name)) + name.encode() + \
           struct.pack('>H', len(v)) + v

def req(host, op, extra=b'', doc=b''):
    b = struct.pack('>HHI', 0x0200, op, rid()) + b'\x01'
    b += attr(0x47, 'attributes-charset', 'utf-8')
    b += attr(0x48, 'attributes-natural-language', 'zh')   # iOS 中文环境
    b += attr(0x45, 'printer-uri', f'ipp://{host}.:631/ipp/print')   # iOS 带结尾点
    b += attr(0x42, 'requesting-user-name', 'devtest')
    b += extra + b'\x03'
    return b + doc

def post(sock, host, body, expect=False, timeout=90):
    hdr = (f'POST /ipp/print HTTP/1.1\r\nHost: {host}\r\n'
           f'Content-Type: application/ipp\r\nTransfer-Encoding: chunked\r\n'
           f'Connection: keep-alive\r\n')
    if expect: hdr += 'Expect: 100-continue\r\n'
    sock.sendall((hdr + '\r\n').encode())
    if expect:
        sock.settimeout(15)
        pre = sock.recv(200)
        if b'100' not in pre:
            return None, 'no-100-continue:' + pre[:60].decode('latin1', 'replace')
    sock.settimeout(timeout)
    for i in range(0, len(body), 8000):
        c = body[i:i+8000]
        sock.sendall(f'{len(c):x}\r\n'.encode() + c + b'\r\n')
    sock.sendall(b'0\r\n\r\n')
    buf = b''
    while b'\r\n\r\n' not in buf:
        d = sock.recv(4096)
        if not d: return None, 'closed'
        buf += d
    head, rest = buf.split(b'\r\n\r\n', 1)
    if '100 Continue' in head.decode('latin1').split('\r\n')[0]:
        buf = rest
        while b'\r\n\r\n' not in buf:
            d = sock.recv(4096)
            if not d: return None, 'closed-after-100'
            buf += d
        head, rest = buf.split(b'\r\n\r\n', 1)
    n = 0
    for l in head.decode('latin1').split('\r\n'):
        if l.lower().startswith('content-length:'): n = int(l.split(':')[1])
    while len(rest) < n:
        d = sock.recv(4096)
        if not d: break
        rest += d
    if len(rest) >= 8:
        _, st, _ = struct.unpack('>HHI', rest[:8])
        return st, 'ok'
    return None, 'short'

# iOS 实际点名要的四个属性（真机抓包所得），不是 "all"
def _multi(tag, name, vals):
    b = attr(tag, name, vals[0])
    for v in vals[1:]:
        b += bytes([tag]) + struct.pack('>H', 0) + struct.pack('>H', len(v)) + v.encode()
    return b

REQ_ALL = _multi(0x44, 'requested-attributes', [
    'printer-state-reasons', 'media-source-supported',
    'printer-input-tray', 'printer-mandatory-job-attributes'])

def poller(host, idx, stop, errs):
    """iOS 的常驻探测连接：一条长连接上反复问属性"""
    try:
        s = socket.create_connection((host, 631), timeout=15)
        while not stop.is_set():
            st, m = post(s, host, req(host, 0x000B, REQ_ALL))
            if st != 0: errs.append(f'轮询{idx}: {st} {m}'); return
            time.sleep(0.5)
        s.close()
    except Exception as e:
        if not stop.is_set(): errs.append(f'轮询{idx} 异常: {e}')

def main():
    host = sys.argv[1] if len(sys.argv) > 1 else 'hp136a-bridge.local'
    pdf  = sys.argv[2] if len(sys.argv) > 2 else None

    print(f"\n{Y}══ StickBox · dev 全流程测试 ══{N}")
    print(f"目标: {host}\n")

    print("① 准备文档")
    if not pdf:
        pdf = make_pdf(); info(f"自动生成 PDF: {pdf}")
    urf = pdf_to_urf(pdf)
    doc = open(urf, 'rb').read()
    crc = zlib.crc32(doc) & 0xffffffff
    ok(f"PDF → URF: {len(doc)} 字节, CRC32={crc:08x}")
    if doc[:8] != b'UNIRAST\0': bad("URF 魔数不对"); sys.exit(1)

    print("\n② 起 6 条并发轮询（复刻 iOS 的常驻探测）")
    stop = threading.Event(); errs = []
    ts = [threading.Thread(target=poller, args=(host, i, stop, errs), daemon=True)
          for i in range(6)]
    for t in ts: t.start()
    time.sleep(4)
    if errs: [bad(e) for e in errs[:3]]
    else:    ok("6 条轮询连接稳定")

    print("\n③ 提交打印作业（与轮询并发，同一连接上流水线发请求）")
    s = socket.create_connection((host, 631), timeout=20)
    steps = [("Validate-Job", 0x0004, REQ_ALL, b''),
             ("Get-Printer-Attributes", 0x000B, REQ_ALL, b''),
             ("Create-Job", 0x0005, REQ_ALL, b'')]
    good = True
    for name, op, extra, d in steps:
        st, m = post(s, host, req(host, op, extra, d))
        (ok if st == 0 else bad)(f"{name:24} → " + (f"0x{st:04x}" if st is not None else m))
        if st != 0: good = False
    body = req(host, 0x0006,
               attr(0x21, 'job-id', struct.pack('>I', 1)) +
               attr(0x49, 'document-format', 'image/urf') +
               attr(0x22, 'last-document', b'\x01'), doc)
    st, m = post(s, host, body, expect=True)
    (ok if st == 0 else bad)(f"{'Send-Document':24} → " +
                             (f"0x{st:04x} ({len(doc)} 字节)" if st is not None else m))
    if st != 0: good = False
    st, m = post(s, host, req(host, 0x0009))
    (ok if st == 0 else bad)(f"{'Get-Job-Attributes':24} → " +
                             (f"0x{st:04x}" if st is not None else m))
    s.close()

    stop.set(); time.sleep(1)
    print(f"\n{Y}══ 结果 ══{N}")
    print(f"  轮询错误: {len(errs)}")
    for e in errs[:5]: print(f"    {e}")
    print(f"  作业: {G+'通过'+N if good else R+'失败'+N}")
    print(f"\n对照桥端日志应看到：文档头 UNIRAST / CRC32={crc:08x} / UEL / 作业完成\n")

main()
