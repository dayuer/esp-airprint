import {ApiFailure, ClientConfig, DeviceId, enrollDevice, listDevices} from '../api';
import {ConnectOutcome, WaitOptions, provisionDevice} from './flow';
import {PortalNetwork, PortalOptions, readDeviceId, scanNetworks} from './portal';

/**
 * 配网编排。把三件事串起来，每一件都在不同的网络上：
 *
 *   1. 读设备 ID、扫 Wi-Fi        —— 手机连在设备的配网热点上
 *   2. enroll 换设备密钥           —— **需要互联网**
 *   3. 写凭据、等结果              —— 又回到配网热点
 *
 * 第 2 步是个真实的麻烦：配网热点没有互联网。iOS 会把非本地流量走蜂窝，
 * Android 连上「无互联网」的 Wi-Fi 后通常也保留移动数据，所以多数情况能过，
 * 但手机没有蜂窝（只有 Wi-Fi 的 iPad、飞行模式）时就不行。
 * 这一层不掩盖它——enroll 失败时如实报「配网时手机需要能上网」。
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

export type ProvisionStage = 'enrolling' | 'sending' | 'waiting';

/**
 * 第二步：换密钥、写进设备、等它连上。
 *
 * enroll 放在写凭据之前：密钥要和 Wi-Fi 凭据一起写进去，一次搞定。
 * 分两次写的话，设备会先用空密钥重启一轮，那是白等 30 秒。
 */
export async function completeProvisioning(
  api: ClientConfig,
  input: ProvisionInput,
  opts: WaitOptions = {},
  onStage?: (s: ProvisionStage) => void,
): Promise<ProvisionResult> {
  onStage?.('enrolling');
  const enrolled = await enrollDevice(api, input.dev, input.name);

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
