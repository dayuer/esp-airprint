import {ApiFailure, ClientConfig, DeviceId, enrollDevice, listDevices} from '../api';
import {ConnectOutcome, WaitOptions, provisionDevice} from './flow';
import {PortalNetwork, PortalOptions, readDeviceId, scanNetworks} from './portal';

/**
 * 配网编排。三件事，每一件在不同的网络上：
 *
 *   1. 读设备 ID、扫 Wi-Fi   —— 连在配网热点上
 *   2. enroll 换设备密钥      —— **必须回到能上网的网络**
 *   3. 写凭据、等结果         —— 再连一次配网热点
 *
 * **顺序是被逼出来的，不是随便排的。**
 *
 * 手机同一时刻只能连一个 AP。连上配网热点就意味着断开原来的 Wi-Fi，
 * 此时只剩蜂窝兜底——没有 SIM、没开数据、Wi-Fi-only 的平板，enroll 就调不通。
 * 所以第 2 步必须在**离开热点之后**做，而不是趁着还连着顺手做掉。
 *
 * 代价是要连两次热点（两次系统确认框）。换来的是不依赖蜂窝。
 *
 * 而 enroll 又非得排在第 1 步之后不可：它要 dev，而 dev 只能从门户的
 * /status 读到（SSID 后缀是 SoftAP MAC，跟 STA MAC 差 1，推不出来）。
 */

export interface PortalInfo {
  /** 设备 ID。老固件读不到，那时抛错而不是编一个。 */
  dev: DeviceId;
  /** **设备**扫到的 Wi-Fi，不是手机扫到的。 */
  networks: PortalNetwork[];
}

/** 第一步：确认连上了设备热点，把设备 ID 和它能看到的网络读回来。 */
export async function readPortal(opts?: PortalOptions): Promise<PortalInfo> {
  const dev = await readDeviceId(opts);
  const networks = await scanNetworks(opts);
  return {dev, networks};
}

export interface ProvisionInput {
  dev: DeviceId;
  ssid: string;
  /** 开放网络传空串。 */
  pass: string;
  /** 设备在 App 里显示的名字。 */
  name: string;
}

export interface ProvisionResult {
  outcome: ConnectOutcome;
  /** true 表示这台设备之前就绑在本账号上，旧密钥已被吊销。 */
  reset: boolean;
}

export type ProvisionStage = 'enrolling' | 'rejoining' | 'sending' | 'waiting';

/**
 * 第二步：换密钥 →（回到热点）→ 写进设备 → 等它连上。
 *
 * `rejoin` 在 enroll 之后、写凭据之前调用，用来重新加入配网热点。
 * 调用方在读完门户信息后就应该已经离开热点了——那样 enroll 才走得通。
 * 不传 rejoin 就是「一直连着热点」的模式，只在测试里用（假门户在局域网上）。
 *
 * 密钥和 Wi-Fi 凭据一次写进去：分两次写的话设备会先用空密钥重启一轮，
 * 白等 30 秒还多一次失败。
 */
export async function completeProvisioning(
  api: ClientConfig,
  input: ProvisionInput,
  opts: WaitOptions = {},
  onStage?: (s: ProvisionStage) => void,
  rejoin?: () => Promise<unknown>,
): Promise<ProvisionResult> {
  onStage?.('enrolling');
  const enrolled = await enrollDevice(api, input.dev, input.name);

  if (rejoin) {
    onStage?.('rejoining');
    await rejoin();
  }

  onStage?.('sending');
  onStage?.('waiting');
  const outcome = await provisionDevice(
    {ssid: input.ssid, pass: input.pass, devKey: enrolled.device_key},
    opts,
  );

  return {outcome, reset: enrolled.reset};
}

/**
 * 第三步（只在结果是 lost 或 timeout 时需要）：回到自己的网络，
 * 问服务端那台设备到底在不在。
 *
 * 这是唯一能回答「配好了没有」的地方。门户在设备重启后就消失了，
 * 从那边看不出区别。
 */
export async function confirmProvisioned(
  api: ClientConfig,
  dev: DeviceId,
): Promise<boolean> {
  try {
    const devices = await listDevices(api);
    return devices.some(d => d.dev === dev);
  } catch (e) {
    if (e instanceof ApiFailure) return false;
    throw e;
  }
}
