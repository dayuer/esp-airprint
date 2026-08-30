import {
  ConnectInput, PortalFailure, PortalOptions, getPortalStatus, sendCredentials,
} from './portal';

/**
 * 试连的结果。
 *
 * `lost` 是这里唯一不寻常的一个，也是最重要的一个：设备验证通过后 3 秒就
 * `esp_restart()`，热点随之消失。如果轮询正好错过那 3 秒窗口（App 被切到
 * 后台、手机慢了一拍），我们看到的只有「门户不见了」——那既可能是配网成功，
 * 也可能是设备掉电。
 *
 * **不许猜。** 猜成功会让用户以为配好了然后发现打不了印；猜失败会让他把
 * 一台已经配好的设备重配一遍。正确的做法是回到自己的 Wi-Fi 去
 * `GET /api/devices` 看那台 dev 在不在——那是唯一的权威答案。
 */
export type ConnectOutcome =
  | {kind: 'connected'; ip: string}
  | {kind: 'failed'; reason: string}
  | {kind: 'lost'}
  | {kind: 'timeout'};

export interface WaitOptions extends PortalOptions {
  /** 轮询间隔。默认 500ms——st 从 0 变 1 只有 3 秒窗口，慢了会错过。 */
  pollMs?: number;
  /** 总时长。固件的试连超时在 15 秒上下，留够余量。 */
  totalMs?: number;
  /** 连续多少次连不上就判定门户没了。默认 3 次。 */
  lostAfter?: number;
  sleep?: (ms: number) => Promise<void>;
  now?: () => number;
}

const defaultSleep = (ms: number) => new Promise<void>(r => setTimeout(r, ms));

export async function waitForConnectResult(opts: WaitOptions = {}): Promise<ConnectOutcome> {
  const {
    pollMs = 500,
    totalMs = 40_000,
    lostAfter = 3,
    sleep = defaultSleep,
    now = Date.now,
    ...portal
  } = opts;

  const deadline = now() + totalMs;
  let everReached = false;
  let consecutiveMisses = 0;

  while (now() < deadline) {
    try {
      const s = await getPortalStatus(portal);
      everReached = true;
      consecutiveMisses = 0;
      if (s.st === 1) return {kind: 'connected', ip: s.ip};
      if (s.st === 2) return {kind: 'failed', reason: s.e || '设备没能连上这个网络'};
    } catch (e) {
      if (!(e instanceof PortalFailure)) throw e;
      consecutiveMisses++;
      // 只有在「曾经连上过门户」之后失联才算 lost。一次都没连上说明手机
      // 压根没在这个热点上，那是另一回事，让它走到 timeout 去。
      if (everReached && consecutiveMisses >= lostAfter) return {kind: 'lost'};
    }
    await sleep(pollMs);
  }
  return {kind: 'timeout'};
}

/** 写凭据 + 等结果。配网向导直接用这一个。 */
export async function provisionDevice(
  input: ConnectInput,
  opts: WaitOptions = {},
): Promise<ConnectOutcome> {
  await sendCredentials(input, opts);
  return waitForConnectResult(opts);
}

/** 给用户看的一句话，以及下一步该做什么。 */
export function describeOutcome(o: ConnectOutcome): {text: string; nextStep: string} {
  switch (o.kind) {
    case 'connected':
      return {
        text: `设备已连上网络（${o.ip}），正在重启`,
        nextStep: '回到你自己的 Wi-Fi，稍等片刻它就会出现在设备列表里',
      };
    case 'failed':
      return {text: `设备连不上这个网络：${o.reason}`, nextStep: '检查密码，或换一个网络重试'};
    case 'lost':
      return {
        // 说不知道，比猜一个答案强。
        text: '设备的配网热点消失了。它可能已经配好并重启，也可能掉电了',
        nextStep: '回到你自己的 Wi-Fi 看设备列表——它在那儿就说明配好了',
      };
    case 'timeout':
      return {
        text: '一直没等到设备的回应',
        nextStep: '确认手机还连在设备的配网热点上，然后重试',
      };
  }
}
