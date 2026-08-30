import {ApiFailure, ClientConfig} from './http';
import {getStatus} from './jobs';
import {DeviceId, DeviceListItem} from './types';
import {listDevices} from './devices';

/**
 * 已知设备的本地记录。
 *
 * 它**不是**设备列表的真相来源——`GET /api/devices`（文档 4.5b）才是。
 * 这份记录只有一个用途：服务端还没部署那个端点时的降级。
 *
 * 为什么不能长期靠它：换手机、重装 App 之后它就没了，而账号还在、设备还
 * 绑着，用户会看到一个空列表，只能把每台设备重新配一遍网。当初往文档里
 * 补 4.5b 就是为了这个。
 */
export interface KnownDeviceStore {
  load(): Promise<DeviceId[]>;
  add(dev: DeviceId): Promise<void>;
  remove(dev: DeviceId): Promise<void>;
}

export function memoryKnownDevices(initial: DeviceId[] = []): KnownDeviceStore {
  let ids = [...initial];
  return {
    load: async () => [...ids],
    add: async d => {
      if (!ids.includes(d)) ids.push(d);
    },
    remove: async d => {
      ids = ids.filter(x => x !== d);
    },
  };
}

/**
 * 拉设备列表，服务端没有 4.5b 时降级到逐台查 /api/status。
 *
 * 降级是**明说的**，不是悄悄兜底：返回值带 degraded 标记，UI 据此提示
 * 「这台服务器还没升级，换手机后需要重新配网」。悄悄兜底的话，用户会在
 * 换手机之后才发现设备全没了，而那时候完全看不出是服务端的问题。
 */
export interface DeviceListResult {
  devices: DeviceListItem[];
  degraded: boolean;
}

export async function fetchDeviceList(
  cfg: ClientConfig,
  known: KnownDeviceStore,
): Promise<DeviceListResult> {
  try {
    const devices = await listDevices(cfg);
    // 端点存在，顺手把本地记录对齐，将来降级时也是新的。
    for (const d of devices) await known.add(d.dev);
    return {devices, degraded: false};
  } catch (e) {
    // 只有 404 才降级。401 要照常抛出去让会话作废，网络错误也不该被当成
    // 「服务端没这个端点」——那会让一次断网看起来像一次服务端降级。
    if (!(e instanceof ApiFailure) || e.error.kind !== 'notFound') throw e;
  }

  const ids = await known.load();
  const devices: DeviceListItem[] = [];
  for (const dev of ids) {
    try {
      const s = await getStatus(cfg, dev);
      devices.push({
        dev,
        name: '',
        online: s.device.online,
        // 真服务端不返回 seen/state，只给 online。别在这里编一个出来。
        seen: s.device.seen ?? 0,
        state: s.device.state ?? (s.device.online ? 'ready' : 'offline'),
        bound: 0,
        // /api/status 不返回打印机型号，只能给出「有没有」。
        printer: null,
        queued_jobs: s.jobs.filter(j => j.state === 'queued').length,
      });
    } catch (e) {
      if (e instanceof ApiFailure && e.error.kind === 'unauthorized') throw e;
      // 查不到的那台先跳过——一台查不通不该让整个列表空掉。
    }
  }
  return {devices, degraded: true};
}
