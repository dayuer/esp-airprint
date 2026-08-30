import {apiErrorFromResponse} from './errors';
import {ApiFailure, ClientConfig, buildHeaders, buildUrl, request} from './http';
import {encodeRfc5987} from './rfc5987';
import {DeviceId, PrintResponse} from './types';

export type RasterContentType = 'image/urf' | 'image/pwg-raster';

export interface PrintRequestInput {
  device: DeviceId;
  /**
   * 必填。取自 render-profile 响应里的 serial。
   *
   * 这个字段是防废纸的：URF 是按某台打印机的 dpi 和像素尺寸光栅的，派给另一台
   * 就是一沓废纸，而服务端不解析文档、设备不认识格式，没有任何环节会发现。
   */
  printerSerial: string;
  contentType: RasterContentType;
  /** 仅用于显示。 */
  filename?: string;
  /** 适配测试的一轮 ID，服务端据此下发一次性 quirk 覆盖。 */
  testRunId?: string;
}

export interface PreparedRequest {
  url: string;
  method: 'POST';
  headers: Record<string, string>;
}

/**
 * 只构造请求，不发送。
 *
 * 真正的上传在阶段三由原生后台任务做（200KB~15MB 不能过 JS 堆），但请求头的
 * 构造逻辑两边必须是同一份——否则 X-Printer-Serial 或 RFC 5987 文件名会在
 * 某一侧写错，而那两处写错都不会报错，只会变成废纸或乱码。
 */
export function buildPrintRequest(cfg: ClientConfig, input: PrintRequestInput): PreparedRequest {
  if (!input.printerSerial) {
    // 宁可在这里炸，也不要发一个会打到别的打印机上的请求。
    throw new ApiFailure({
      kind: 'badRequest',
      detail: 'X-Printer-Serial 必填，取自 render-profile 的 serial',
    });
  }
  const headers = buildHeaders(cfg, {device: input.device, contentType: input.contentType});
  headers['X-Printer-Serial'] = input.printerSerial;
  if (input.filename) headers['X-Filename'] = encodeRfc5987(input.filename);
  if (input.testRunId) headers['X-Test-Run'] = input.testRunId;
  return {url: buildUrl(cfg, '/print'), method: 'POST', headers};
}

/**
 * 直接上传一段字节。仅用于测试和小体积场景——真实作业走原生后台上传。
 */
export async function submitPrint(
  cfg: ClientConfig,
  input: PrintRequestInput,
  body: Uint8Array,
): Promise<PrintResponse> {
  const req = buildPrintRequest(cfg, input);
  const f = cfg.fetchImpl ?? fetch;
  let res: Response;
  try {
    res = await f(req.url, {method: req.method, headers: req.headers, body: body as unknown as BodyInit_});
  } catch (e) {
    throw new ApiFailure({kind: 'network', cause: (e as Error)?.message});
  }
  const text = await res.text();
  let parsed: unknown;
  try {
    parsed = text ? JSON.parse(text) : undefined;
  } catch {
    parsed = undefined;
  }
  if (!res.ok) {
    const err = apiErrorFromResponse(res.status, parsed);
    if (err.kind === 'unauthorized') cfg.onUnauthorized?.();
    throw new ApiFailure(err);
  }
  return parsed as PrintResponse;
}

/** 重新导出，方便调用方在上传前重查 serial。 */
export {request};
