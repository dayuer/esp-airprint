// mock 的内存状态。进程退出即丢，不落盘。
import {randomBytes} from 'node:crypto';

export const FIXED_CODE = '123456';

/** `{key_id}.{secret}`：12 位十六进制 + 32 位 Base64URL（文档 1.1）。 */
export function makeToken() {
  const keyId = randomBytes(6).toString('hex');
  const secret = randomBytes(24).toString('base64url').slice(0, 32);
  return `${keyId}.${secret}`;
}

export function makeId12() {
  return randomBytes(6).toString('hex');
}

export function createState() {
  return {
    users: new Map(),        // phone -> {user_id, phone}
    sessions: new Map(),     // token -> user_id
    smsSentAt: new Map(),    // phone -> ms，只实现「同号码 60 秒间隔」这一道闸
    devices: new Map(),      // dev -> {dev, name, user_id, key, bound, ...}
    deviceKeys: new Map(),   // device key -> dev
    jobs: new Map(),         // job id -> job
    testRuns: new Map(),     // run_id -> {dev, test}
  };
}

/**
 * 一台设备的默认形态。数值取自文档 4.5 / 4.7 的示例，逐字段抄，
 * 不按 client 的方便改——mock 编得顺手，契约测试就什么都证明不了。
 *
 * **刚 enroll 的设备是离线的、没插打印机的。**
 *
 * 这里一度默认 online:true 并硬塞一台 HP 打印机，结果 App 里那条连接线
 * 对每台设备都显示「全通」——而真设备当时连 MQTT 被拒、根本没上线。
 * 连接线是这个 App 的核心视觉，它显示假状态比不显示更糟：它恰好盖住了
 * 「设备没连上」这个真信号。
 *
 * 真服务端是按 actor 在不在判在线的（handleDevices），mock 照做：
 * 没有任何东西声称它上线过，它就是离线。要在 UI 上造在线状态，
 * 用 POST /api/dev/device/{dev}/state。
 */
export function defaultDevice(dev, name, userId) {
  return {
    dev,
    name,
    user_id: userId,
    bound: Math.floor(Date.now() / 1000),
    online: false,
    seen: 0,
    state: 'offline',
    prn: {
      code: 10001, display: 'Ready', online: true,
      asleep: false, paper_out: false, error: false,
    },
    // 没插打印机。设备从没上报过 ident，服务端也就不知道插的是什么。
    printer: null,
    // 设备上报 ident 之后打印机长这样。dev 端点用它造「插上了」的状态。
    printerWhenAttached: {
      serial: 'CNB9K1P2X4', vid: '03F0', pid: 'F22A',
      make: 'HP', model: 'HP Laser MFP 136a',
      cmd: 'URF,PCL,PJL,PWGRaster',
      urf_caps: 'CP1,IS1-4-5-19,MT1-2-3-4-5-6,RS600,V1.4,W8',
      proto: 2, first_seen: 1756400000, last_seen: 1756500000,
      attached: true,
    },
    renderProfile: {
      format: 'urf',
      urf_caps: 'CP1,IS1-4-5-19,MT1-2-3-4-5-6,RS600,V1.4,W8',
      dpi: 600,
      color: 'gray8',
      pages: {
        a4: {w_px: 4962, h_px: 7014},
        letter: {w_px: 5100, h_px: 6600},
      },
      margins_mm: [4, 4, 4, 4],
      max_job_bytes: 209715200,
      updated: 1756500000,
    },
    tests: {
      double_print: {
        id: 'double_print', name: '连打两份', required: true,
        jobs_needed: 2, state: 'untested', affects: ['uel_job_end'],
      },
      wake: {
        id: 'wake', name: '休眠唤醒', required: false,
        jobs_needed: 1, state: 'untested', affects: ['uel_wake', 'wake_delay_ms'],
      },
      margins: {
        id: 'margins', name: '页边距', required: false,
        jobs_needed: 1, state: 'untested', affects: ['margins_mm'],
      },
    },
  };
}
