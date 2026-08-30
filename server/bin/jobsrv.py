#!/usr/bin/env python3
"""云打印作业服务。

职责三件：
  1. 收文件（网页上传）→ 用打印机自己的 PPD 渲染成 URF
  2. 经 MQTT 通知设备「有活了」（只发几十字节信令）
  3. 设备回来用 HTTPS 流式取文档（TCP 自带反压，设备内存恒定）

刻意不把文档塞进 MQTT：一份作业 100~500KB，而设备可用堆只有几十 KB，
MQTT 消息必须整包进内存，塞不下。
"""
import os, re, json, uuid, subprocess, threading, time, sqlite3
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import paho.mqtt.client as mqtt

ROOT     = '/opt/stickbox'
JOBS     = f'{ROOT}/jobs'
IDENTS   = f'{ROOT}/idents'             # 设备上报的机型档案，兼容性库的原料
DB       = f'{ROOT}/jobs.db'
RENDER   = f'{ROOT}/bin/render.py'
LISTEN   = ('0.0.0.0', 9443)            # 直接 TLS 对外（443/8443 被 xray 占用）
# 口令从 config.json 读（不入库），照 config.example.json 填。
CONF = json.load(open(os.environ.get('STICKBOX_CONF', f'{ROOT}/config.json')))
MQTT_HOST, MQTT_PORT = CONF.get('mqtt_host', '127.0.0.1'), CONF.get('mqtt_port', 1883)
MQTT_USER, MQTT_PASS = CONF['mqtt_user'], CONF['mqtt_pass']
CERT_DIR = CONF.get('cert_dir', '/etc/letsencrypt/live/mqtt.silkline.id')

def db():
    c = sqlite3.connect(DB, timeout=10)
    c.execute('''CREATE TABLE IF NOT EXISTS jobs(
        id TEXT PRIMARY KEY, dev TEXT, name TEXT, size INT,
        state TEXT, bytes INT DEFAULT 0, created INT, updated INT)''')
    return c

# ── 设备在线状态 ──
devices = {}
lock = threading.Lock()

def on_connect(c, u, f, rc, props=None):
    c.subscribe('printer/+/status', 1)
    print('[mqtt] 已连接 broker', flush=True)

def on_message(c, u, m):
    try:
        d = json.loads(m.payload.decode())
    except Exception:
        return
    dev = d.get('dev') or m.topic.split('/')[1]
    with lock:
        devices[dev] = {**d, 'seen': int(time.time())}
    print(f"[dev] {dev}: {d.get('state')} {d.get('bytes','')}", flush=True)
    if d.get('job') and d.get('state') in ('done', 'failed'):
        con = db()
        con.execute('UPDATE jobs SET state=?, bytes=?, updated=? WHERE id=?',
                    (d['state'], d.get('bytes', 0), int(time.time()), d['job']))
        con.commit(); con.close()
        _pub.pop(d['job'], None)
    # 设备一报到就看队列——离线期间攒下的活在这里续上
    if d.get('state') in ('ready', 'done', 'failed'):
        threading.Thread(target=pump, args=(dev,), daemon=True).start()

mq = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id='jobsrv')
mq.username_pw_set(MQTT_USER, MQTT_PASS)
mq.on_connect, mq.on_message = on_connect, on_message
mq.connect(MQTT_HOST, MQTT_PORT, 60)
mq.loop_start()

def pick_device():
    """挑一台在线设备；只有一台就直接用"""
    with lock:
        alive = [k for k, v in devices.items()
                 if v.get('state') != 'offline' and time.time() - v['seen'] < 120]
    return alive[0] if alive else None

def target_device():
    """没有在线设备时，把作业挂到「最近见过的那台」。
    单桥场景下就是它；将来多设备必须靠账号绑定来定，不能靠猜。"""
    with lock:
        if devices:
            return max(devices.items(), key=lambda kv: kv[1]['seen'])[0]
    con = db()
    r = con.execute("SELECT dev FROM jobs WHERE dev<>'' ORDER BY created DESC LIMIT 1").fetchone()
    con.close()
    return r[0] if r else None

_pub = {}          # jid -> 上次派发时间，防止重复轰炸设备

def pump(dev=None):
    """给设备派一件活，返回是否派出去了。

    一次只派一件：设备可用堆只有几十 KB，也没有本地队列，
    一次性堆过去等于丢件。队列的真相全在服务端的 jobs 表里。"""
    dev = dev or pick_device()
    if not dev or dev != pick_device():
        return False                       # 目标设备不在线，作业老实待在队列里
    now = int(time.time())
    con = db()
    # 正在传的那件还活着就别插队；卡死超 180 秒的退回队列重传
    busy = False
    for jid, up in con.execute(
            "SELECT id,updated FROM jobs WHERE dev=? AND state='downloading'", (dev,)).fetchall():
        if now - up < 180:
            busy = True
        else:
            con.execute("UPDATE jobs SET state='queued',updated=? WHERE id=?", (now, jid))
            _pub.pop(jid, None)
            print(f'[pump] {jid} 传输超时，退回队列', flush=True)
    con.commit()
    if busy:
        con.close(); return False
    row = con.execute("SELECT id,size FROM jobs WHERE state='queued' AND dev IN (?,'') "
                      "ORDER BY created LIMIT 1", (dev,)).fetchone()
    if not row:
        con.close(); return False
    jid, size = row
    con.execute('UPDATE jobs SET dev=?,updated=? WHERE id=?', (dev, now, jid))
    con.commit(); con.close()
    if now - _pub.get(jid, 0) <= 120:
        return True                        # 刚派过，等设备反应，别重发
    _pub[jid] = now
    mq.publish(f'printer/{dev}/job', json.dumps({'id': jid, 'size': size}), qos=1)
    print(f'[pump] 派发 {jid} -> {dev}', flush=True)
    return True

PAGE = open(f'{ROOT}/web/index.html', 'rb').read() if os.path.exists(f'{ROOT}/web/index.html') else b'<h1>upload page missing</h1>'

class H(BaseHTTPRequestHandler):
    protocol_version = 'HTTP/1.1'
    def log_message(self, *a): pass

    def _send(self, code, body=b'', ctype='application/json', extra=None):
        self.send_response(code)
        self.send_header('Content-Type', ctype)
        self.send_header('Content-Length', str(len(body)))
        for k, v in (extra or {}).items(): self.send_header(k, v)
        self.end_headers()
        if body: self.wfile.write(body)

    def do_GET(self):
        p = self.path.split('?')[0]
        if p in ('/', '/api/', '/api'):
            return self._send(200, open(f'{ROOT}/web/index.html','rb').read(),
                              'text/html; charset=utf-8')
        if p == '/api/status':
            with lock: snap = dict(devices)
            con = db()
            rows = con.execute('SELECT id,name,size,state,bytes,created FROM jobs '
                               'ORDER BY created DESC LIMIT 15').fetchall()
            con.close()
            body = json.dumps({'devices': snap, 'jobs': [
                {'id': r[0], 'name': r[1], 'size': r[2], 'state': r[3],
                 'bytes': r[4], 'created': r[5]} for r in rows]},
                ensure_ascii=False).encode()
            return self._send(200, body)
        if p.startswith('/api/job/') and p.endswith('/data'):
            jid = p.split('/')[3]
            f = f'{JOBS}/{jid}.urf'
            if not os.path.exists(f): return self._send(404, b'{"e":"no job"}')
            n = os.path.getsize(f)
            self.send_response(200)
            self.send_header('Content-Type', 'application/octet-stream')
            self.send_header('Content-Length', str(n))
            self.end_headers()
            with open(f, 'rb') as fh:          # 流式发送，服务端也不整包进内存
                while True:
                    b = fh.read(8192)
                    if not b: break
                    self.wfile.write(b)
            con = db(); con.execute('UPDATE jobs SET state=?, updated=? WHERE id=?',
                                    ('downloading', int(time.time()), jid))
            con.commit(); con.close()
            return
        return self._send(404, b'{"e":"not found"}')

    def do_POST(self):
        p = self.path.split('?')[0]

        # 设备上报机型档案。设备每次插上打印机报一次，几 KB 到十几 KB。
        # 每台设备留一份 latest.json，另外按时间戳存历史——机型档案会随
        # 固件探针的改进而变化，覆盖掉就没法比对了。
        m = re.match(r'^/api/device/([0-9a-f]{6,32})/ident$', p)
        if m:
            dev = m.group(1)
            n = int(self.headers.get('Content-Length', 0))
            if n <= 0 or n > 256 * 1024:
                return self._send(400, b'{"e":"bad size"}')
            raw = self.rfile.read(n)
            try:
                obj = json.loads(raw)
            except Exception as e:
                print(f'[ident] {dev} JSON 非法: {e}', flush=True)
                return self._send(400, b'{"e":"bad json"}')
            obj['_dev'] = dev
            obj['_ts'] = int(time.time())
            d = f'{IDENTS}/{dev}'
            os.makedirs(d, exist_ok=True)

            # 设备内存只够开一条额外 TLS，握着大缓冲做 HTTPS 会在证书验签
            # 那一步分不到内存。所以档案是分两趟小载荷传上来的：
            # 第 0 层（USB 描述符）整份替换，带 _part 的（PJL 探针）合并进去。
            part = obj.pop('_part', None)
            latest = f'{d}/latest.json'
            if part and os.path.exists(latest):
                try:
                    base = json.load(open(latest))
                except Exception:
                    base = {}
                base.update(obj)
                obj = base
            body = json.dumps(obj, ensure_ascii=False, indent=1).encode()
            open(f'{d}/{obj["_ts"]}.json', 'wb').write(body)
            open(latest, 'wb').write(body)
            mdl = (obj.get('printer_class') or {}).get('model', '?')
            print(f'[ident] {dev} 机型档案 {n} 字节 part={part or "base"} '
                  f'型号={mdl}', flush=True)
            return self._send(200, b'{"ok":1}')

        if p != '/api/print':
            return self._send(404, b'{"e":"not found"}')
        n = int(self.headers.get('Content-Length', 0))
        if n <= 0 or n > 50 * 1024 * 1024:
            return self._send(400, b'{"e":"bad size"}')
        # HTTP 头按 latin-1 解码，中文文件名要转回 UTF-8，否则列表里全是乱码
        name = self.headers.get('X-Filename', 'document')
        try:    name = name.encode('latin-1').decode('utf-8')
        except Exception: pass
        raw = self.rfile.read(n)

        jid = uuid.uuid4().hex[:12]
        src = f'{JOBS}/{jid}.src'
        open(src, 'wb').write(raw)
        try:
            out = subprocess.run(['python3', RENDER, src, f'{JOBS}/{jid}.urf'],
                                 capture_output=True, timeout=180)
            if out.returncode:
                raise RuntimeError(out.stderr.decode()[:200])
        except Exception as e:
            return self._send(500, json.dumps({'e': f'渲染失败: {e}'},
                                              ensure_ascii=False).encode())
        finally:
            try: os.remove(src)
            except OSError: pass

        size = os.path.getsize(f'{JOBS}/{jid}.urf')
        # 设备离线不丢件：渲染好的 URF 留在盘上，一律入队，
        # 等设备回来由 pump() 续上。
        dev = pick_device() or target_device() or ''
        con = db()
        con.execute('INSERT INTO jobs VALUES(?,?,?,?,?,?,?,?)',
                    (jid, dev, name, size, 'queued', 0,
                     int(time.time()), int(time.time())))
        con.commit(); con.close()
        sent = pump(dev) if dev else False
        return self._send(200, json.dumps(
            {'job': jid, 'size': size, 'dev': dev, 'queued': not sent},
            ensure_ascii=False).encode())

if __name__ == '__main__':
    import ssl
    os.makedirs(JOBS, exist_ok=True)
    os.makedirs(IDENTS, exist_ok=True)
    srv = ThreadingHTTPServer(LISTEN, H)
    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    ctx.load_cert_chain(f'{CERT_DIR}/fullchain.pem', f'{CERT_DIR}/privkey.pem')
    srv.socket = ctx.wrap_socket(srv.socket, server_side=True)
    con = db()
    n = con.execute("UPDATE jobs SET state='queued' WHERE state='no-device'").rowcount
    con.commit(); con.close()
    if n: print(f'[srv] {n} 件旧的 no-device 作业已转入队列', flush=True)

    def sweeper():
        while True:
            time.sleep(20)
            try: pump()
            except Exception as e: print('[pump] 异常', e, flush=True)
    threading.Thread(target=sweeper, daemon=True).start()

    print(f'[srv] TLS 监听 {LISTEN}', flush=True)
    srv.serve_forever()
