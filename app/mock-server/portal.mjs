/**
 * 假的配网门户，按 main/provision.c 的行为写。给门户协议客户端当对手方。
 *
 * 它刻意复刻了三个真实行为，那三个才是客户端会栽跟头的地方：
 *
 * 1. 验证通过后设备 3 秒后 esp_restart()——热点消失，/status 从此连不上。
 *    「连接断掉」既可能是成功也可能是失败，客户端不能猜。
 * 2. /status 是轮询出来的，st 从 0 变 1 只有 3 秒窗口，错过就再也读不到。
 * 3. 老固件不返回 dev 字段（见 API 文档 5.0b 的缺口）。
 *
 * 用法：node mock-server/portal.mjs [port] [--no-dev] [--fail] [--reboot-ms N]
 * 端口传 0 会随机选并把实际端口打到 stdout。
 */
import {createServer} from 'node:http';

export function createPortal(opts = {}) {
  const {
    dev = 'f412fa87c9e0',        // 传 null 模拟老固件
    verdict = 'ok',              // 'ok' | 'fail'
    failReason = 'AUTH_FAIL(202)',
    verifyMs = 300,              // 多久出结果
    rebootMs = 3000,             // 出结果后多久重启（固件是 3 秒）
    networks = [
      {s: '办公室 5G', r: -42, k: 1},
      {s: 'ChinaNet-8x2k', r: -71, k: 1},
      {s: 'Guest', r: -80, k: 0},
    ],
  } = opts;

  const state = {st: 0, ip: '', e: '', dead: false, connectCalls: 0, lastBody: null};

  const send = (res, body, status = 200) => {
    const text = JSON.stringify(body);
    res.writeHead(status, {
      'Content-Type': 'application/json',
      'Content-Length': Buffer.byteLength(text),
    });
    res.end(text);
  };

  const server = createServer((req, res) => {
    // 重启之后什么都不回——热点已经没了。
    if (state.dead) {
      req.socket.destroy();
      return;
    }

    const path = req.url.split('?')[0];

    // 只给测试用的探针。真固件没有这个端点——它不参与协议，
    // 只是让测试能看到设备收到了什么，省得为此打桩 fetch。
    if (path === '/__probe') {
      return send(res, {connectCalls: state.connectCalls, lastBody: state.lastBody});
    }

    if (path === '/scan' && req.method === 'GET') return send(res, networks);

    if (path === '/status' && req.method === 'GET') {
      const body = {st: state.st, ip: state.ip, e: state.e};
      // 老固件没有这个字段。App 不许猜，见 API 文档 5.0b。
      if (dev) body.dev = dev;
      return send(res, body);
    }

    if (path === '/connect' && req.method === 'POST') {
      let raw = '';
      req.on('data', c => (raw += c));
      req.on('end', () => {
        state.connectCalls++;
        let b = {};
        try {
          b = JSON.parse(raw);
        } catch {
          /* 固件用的是极简 JSON 取值，解不出来就当空 */
        }
        state.lastBody = b;
        if (!b.s) return send(res, {ok: 0});

        state.st = 0;
        state.e = '';
        state.ip = '';
        setTimeout(() => {
          if (verdict === 'ok') {
            state.st = 1;
            state.ip = '192.168.1.37';
            // 固件：验证通过 → 3 秒后 esp_restart()。热点随之消失。
            setTimeout(() => (state.dead = true), rebootMs);
          } else {
            state.st = 2;
            state.e = failReason;
          }
        }, verifyMs);

        return send(res, {ok: 1});
      });
      return;
    }

    // 各平台联网检测地址都被重定向到配网页
    res.writeHead(302, {Location: 'http://192.168.4.1/'});
    res.end();
  });

  return {server, state};
}

if (import.meta.url === `file://${process.argv[1]}`) {
  const args = process.argv.slice(2);
  const port = Number(args.find(a => /^\d+$/.test(a)) ?? 8888);
  const num = name => {
    const a = args.find(x => x.startsWith(`--${name}=`));
    return a ? Number(a.split('=')[1]) : undefined;
  };
  const {server} = createPortal({
    dev: args.includes('--no-dev') ? null : undefined,
    verdict: args.includes('--fail') ? 'fail' : 'ok',
    rebootMs: num('reboot-ms'),
    verifyMs: num('verify-ms'),
  });
  server.listen(port, () => {
    console.log(`fake portal on http://127.0.0.1:${server.address().port}`);
  });
}
