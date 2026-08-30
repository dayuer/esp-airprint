import {ClientConfig, request} from './http';
import {DeviceId, DeviceListItem, PrinterDetail, PrinterList, RenderProfile} from './types';

/** 4.5b 列出我名下的设备。一台都没有时返回空数组，不是 404。 */
export async function listDevices(cfg: ClientConfig): Promise<DeviceListItem[]> {
  const r = await request<{devices: DeviceListItem[]}>(cfg, {path: '/devices', device: null});
  return r.devices;
}

/** 4.1b 解绑。设备本来就没绑定时也成功（幂等）。 */
export function unbindDevice(cfg: ClientConfig, dev: DeviceId) {
  return request<{ok: 1}>(cfg, {path: `/device/${dev}/unbind`, method: 'POST', body: {}, device: null});
}

/**
 * 4.5 取光栅参数。**光栅之前必须先拉这个，不要硬编码尺寸和 dpi。**
 *
 * 404 表示设备从未上报过 ident（没插打印机，或还没枚举完）。此时提示
 * 「打印机未就绪」，不要用默认值蒙——尺寸蒙错就是废纸。所以这个函数
 * 没有也不会有 fallback 分支。
 */
export function getRenderProfile(cfg: ClientConfig, dev: DeviceId) {
  return request<RenderProfile>(cfg, {path: `/device/${dev}/render-profile`, device: null});
}

/** 4.7 打印机完整信息，含档案来源可信度。 */
export function getPrinter(cfg: ClientConfig, dev: DeviceId) {
  return request<PrinterDetail>(cfg, {path: `/device/${dev}/printer`, device: null});
}

/** 4.7b 这个桥见过的所有打印机，含各自排队中的作业数。 */
export function listPrinters(cfg: ClientConfig, dev: DeviceId) {
  return request<PrinterList>(cfg, {path: `/device/${dev}/printers`, device: null});
}
