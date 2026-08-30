/**
 * 设备配网门户的客户端。协议来自 main/provision.c，记在
 * docs/API-cloud-print.md 5.0b。
 *
 * 它和 src/api 是两套东西，不共用 ApiError：门户不是那个服务端，
 * 没有 token、没有 401、没有 JSON 错误体，而且**它会在成功之后消失**。
 */

/**
 * 设备未配网时的开放热点。SSID 后缀是 STA MAC 的最后两字节（大写十六进制）。
 *
 * 两个前缀都认：项目正在从 esp-airprint 改名为 stickbox，固件里的
 * `AirPrint-Setup-` 会变成 `StickBox-Setup-`。全网设备都升上去之前，
 * 只认新的会让老设备配不了网。**改名落定后删掉旧前缀。**
 */
export const SETUP_SSID_PREFIXES = ['StickBox-Setup-', 'AirPrint-Setup-'] as const;

export function isSetupSsid(ssid: string): boolean {
  return SETUP_SSID_PREFIXES.some(
    p => ssid.startsWith(p) && /^[0-9A-F]{4}$/.test(ssid.slice(p.length)),
  );
}

export const PORTAL_BASE = 'http://192.168.4.1';

/** 门户里的一个 Wi-Fi。字段名短是因为固件要塞进 3KB 的缓冲。 */
export interface PortalNetwork {
  /** SSID */
  s: string;
  /** RSSI，负数，越接近 0 越强 */
  r: number;
  /** 1 表示要密码 */
  k: number;
}

export interface PortalStatus {
  /** 0 试连中，1 验证通过（设备 3 秒后重启），2 失败 */
  st: 0 | 1 | 2;
  ip: string;
  e: string;
  /**
   * 设备 ID。**老固件没有这个字段**（见 API 文档 5.0b 的缺口）。
   * 缺失时不要猜——猜出来的后果是 enroll 到一台不存在的设备上，
   * 密钥写进去也连不上，而用户完全看不出问题在哪。
   */
  dev?: string;
}

export type PortalError =
  /** 连不上门户。手机可能没连上热点，或设备已经重启。 */
  | {kind: 'unreachable'}
  /** 超时。SoftAP 没有互联网，路由错了会一直挂着，所以每个请求都要有超时。 */
  | {kind: 'timeout'}
  /** 回了东西但不是预期的形状。多半是连到了别的设备或运营商的劫持页。 */
  | {kind: 'malformed'; detail: string};

export class PortalFailure extends Error {
  constructor(public readonly error: PortalError) {
    super(`portal ${error.kind}`);
    this.name = 'PortalFailure';
  }
}

export interface PortalOptions {
  base?: string;
  /** 每个请求的超时。默认 5 秒——门户在同一个链路层，慢不到哪去。 */
  timeoutMs?: number;
  fetchImpl?: typeof fetch;
}

async function getJson<T>(path: string, opts: PortalOptions = {}): Promise<T> {
  const {base = PORTAL_BASE, timeoutMs = 5000, fetchImpl = fetch} = opts;
  const ctl = new AbortController();
  const timer = setTimeout(() => ctl.abort(), timeoutMs);
  let res: Response;
  try {
    res = await fetchImpl(`${base}${path}`, {signal: ctl.signal});
  } catch (e) {
    // 注入的传输层（走配网热点那个）已经分好类了，别再包一层丢掉信息。
    if (e instanceof PortalFailure) throw e;
    // AbortError 和「连不上」要分开：前者可能是路由走错了（走了蜂窝），
    // 后者多半是没连上热点或设备已重启。给用户的下一步不一样。
    const aborted = (e as {name?: string})?.name === 'AbortError';
    throw new PortalFailure({kind: aborted ? 'timeout' : 'unreachable'});
  } finally {
    clearTimeout(timer);
  }

  const text = await res.text();
  try {
    return JSON.parse(text) as T;
  } catch {
    throw new PortalFailure({kind: 'malformed', detail: text.slice(0, 120)});
  }
}

/**
 * 设备扫到的 Wi-Fi 列表，按信号从强到弱。
 *
 * **要用设备扫到的，不是手机扫到的。** 设备的天线和位置跟手机不同，
 * 手机能看到的 AP 设备未必够得着——那会让用户选一个设备连不上的网络。
 */
export async function scanNetworks(opts?: PortalOptions): Promise<PortalNetwork[]> {
  const list = await getJson<unknown>('/scan', opts);
  if (!Array.isArray(list)) {
    throw new PortalFailure({kind: 'malformed', detail: '/scan 没有返回数组'});
  }
  return (list as PortalNetwork[])
    .filter(n => n && typeof n.s === 'string' && n.s.length > 0)
    .sort((a, b) => b.r - a.r);
}

export async function getPortalStatus(opts?: PortalOptions): Promise<PortalStatus> {
  const s = await getJson<PortalStatus>('/status', opts);
  if (typeof s?.st !== 'number') {
    throw new PortalFailure({kind: 'malformed', detail: '/status 没有 st 字段'});
  }
  return s;
}

/**
 * 读设备 ID。老固件不返回它，此时抛 malformed 而不是编一个出来。
 */
export async function readDeviceId(opts?: PortalOptions): Promise<string> {
  const s = await getPortalStatus(opts);
  if (typeof s.dev !== 'string' || !/^[0-9a-f]{12}$/.test(s.dev)) {
    throw new PortalFailure({
      kind: 'malformed',
      detail: '门户没有返回设备 ID，固件版本过旧',
    });
  }
  return s.dev;
}

export interface ConnectInput {
  ssid: string;
  /** 开放网络传空串。 */
  pass: string;
  /** 设备密钥，来自 POST /api/device/enroll。 */
  devKey: string;
}

/** 把 Wi-Fi 凭据和设备密钥一起写进设备，设备随即开始试连。 */
export async function sendCredentials(
  input: ConnectInput,
  opts: PortalOptions = {},
): Promise<void> {
  const {base = PORTAL_BASE, timeoutMs = 5000, fetchImpl = fetch} = opts;
  const ctl = new AbortController();
  const timer = setTimeout(() => ctl.abort(), timeoutMs);
  let res: Response;
  try {
    res = await fetchImpl(`${base}/connect`, {
      method: 'POST',
      headers: {'Content-Type': 'application/json'},
      // 字段名就是这三个单字母，固件用的是极简 JSON 取值，认不出别的。
      body: JSON.stringify({s: input.ssid, p: input.pass, k: input.devKey}),
      signal: ctl.signal,
    });
  } catch (e) {
    if (e instanceof PortalFailure) throw e;
    const aborted = (e as {name?: string})?.name === 'AbortError';
    throw new PortalFailure({kind: aborted ? 'timeout' : 'unreachable'});
  } finally {
    clearTimeout(timer);
  }

  const text = await res.text();
  let body: {ok?: number};
  try {
    body = JSON.parse(text);
  } catch {
    throw new PortalFailure({kind: 'malformed', detail: text.slice(0, 120)});
  }
  if (body.ok !== 1) {
    throw new PortalFailure({kind: 'malformed', detail: 'SSID 为空，设备拒绝了'});
  }
}
