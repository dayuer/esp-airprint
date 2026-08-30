import {ApiError, apiErrorFromResponse} from './errors';
import {DeviceId, Token} from './types';

/**
 * fetch 封装。用全局 fetch，Node 22 和 RN 0.87 都有，所以契约测试能在
 * jest 的 node 环境里跑，不需要模拟器。
 */
export interface ClientConfig {
  /** 例如 https://mqtt.silkline.id:9443/api */
  baseUrl: string;
  /** session token 或 device 密钥。当不透明串处理。 */
  token?: Token;
  /** 一个用户可能有多个桥，得说清楚操作哪一台（文档 4.1）。 */
  device?: DeviceId;
  fetchImpl?: typeof fetch;
}

/** 请求失败时抛这个，body 是已经映射好的 union。 */
export class ApiFailure extends Error {
  constructor(public readonly error: ApiError) {
    super(`api ${error.kind}`);
    this.name = 'ApiFailure';
  }
}

export interface RequestOptions {
  method?: string;
  path: string;
  body?: unknown;
  /** 覆盖 config 里的 device，或显式不带。 */
  device?: DeviceId | null;
  query?: Record<string, string>;
}

export function buildHeaders(
  cfg: ClientConfig,
  opts: {device?: DeviceId | null; contentType?: string} = {},
): Record<string, string> {
  const h: Record<string, string> = {};
  if (opts.contentType) h['Content-Type'] = opts.contentType;
  if (cfg.token) h.Authorization = `Bearer ${cfg.token}`;
  const dev = opts.device === undefined ? cfg.device : opts.device;
  if (dev) h['X-Device'] = dev;
  return h;
}

export function buildUrl(cfg: ClientConfig, path: string, query?: Record<string, string>): string {
  const base = cfg.baseUrl.replace(/\/+$/, '');
  const url = `${base}${path}`;
  if (!query || Object.keys(query).length === 0) return url;
  const qs = new URLSearchParams(query).toString();
  return `${url}?${qs}`;
}

export async function request<T>(cfg: ClientConfig, opts: RequestOptions): Promise<T> {
  const f = cfg.fetchImpl ?? fetch;
  const hasBody = opts.body !== undefined;

  let res: Response;
  try {
    res = await f(buildUrl(cfg, opts.path, opts.query), {
      method: opts.method ?? (hasBody ? 'POST' : 'GET'),
      headers: buildHeaders(cfg, {
        device: opts.device,
        contentType: hasBody ? 'application/json' : undefined,
      }),
      body: hasBody ? JSON.stringify(opts.body) : undefined,
    });
  } catch {
    // 网络类异常不能让 TypeError 漏到 UI 层。
    throw new ApiFailure({kind: 'network'});
  }

  const text = await res.text();
  let parsed: unknown;
  try {
    parsed = text ? JSON.parse(text) : undefined;
  } catch {
    parsed = undefined;
  }

  if (!res.ok) throw new ApiFailure(apiErrorFromResponse(res.status, parsed));
  return parsed as T;
}
