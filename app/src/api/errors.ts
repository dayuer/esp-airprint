/**
 * HTTP 错误在 client 边界就转成这个 union，UI 层不看状态码。
 *
 * 对应 docs/API-cloud-print.md 4.1 的状态码表。
 */
export type ApiError =
  | {kind: 'unauthorized'}                    // 401 密钥错、已吊销、或角色不对
  | {kind: 'forbidden'}                       // 403 有效但不属于这台设备
  | {kind: 'notFound'}                        // 404
  | {kind: 'conflict'; detail: string}        // 409 重试不会成功，要用户先做别的事
  | {kind: 'badRequest'; detail: string}      // 400 detail 里有期望值，要展示
  | {kind: 'tooLarge'}                        // 413
  | {kind: 'unsupportedMedia'}                // 415
  | {kind: 'rateLimited'; detail: string}     // 429 detail 说明撞了哪道闸
  | {kind: 'server'; detail: string}          // 5xx 可重试
  /**
   * 请求就没发出去。
   *
   * `cause` 是底层的原始报错，**只用于排查**，不进用户文案。
   * 少了它的话，线上只能看到一句「网络连不上」——而那句话对
   * 「证书不被信任」「DNS 解析失败」「端口被拦」是同一个说法。
   */
  | {kind: 'network'; cause?: string};

function detailOf(body: unknown): string {
  if (typeof body === 'object' && body !== null) {
    const d = (body as Record<string, unknown>).detail;
    if (typeof d === 'string') return d;
    const e = (body as Record<string, unknown>).e;
    if (typeof e === 'string') return e;
  }
  return '';
}

export function apiErrorFromResponse(status: number, body: unknown): ApiError {
  switch (status) {
    case 400:
      return {kind: 'badRequest', detail: detailOf(body)};
    case 401:
      // 文档 4.1：401 不区分「设备不存在」和「密钥错」，不给探测者提供信息。
      // 所以这里刻意丢掉 body——让 UI 层没有能力区分。
      return {kind: 'unauthorized'};
    case 403:
      return {kind: 'forbidden'};
    case 404:
      return {kind: 'notFound'};
    case 409:
      // 文档 4.1 的表里没有 409，但 enroll（设备属于其他账号）和
      // 测试 start（已有一轮在跑）都用它。落进 server 分支会让 UI 说
      // 「稍后重试」——而这两种情况重试永远不会成功。
      return {kind: 'conflict', detail: detailOf(body)};
    case 413:
      return {kind: 'tooLarge'};
    case 415:
      return {kind: 'unsupportedMedia'};
    case 429:
      return {kind: 'rateLimited', detail: detailOf(body)};
    default:
      return {kind: 'server', detail: detailOf(body)};
  }
}

/** 面向用户的文案。每个分支都要有，别让用户看到 undefined。 */
export function describeApiError(err: ApiError): string {
  switch (err.kind) {
    case 'unauthorized':
      return '登录已失效，请重新登录';
    case 'forbidden':
      return '没有权限操作这台设备';
    case 'notFound':
      return '找不到对应的记录';
    case 'conflict':
      return err.detail === 'device bound'
        ? '这台设备已绑定到其他账号，需原持有人先解绑'
        : err.detail || '当前状态下不能做这个操作';
    case 'badRequest':
      return err.detail || '请求被服务端拒绝';
    case 'tooLarge':
      return '文件太大，超过了服务端的上限';
    case 'unsupportedMedia':
      return '这种格式不能直接打印';
    case 'rateLimited':
      return err.detail || '操作太频繁，请稍后再试';
    case 'server':
      return '服务端出错了，可以稍后重试';
    case 'network':
      return __DEV__ && err.cause
        ? `网络连不上：${err.cause}`
        : '网络连不上，检查一下网络';
  }
}
