import {ApiFailure, ClientConfig, DeviceId, enrollDevice, listDevices} from '../api';
import {ConnectOutcome, WaitOptions, provisionDevice} from './flow';
import {PortalNetwork, PortalOptions, readDeviceId, scanNetworks} from './portal';

/**
 * 配网编排。只连一次热点：
 *
 *   1. enroll 拿一把**待认领**的密钥  —— 还在自己的网络上，能上网
 *   2. 连配网热点                     —— 唯一一次
 *   3. 扫网络、写 Wi-Fi + 密钥        —— 直连设备
 *   4. 设备重启，热点消失，手机回到原网络
 *   5. 设备连上并认领密钥，上报 ident
 *   6. App 刷出设备
 *
 * 关键是第 1 步不需要知道 dev。`POST /api/device/enroll` 不带 dev 时签发
 * 一把待认领的密钥，设备首次连 MQTT 时用自己的 dev 做 username，服务端
 * 从那里学到 MAC 并完成绑定。
 *
 * 早先的实现要先读 dev 才能 enroll，于是被逼成「连热点读 MAC → 断开去
 * enroll → 再连热点写入」——手机同一时刻只能连一个 AP，连上配网热点就断开
 * 了原来的 Wi-Fi，enroll 只能指望蜂窝。服务端本来就能从设备自报的
 * username 知道 MAC，让 App 先去问设备要是多余的。
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
  /** 可留空。留空时签发待认领的密钥，绑定发生在设备首次连 MQTT 时。 */
  dev?: DeviceId;
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

export type ProvisionStage = 'enrolling' | 'joining' | 'sending' | 'waiting';

/**
 * enroll →（连热点）→ 写进设备 → 等它连上。
 *
 * `join` 在 enroll 之后、写凭据之前调用，用来加入配网热点。这是整个流程里
 * **唯一**一次连热点。不传 join 就是「已经连着」的模式，测试里用
 * （假门户在局域网上，不需要真的切网络）。
 *
 * 密钥和 Wi-Fi 凭据一次写进去：分两次写的话设备会先用空密钥重启一轮，
 * 白等 30 秒还多一次失败。
 */
export async function completeProvisioning(
  api: ClientConfig,
  input: ProvisionInput,
  opts: WaitOptions = {},
  onStage?: (s: ProvisionStage) => void,
  join?: () => Promise<unknown>,
): Promise<ProvisionResult> {
  onStage?.('enrolling');
  const enrolled = await enrollDevice(api, input.dev ?? '', input.name);

  if (join) {
    onStage?.('joining');
    await join();
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
