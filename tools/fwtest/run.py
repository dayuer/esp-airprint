#!/usr/bin/env python3
"""固件协议与线上 API 的联调。

用一个假设备跑完整链路，**并且用固件里那份真正的解析代码**去解服务端下发的
档案和作业信令——协议对不对不靠脑补，靠跑。

    cc -o /tmp/decode_profile tools/fwtest/decode_profile.c main/profile_script.c -Imain
    python3 tools/fwtest/run.py mqtt.silkline.id

它不需要板子。板子烧一轮几分钟，而协议层的问题在这里几秒就能暴露。
"""
import json, os, socket, ssl, struct, subprocess, sys, time, urllib.request

HOST = sys.argv[1] if len(sys.argv) > 1 else "mqtt.silkline.id"
# 刻意不用真板子的 dev id：两者会抢同一个 MQTT 会话和服务端 actor 状态，
# 真板子一重连，测试就莫名其妙地收不到派发。联调脚本必须能在真设备在线时
# 随时跑，所以它自己占一个 id。
DEV = "aa00bb11cc22"
SERIAL = "FWTEST-PRINTER-1"
DECODER = os.environ.get("DECODER", "/tmp/decode_profile")
API = f"https://{HOST}:9443/api"

fails = []


def check(cond, what):
    print(("  ✓ " if cond else "  ✗ ") + what)
    if not cond:
        fails.append(what)


def api(path, data=None, hdr=None, raw=False):
    req = urllib.request.Request(API + path, data=data, method="POST" if data else "GET")
    for k, v in (hdr or {}).items():
        req.add_header(k, v)
    with urllib.request.urlopen(req, timeout=20) as r:
        body = r.read()
    return body if raw else json.loads(body)


def decode(mode, payload):
    p = subprocess.run([DECODER, mode], input=payload, capture_output=True, text=True)
    return p.stdout.strip()


# ── 最小 MQTT 3.1.1 客户端。用标准库拼，免得给联调脚本引依赖 ──
def elen(n):
    out = b""
    while True:
        d, n = n % 128, n // 128
        if n:
            d |= 0x80
        out += bytes([d])
        if not n:
            return out


def sf(x):
    return struct.pack(">H", len(x)) + x.encode()


class MQTT:
    def __init__(self, host, user, passwd, cid):
        ctx = ssl.create_default_context()
        self.s = ctx.wrap_socket(socket.create_connection((host, 8883), 10),
                                 server_hostname=host)
        body = (sf("MQTT") + bytes([4, 0xC2]) + struct.pack(">H", 60)
                + sf(cid) + sf(user) + sf(passwd))
        self.s.sendall(bytes([0x10]) + elen(len(body)) + body)
        self.rc = self.s.recv(4)[3]
        self.buf = b""
        self.pending = []
        self.pid = 0

    def _read_packet(self):
        """按 MQTT 报文边界读一整条，粘包时不会串味。"""
        while True:
            if len(self.buf) >= 2:
                i, mult, rl = 1, 1, 0
                ok = True
                while True:
                    if i >= len(self.buf):
                        ok = False
                        break
                    b = self.buf[i]
                    i += 1
                    rl += (b & 127) * mult
                    mult *= 128
                    if not (b & 0x80):
                        break
                if ok and len(self.buf) >= i + rl:
                    pkt = self.buf[:i + rl]
                    self.buf = self.buf[i + rl:]
                    return pkt[0], pkt[i:i + rl]
            d = self.s.recv(4096)
            if not d:
                return None, None
            self.buf += d

    def subscribe(self, topic):
        # packet id 必须每次不同：复用会拿到 0x91（Packet Identifier in use），
        # 那看着也像「被拒」，但拒的原因不是 ACL——测试会假装通过。
        self.pid += 1
        body = (struct.pack(">H", self.pid) + struct.pack(">H", len(topic))
                + topic.encode() + bytes([1]))
        self.s.sendall(bytes([0x82]) + elen(len(body)) + body)
        # SUBACK 可能跟在别的报文后面（比如 retain 的 profile），得按边界找
        self.s.settimeout(8)
        for _ in range(8):
            t, payload = self._read_packet()
            if t is None:
                return -1
            if (t >> 4) == 9:            # SUBACK
                return payload[-1]       # 返回码，0x80 = 被拒
            if (t >> 4) == 3:            # 中间夹的 PUBLISH，留着待会用
                self.pending.append(payload)
        return -1

    def publish(self, topic, payload):
        body = struct.pack(">H", len(topic)) + topic.encode() + payload.encode()
        self.s.sendall(bytes([0x30]) + elen(len(body)) + body)

    def wait(self, want_suffix, timeout=8):
        """收一条 PUBLISH，返回 (topic, payload)。"""
        self.s.settimeout(timeout)
        deadline = time.time() + timeout
        while time.time() < deadline:
            try:
                d = self.s.recv(4096)
            except socket.timeout:
                return None, None
            if not d:
                return None, None
            self.buf += d
            while self.buf:
                if (self.buf[0] >> 4) != 3:
                    self.buf = b""
                    break
                i, mult, rl = 1, 1, 0
                try:
                    while True:
                        b = self.buf[i]
                        i += 1
                        rl += (b & 127) * mult
                        mult *= 128
                        if not (b & 0x80):
                            break
                except IndexError:
                    break
                if len(self.buf) < i + rl:
                    break
                tl = struct.unpack(">H", self.buf[i:i + 2])[0]
                topic = self.buf[i + 2:i + 2 + tl].decode()
                payload = self.buf[i + 2 + tl:i + rl]
                if (self.buf[0] >> 1) & 3:
                    payload = payload[2:]
                self.buf = self.buf[i + rl:]
                if topic.endswith(want_suffix):
                    return topic, payload
        return None, None

    def close(self):
        self.s.close()


print(f"联调目标 {HOST}\n")

print("1. 登录与 enroll")
api("/auth/sms", json.dumps({"phone": "13800000000"}).encode(),
    {"Content-Type": "application/json"})
tok = api("/auth/verify", json.dumps(
    {"phone": "13800000000", "code": "424242", "device": "fwtest"}).encode(),
    {"Content-Type": "application/json"})["token"]
devkey = api("/device/enroll", json.dumps({"dev": DEV}).encode(),
             {"Content-Type": "application/json",
              "Authorization": "Bearer " + tok})["device_key"]
check(bool(tok and devkey), "拿到 session token 与 device key")

print("2. 设备连 MQTT 并订阅")
m = MQTT(HOST, DEV, devkey, DEV)
check(m.rc == 0, f"CONNACK rc={m.rc}")
check(m.subscribe(f"printer/{DEV}/job") < 0x80, "订阅 job")
check(m.subscribe(f"printer/{DEV}/profile") < 0x80, "订阅 profile")
rc_other = m.subscribe("printer/aaaaaaaaaaaa/job")
# 0x80 = ACL 拒绝。别用 >=0x80 兜底：0x91 这类「packet id 复用」也 >=0x80，
# 会让测试在根本没验 ACL 的情况下变绿。
check(rc_other == 0x80, f"订阅别人的 topic 被 ACL 拒绝（SUBACK 0x{rc_other:02x}）")

print("3. 上报机型档案，等服务端下发怪癖档案")
api(f"/device/{DEV}/ident", json.dumps({
    "serial": SERIAL, "vid": "03F0", "pid": "F22A", "make": "HP",
    "model": "HP Laser MFP 136a", "cmd": "URF,PCL,PJL,PWGRaster",
    "urf_caps": "CP1,IS1-4-5-19,MT1-2-3-4-5-6,RS600,V1.4,W8",
}).encode(), {"Content-Type": "application/json",
              "Authorization": "Bearer " + devkey})
topic, prof = m.wait("/profile")
check(prof is not None, "收到 profile")
if prof:
    check(len(prof) <= 1024, f"档案 {len(prof)} 字节，未超 1KB 上限")
    out = decode("profile", prof.decode())
    print("     固件解析结果：")
    for line in out.splitlines():
        print("       " + line)
    check(not out.startswith("ERR"), "固件解析器能解开线上下发的档案")
    check("send_hex len=9 bytes=1b252d313233343558" in out,
          "job_end 里是那 9 个字节的 UEL")

print("4. 心跳，然后上传一份作业")
m.publish(f"printer/{DEV}/status", json.dumps({
    "dev": DEV, "job": "", "state": "ready", "bytes": 0, "serial": SERIAL,
    "prn": {"code": 10001, "display": "Ready", "online": True,
            "asleep": False, "paper_out": False, "error": False},
}))
time.sleep(0.5)
urf = (b"UNIRAST\x00" + struct.pack(">I", 1) + b"\x00" * 12
       + struct.pack(">II", 4962, 7014) + b"\x00" * 12 + b"\x00" * 512)
res = api("/print", urf, {"Content-Type": "image/urf", "X-Device": DEV,
                          "X-Printer-Serial": SERIAL,
                          "Authorization": "Bearer " + tok})
check(res.get("state") == "queued", f"作业入队 job={res.get('job')}")

print("5. 收派发信令，按信令取件")
# 队列是 FIFO，前面可能还排着别的作业。像真设备那样一件件收，直到轮到我们
# 这件——写死「第一条就是我的」会在队列非空时假失败。
sig = None
for _ in range(10):
    topic, s2 = m.wait("/job")
    if s2 is None:
        break
    j2 = json.loads(s2)
    if j2["id"] == res["job"]:
        sig = s2
        break
    # 不是我们这件：取回并回执，把它让过去（一次只派一件，不回执就不会派下一件）
    api(f"/job/{j2['id']}/data", hdr={"Authorization": "Bearer " + devkey}, raw=True)
    m.publish(f"printer/{DEV}/status", json.dumps({
        "dev": DEV, "job": j2["id"], "state": "done", "bytes": j2["size"],
        "serial": SERIAL}))
check(sig is not None, "收到自己那件的派发信令")
if sig:
    j = json.loads(sig)
    check(len(sig) < 200, f"信令 {len(sig)} 字节——只传几十字节的信令")
    body = api(f"/job/{j['id']}/data", hdr={"Authorization": "Bearer " + devkey}, raw=True)
    check(len(body) == j["size"], f"取回 {len(body)} 字节，与信令声明一致")
    check(body[:8] == b"UNIRAST\x00", "取回的是 URF")

    print("6. 回执，作业应转 done")
    m.publish(f"printer/{DEV}/status", json.dumps({
        "dev": DEV, "job": j["id"], "state": "done", "bytes": len(body),
        "serial": SERIAL, "profile_rev": 1,
    }))
    time.sleep(0.8)
    st = api("/status", hdr={"Authorization": "Bearer " + tok, "X-Device": DEV})
    done = [x for x in st["jobs"] if x["id"] == j["id"]]
    check(done and done[0]["state"] == "done",
          f"服务端记为 {done[0]['state'] if done else '?'}")

print("7. 一次性钩子覆盖：固件能不能解服务端可能下发的变体")
for name, sig_json, expect in [
    ("不发作业结束符", '{"id":"j1","size":1,"hooks":{"job_end":[]}}', "hook job_end n=0"),
    ("没有 hooks 字段", '{"id":"j1","size":1}', "send_hex len=9"),
    ("唤醒序列", '{"id":"j1","size":1,"hooks":{"wake":[{"op":"send_hex","data":"1b25"},'
                 '{"op":"delay_ms","ms":300}]}}', "ms=300"),
]:
    out = decode("hooks", sig_json)
    check(expect in out, f"{name}：{expect}")

m.close()
print()
if fails:
    print(f"{len(fails)} 项失败：")
    for f in fails:
        print("  - " + f)
    sys.exit(1)
print("全部通过")
