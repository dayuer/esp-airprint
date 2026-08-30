import {ClientConfig, request} from './http';
import {DeviceId, StatusResponse} from './types';

/** 4.6 查设备与作业。只返回 X-Device 那一台的数据，最多最近 15 件作业。 */
export function getStatus(cfg: ClientConfig, dev: DeviceId) {
  return request<StatusResponse>(cfg, {path: '/status', device: dev});
}
