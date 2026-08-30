import {ApiFailure, ClientConfig, DeviceId, enrollDevice, listDevices} from '../api';
import {ConnectOutcome, WaitOptions, provisionDevice} from './flow';
import {PortalNetwork, PortalOptions, readDeviceId, scanNetworks} from './portal';

/**
 * 配网编排。**密钥前置**，只连一次热点：
 *
 *   1. enroll 拿一把待认领的密钥  —— 还在自己的网络上，能上网
 *   2. 连配网热点                 —— 唯一一次，之后手机就没有互联网了
 *   3. 读 dev、扫网络、写 Wi-Fi + 密钥
 *   4. 设备重启，热点消失，手机回到原网络
 *   5. 设备连上并认领密钥，上报 ident
 *   6. App 刷出设备
 *
 * **第 1 步必须在第 2 步之前**，这是整个设计的要害。手机同一时刻只能连一个
 * AP，连上配网热点就断开了原来的 Wi-Fi，此时只剩蜂窝兜底——没 SIM、没开
 * 数据、Wi-Fi-only 的平板就调不通 enroll。
 *
 * 能前置是因为 enroll 不需要知道 dev：不带 dev 时签发一把待认领的密钥，
 * 设备首次连 MQTT 时用自己的 dev 做 username，服务端从那里学到 MAC 并完成
 * 绑定。服务端本来就能知道，让 App 先去问设备要是多余的。
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

export interface EnrolledKey {
  deviceKey: string;
  /** true 表示这台设备之前就绑在本账号上，旧密钥已被吊销。 */
  reset: boolean;
}

/**
 * 第一步：拿密钥。**在连热点之前调用**——那之后手机就没有互联网了。
 *
 * 不带 dev：这一刻还不知道设备的 MAC，也不需要知道。
 */
export async function enrollForProvisioning(
  api: ClientConfig,
  name: string,
): Promise<EnrolledKey> {
  const r = await enrollDevice(api, '', name);
  return {deviceKey: r.device_key, reset: r.reset};
}

export interface WriteInput {
  ssid: string;
  /** 开放网络传空串。 */
  pass: string;
  /** 第一步拿到的密钥。 */
  devKey: string;
}

export type ProvisionStage = 'sending' | 'waiting';

/**
 * 第二步：把 Wi-Fi 凭据和密钥一起写进设备，等它连上。调用时手机已经在热点上。
 *
 * 密钥和 Wi-Fi 凭据一次写进去：分两次写的话设备会先用空密钥重启一轮，
 * 白等 30 秒还多一次失败。
 */
export async function writeAndWait(
  input: WriteInput,
  opts: WaitOptions = {},
  onStage?: (s: ProvisionStage) => void,
): Promise<ConnectOutcome> {
  onStage?.('sending');
  onStage?.('waiting');
  return provisionDevice(input, opts);
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
