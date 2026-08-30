import NativeWifiSetup from '../native/NativeWifiSetup';
import {PortalFailure, PortalOptions} from './portal';

export class WifiSetupUnavailable extends Error {
  constructor() {
    super('原生 Wi-Fi 模块未加载');
    this.name = 'WifiSetupUnavailable';
  }
}

export function isWifiSetupAvailable(): boolean {
  return NativeWifiSetup != null;
}

/** 系统会弹一个只列出匹配网络的选择框。返回实际加入的 SSID。 */
export async function joinSetupNetwork(
  ssidPrefix: string,
  timeoutMs = 30_000,
): Promise<string> {
  if (!NativeWifiSetup) throw new WifiSetupUnavailable();
  return NativeWifiSetup.joinSetupNetwork(ssidPrefix, timeoutMs);
}

export function leaveSetupNetwork(): void {
  NativeWifiSetup?.leave();
}

/**
 * 一个绑到配网热点的 fetch。
 *
 * portal.ts 的每个函数都接受 `fetchImpl`，所以协议实现一行都不用改——
 * 当初把它做成可注入的，就是为了这一刻。
 *
 * 普通的 fetch 在这里是不行的：手机的默认网络还是蜂窝或家里的 Wi-Fi
 * （这正是我们要的，enroll 得走它），所以 192.168.4.1 根本不在路由上。
 */
export const wifiBoundFetch: typeof fetch = async (input, init) => {
  if (!NativeWifiSetup) throw new WifiSetupUnavailable();
  const url = typeof input === 'string' ? input : String(input);
  const method = init?.method ?? 'GET';
  const body = typeof init?.body === 'string' ? init.body : null;

  // AbortController 的超时由调用方给；这里把它折算成原生侧的超时。
  const timeoutMs = 5000;
  try {
    const r = await NativeWifiSetup.portalRequest(method, url, body, timeoutMs);
    return new Response(r.body, {status: r.status});
  } catch (e) {
    const msg = (e as Error).message ?? '';
    // 原生侧只给得出「失败」，这里按错误文本分一下类，让 UI 能说清下一步。
    throw new PortalFailure(
      /timeout|timed out/i.test(msg) ? {kind: 'timeout'} : {kind: 'unreachable'},
    );
  }
};

/** 走配网热点的 PortalOptions。 */
export function portalOverWifi(base: string): PortalOptions {
  return {base, fetchImpl: wifiBoundFetch};
}
