/**
 * 按 docs/API-cloud-print.md 第 4 节实现的 mock server。零依赖，node:http。
 *
 * 它有两个用途：给契约测试当对手方，以及给开发期的 App 用。
 *
 * 刻意不做的事：
 * - 429 的四道闸只实现「同号码 60 秒间隔」。mock 的作用是验契约不是验限流，
 *   实现四道闸只会让测试变脆。
 * - 不校验上传尺寸。尺寸校验要和 render-profile 联动，放在契约测试里显式构造。
 *
 * 起服务：node mock-server/server.mjs [port]
 * 端口传 0 会随机选一个并把实际端口打到 stdout，测试据此连接。
 */
import {createServer} from 'node:http';
import {createState, defaultDevice, makeToken, makeId12, FIXED_CODE} from './state.mjs';

const SMS_INTERVAL_MS = 60_000;

/**
 * 解 RFC 5987 的 ext-value：`UTF-8''%E6%8A%A5%E5%91%8A.pdf` → `报告.pdf`。
 *
 * 文档 4.4 说 X-Filename 用 RFC 5987 编码，4.6 的 /api/status 返回的 name 是
 * 解码后的原文——所以解码这一步在服务端。漏掉的话用户在作业列表里看到的
 * 就是那一长串百分号。
 */
function decodeRfc5987(raw) {
  if (!raw) return '';
  const m = /^([A-Za-z0-9-]+)'([^']*)'(.*)$/.exec(raw);
  if (!m) {
    // 没按 RFC 5987 编码就原样收下。Node 把 HTTP 头按 latin1 解，
    // 直接塞 UTF-8 字节进来会变成乱码，这里补一次修正。
    return Buffer.from(raw, 'latin1').toString('utf8');
  }
  const [, charset, , value] = m;
  const bytes = Buffer.from(
    value.replace(/%([0-9A-Fa-f]{2})/g, (_, h) => String.fromCharCode(parseInt(h, 16))),
    'latin1',
  );
  return bytes.toString(charset.toLowerCase() === 'utf-8' ? 'utf8' : 'latin1');
}

function normalizePhone(raw) {
  const s = String(raw ?? '').replace(/\s+/g, '').replace(/^\+?86/, '');
  return /^1\d{10}$/.test(s) ? s : null;
}

export function createMockServer(state = createState(), opts = {}) {
  const log = opts.log ?? (() => {});

  const send = (res, status, body) => {
    const text = body === undefined ? '' : JSON.stringify(body);
    res.writeHead(status, {
      'Content-Type': 'application/json',
      'Content-Length': Buffer.byteLength(text),
    });
    res.end(text);
  };
  const err = (res, status, e, detail) =>
    send(res, status, detail === undefined ? {e} : {e, detail});

  /** 返回 session 的 user_id，或 null。 */
  const sessionUser = req => {
    const auth = req.headers.authorization ?? '';
    const m = /^Bearer (.+)$/.exec(auth);
    return m ? (state.sessions.get(m[1]) ?? null) : null;
  };

  /** 取出 X-Device 指定的设备，并确认属于当前用户。 */
  const ownedDevice = (req, userId, devId) => {
    const dev = devId ?? req.headers['x-device'];
    if (!dev) return {error: [400, 'bad request']};
    const d = state.devices.get(dev);
    if (!d) return {error: [404, 'not found']};
    if (d.user_id !== userId) return {error: [403, 'forbidden']};
    return {device: d};
  };

  const readBody = req =>
    new Promise(resolve => {
      const chunks = [];
      req.on('data', c => chunks.push(c));
      req.on('end', () => resolve(Buffer.concat(chunks)));
    });

  const server = createServer(async (req, res) => {
    const url = new URL(req.url, 'http://mock');
    const path = url.pathname;
    const raw = await readBody(req);
    let body;
    try {
      body = raw.length && req.headers['content-type']?.includes('json')
        ? JSON.parse(raw.toString('utf8'))
        : undefined;
    } catch {
      return err(res, 400, 'bad request');
    }

    // ---- 4.1b 认证：无需身份 ----

    if (path === '/api/auth/sms' && req.method === 'POST') {
      const phone = normalizePhone(body?.phone);
      if (!phone) return err(res, 400, 'bad request');
      const last = state.smsSentAt.get(phone) ?? 0;
      if (Date.now() - last < SMS_INTERVAL_MS)
        return err(res, 429, 'rate limited', '同号码 60 秒间隔');
      state.smsSentAt.set(phone, Date.now());
      log(`短信验证码 ${phone} -> ${FIXED_CODE}`);
      return send(res, 200, {ok: 1, ttl: 300});
    }

    if (path === '/api/auth/verify' && req.method === 'POST') {
      const phone = normalizePhone(body?.phone);
      if (!phone) return err(res, 400, 'bad request');
      if (body?.code !== FIXED_CODE) return err(res, 401, 'unauthorized');
      state.smsSentAt.delete(phone);   // 用过一次即作废
      let user = state.users.get(phone);
      const newUser = !user;
      if (!user) {
        user = {user_id: makeId12() + makeId12(), phone};
        state.users.set(phone, user);
      }
      const token = makeToken();
      state.sessions.set(token, user.user_id);
      return send(res, 200, {
        token,
        user_id: user.user_id,
        phone_tail: phone.slice(-4),
        new_user: newUser,
      });
    }

    // ---- 以下都要 app 角色 ----

    const userId = sessionUser(req);
    if (!userId) return err(res, 401, 'unauthorized');

    if (path === '/api/auth/logout' && req.method === 'POST') {
      if (url.searchParams.get('all') === '1') {
        for (const [t, u] of state.sessions) if (u === userId) state.sessions.delete(t);
      } else {
        const m = /^Bearer (.+)$/.exec(req.headers.authorization ?? '');
        if (m) state.sessions.delete(m[1]);
      }
      return send(res, 200, {ok: 1});
    }

    if (path === '/api/account/delete' && req.method === 'POST') {
      let jobsDeleted = 0;
      for (const [id, j] of state.jobs) {
        const d = state.devices.get(j.dev);
        if (d?.user_id === userId) { state.jobs.delete(id); ++jobsDeleted; }
      }
      for (const [dev, d] of state.devices) if (d.user_id === userId) state.devices.delete(dev);
      for (const [t, u] of state.sessions) if (u === userId) state.sessions.delete(t);
      return send(res, 200, {ok: 1, jobs_deleted: jobsDeleted});
    }

    if (path === '/api/device/enroll' && req.method === 'POST') {
      const dev = String(body?.dev ?? '');
      // dev 可选：不带就签发一把待认领的密钥。绑定发生在设备首次连 MQTT 时
      // ——服务端从设备自报的 username 学到 MAC（文档 4.1b）。
      // 这正是「配网只连一次热点」的前提：App 不需要先去问设备要 MAC。
      if (dev === '') {
        const key = makeToken();
        state.deviceKeys.set(key, '');   // 待认领
        state.unclaimed = state.unclaimed ?? new Map();
        state.unclaimed.set(key, {user_id: userId, name: String(body?.name ?? '')});
        return send(res, 200, {device_key: key, dev: '', reset: false});
      }
      if (!/^[0-9a-f]{12}$/.test(dev)) return err(res, 400, 'bad request');
      const existing = state.devices.get(dev);
      if (existing && existing.user_id !== userId) return err(res, 409, 'device bound');
      const reset = Boolean(existing);
      const d = existing ?? defaultDevice(dev, String(body?.name ?? ''), userId);
      if (existing && body?.name) d.name = String(body.name);
      if (existing?.key) state.deviceKeys.delete(existing.key);   // 旧密钥吊销
      d.key = makeToken();
      state.deviceKeys.set(d.key, dev);
      state.devices.set(dev, d);
      return send(res, 200, {device_key: d.key, dev, reset});
    }

    // 4.5b 设备列表。一台都没有时返回 {"devices":[]}，不是 404。
    if (path === '/api/devices' && req.method === 'GET') {
      const devices = [...state.devices.values()].filter(d => d.user_id === userId);
      return send(res, 200, {devices: devices.map(toDeviceListItem)});
    }

    // 4.1b 解绑。设备本来就没绑定时也返回 200（幂等）。
    let m = /^\/api\/device\/([0-9a-f]{12})\/unbind$/.exec(path);
    if (m && req.method === 'POST') {
      const d = state.devices.get(m[1]);
      if (d && d.user_id !== userId) return err(res, 403, 'forbidden');
      if (d) {
        if (d.key) state.deviceKeys.delete(d.key);
        state.devices.delete(d.dev);
      }
      return send(res, 200, {ok: 1});
    }

    // 4.5 光栅参数。设备从未上报过 ident 时 404——App 不能用默认值蒙。
    m = /^\/api\/device\/([0-9a-f]{12})\/render-profile$/.exec(path);
    if (m && req.method === 'GET') {
      const {device, error} = ownedDevice(req, userId, m[1]);
      if (error) return err(res, error[0], error[1]);
      if (!device.printer) return err(res, 404, 'not found');
      return send(res, 200, {
        dev: device.dev,
        serial: device.printer.serial,
        ...device.renderProfile,
      });
    }

    // 4.7 打印机完整信息
    m = /^\/api\/device\/([0-9a-f]{12})\/printer$/.exec(path);
    if (m && req.method === 'GET') {
      const {device, error} = ownedDevice(req, userId, m[1]);
      if (error) return err(res, error[0], error[1]);
      if (!device.printer) return err(res, 404, 'not found');
      const p = device.printer;
      return send(res, 200, {
        printer: {
          serial: p.serial, vid: p.vid, pid: p.pid, make: p.make, model: p.model,
          cmd: p.cmd, urf_caps: p.urf_caps, proto: p.proto,
          first_seen: p.first_seen, last_seen: p.last_seen,
        },
        // 档案是可编排的动作序列（文档 3.7b），不是布尔开关表。
        // 文档 4.7 里画的那个形状是 v1 的遗留，真服务端返回的是这个。
        profile: {
          rev: 2, serial: p.serial, src: 'model',
          flags: {unidir: false, pjl_ok: true},
          hooks: {
            job_begin: [{op: 'iface_reset'}],
            job_end: [{op: 'send_hex', data: '1b252d313233343558', required: true}],
            wake: [{op: 'send_hex', data: '1b252d313233343558'}, {op: 'delay_ms', ms: 300}],
          },
          margins_mm: [4, 4, 4, 4],
          votes: 3, disputed: false,
        },
      });
    }

    // 4.7b 这个桥见过的所有打印机
    m = /^\/api\/device\/([0-9a-f]{12})\/printers$/.exec(path);
    if (m && req.method === 'GET') {
      const {device, error} = ownedDevice(req, userId, m[1]);
      if (error) return err(res, error[0], error[1]);
      const p = device.printer;
      return send(res, 200, {
        attached: p?.attached ? p.serial : '',
        printers: p ? [{
          serial: p.serial, make: p.make, model: p.model,
          attached: p.attached, profile_src: 'model',
          queued_jobs: [...state.jobs.values()].filter(
            j => j.serial === p.serial && j.state === 'queued').length,
          first_seen: p.first_seen, last_seen: p.last_seen,
        }] : [],
      });
    }

    // 4.8 测试清单
    m = /^\/api\/device\/([0-9a-f]{12})\/tests$/.exec(path);
    if (m && req.method === 'GET') {
      const {device, error} = ownedDevice(req, userId, m[1]);
      if (error) return err(res, error[0], error[1]);
      return send(res, 200, {tests: Object.values(device.tests)});
    }

    // 4.9 开始一轮测试
    m = /^\/api\/device\/([0-9a-f]{12})\/tests\/([a-z_]+)\/start$/.exec(path);
    if (m && req.method === 'POST') {
      const {device, error} = ownedDevice(req, userId, m[1]);
      if (error) return err(res, error[0], error[1]);
      const t = device.tests[m[2]];
      if (!t) return err(res, 404, 'not found');
      if (Object.values(device.tests).some(x => x.state === 'running'))
        return err(res, 409, 'test running');
      t.state = 'running';
      const runId = makeId12();
      state.testRuns.set(runId, {dev: device.dev, test: t.id});
      return send(res, 200, {
        run_id: runId,
        jobs_needed: t.jobs_needed,
        variant: t.id === 'double_print' ? {uel_job_end: false} : {},
        baseline_ok: true,
        instruction: `接下来会打印 ${t.jobs_needed} 页。请把打印机纸盒装好，然后看实际出了几张纸。`,
      });
    }

    // 4.10 提交用户判断
    m = /^\/api\/tests\/([0-9a-f]{12})\/answer$/.exec(path);
    if (m && req.method === 'POST') {
      const run = state.testRuns.get(m[1]);
      if (!run) return err(res, 404, 'not found');
      const device = state.devices.get(run.dev);
      if (!device || device.user_id !== userId) return err(res, 403, 'forbidden');
      const verdict = body?.verdict;
      if (!['pass', 'fail', 'unclear', 'aborted'].includes(verdict))
        return err(res, 400, 'bad request');
      device.tests[run.test].state =
        verdict === 'pass' ? 'passed' : verdict === 'fail' ? 'failed' : 'unclear';
      state.testRuns.delete(m[1]);
      return send(res, 200, {
        applied: verdict === 'fail' ? {uel_job_end: true} : {},
        scope: 'serial', votes: 1, promoted: false,
        message: '已针对你的打印机保存。再有 2 台同型号得出相同结论后，将成为该机型的默认配置。',
      });
    }

    // ---- 开发专用，不在文档的契约里 ----
    //
    // 驱动 UI 的三种链路状态用。契约测试不许碰这个端点——它一旦被测试依赖，
    // mock 和真服务端的分歧就会被这层便利掩盖掉。
    m = /^\/api\/dev\/device\/([0-9a-f]{12})\/state$/.exec(path);
    if (m && req.method === 'POST') {
      const {device, error} = ownedDevice(req, userId, m[1]);
      if (error) return err(res, error[0], error[1]);
      if (typeof body?.online === 'boolean') {
        device.online = body.online;
        device.state = body.online ? 'ready' : 'offline';
        if (body.online) device.seen = Math.floor(Date.now() / 1000);
      }
      // printer:true 造「插上了」，printer:null 造「拔掉了」。
      if (body?.printer === null) device.printer = null;
      else if (body?.printer === true) device.printer = {...device.printerWhenAttached};
      else if (typeof body?.attached === 'boolean' && device.printer)
        device.printer.attached = body.attached;
      if (typeof body?.queued_jobs === 'number') {
        for (let i = 0; i < body.queued_jobs; ++i) {
          const id = makeId12();
          state.jobs.set(id, {
            id, dev: device.dev, serial: device.printer?.serial ?? 'X',
            name: `文档${i + 1}.pdf`, size: 284160, state: 'queued', bytes: 0, err: '',
            created: Math.floor(Date.now() / 1000), updated: Math.floor(Date.now() / 1000),
          });
        }
      }
      return send(res, 200, {ok: 1});
    }

    // 4.6 查设备与作业。最多最近 15 件。
    if (path === '/api/status' && req.method === 'GET') {
      const {device, error} = ownedDevice(req, userId);
      if (error) return err(res, error[0], error[1]);
      // created 是秒级的，同一秒入队的作业排序会不确定。用入队序号做次级键——
      // 真实数据库里对应的是主键。不这么做，「最近作业」的顺序每次刷新都可能变。
      const jobs = [...state.jobs.values()]
        .filter(j => j.dev === device.dev)
        .sort((a, b) => b.created - a.created || b.seq - a.seq)
        .slice(0, 15)
        .map(({dev: _dev, serial: _serial, seq: _seq, ...rest}) => rest);
      // 字段跟真服务端对齐：它返回 serial，不返回 seen/state。
      return send(res, 200, {
        device: {
          dev: device.dev, online: device.online,
          serial: device.printer?.serial ?? '',
          prn: device.online ? device.prn : null,
        },
        jobs,
      });
    }

    // 4.4 上传作业。服务端不渲染，只校验、排队、转发。
    if (path === '/api/print' && req.method === 'POST') {
      const {device, error} = ownedDevice(req, userId);
      if (error) return err(res, error[0], error[1]);

      const ct = req.headers['content-type'] ?? '';
      const magic = ct === 'image/urf' ? Buffer.from('UNIRAST\0', 'binary')
                  : ct === 'image/pwg-raster' ? Buffer.from('RaS2', 'binary')
                  : null;
      if (!magic) return err(res, 415, 'unsupported media type');

      const serial = req.headers['x-printer-serial'];
      if (!serial) return err(res, 400, 'bad request', 'X-Printer-Serial 必填');

      if (raw.length > 209715200) return err(res, 413, 'too large');
      if (raw.length < magic.length || !raw.subarray(0, magic.length).equals(magic))
        return err(res, 400, 'bad raster',
          `魔数不匹配：期望 ${ct === 'image/urf' ? 'UNIRAST\\0' : 'RaS2'}，实际 ` +
          raw.subarray(0, magic.length).toString('binary'));

      // URF 头第 9~12 字节是大端页数。写 0 打印机认为文档为空，什么都不打。
      let pages = 0;
      if (ct === 'image/urf') {
        if (raw.length < 12) return err(res, 400, 'bad raster', '文件不足 12 字节');
        pages = raw.readUInt32BE(8);
        if (pages === 0) return err(res, 400, 'bad raster', '页数字段为 0');
      }

      const id = makeId12();
      state.jobSeq = (state.jobSeq ?? 0) + 1;
      const attached = Boolean(device.printer?.attached) && device.printer.serial === serial;
      state.jobs.set(id, {
        id, dev: device.dev, serial, seq: state.jobSeq,
        name: decodeRfc5987(req.headers['x-filename']),
        size: raw.length, state: 'queued', bytes: 0, err: '',
        created: Math.floor(Date.now() / 1000),
        updated: Math.floor(Date.now() / 1000),
        test_run: req.headers['x-test-run'] ?? '',
      });
      return send(res, 200, {
        job: id, size: raw.length, pages, state: 'queued',
        printer_attached: attached,
      });
    }

    return err(res, 404, 'not found');
  });

  function toDeviceListItem(d) {
    return {
      dev: d.dev, name: d.name, online: d.online, seen: d.seen,
      state: d.state, bound: d.bound,
      printer: d.printer
        ? {serial: d.printer.serial, make: d.printer.make,
           model: d.printer.model, attached: d.printer.attached}
        : null,
      queued_jobs: [...state.jobs.values()].filter(
        j => j.dev === d.dev && j.state === 'queued').length,
    };
  }

  return {server, state};
}

// 直接运行时起服务。端口传 0 会随机选一个并把实际端口打到 stdout。
if (import.meta.url === `file://${process.argv[1]}`) {
  const port = Number(process.argv[2] ?? 9443);
  const {server} = createMockServer(createState(), {log: console.log});
  server.listen(port, () => {
    console.log(`mock server on http://127.0.0.1:${server.address().port}/api`);
    console.log(`短信验证码固定为 ${FIXED_CODE}`);
  });
}
