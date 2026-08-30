/**
 * @jest-environment node
 */
import {apiErrorFromResponse, describeApiError} from '../errors';

// 文档 4.1 的状态码表逐行一条。UI 只对着 union 写文案，不看状态码，
// 所以映射错了不会有编译错误，只会让用户看到不相干的提示。
const cases: Array<[number, unknown, string]> = [
  [400, {e: 'bad request'}, 'badRequest'],
  [401, {e: 'unauthorized'}, 'unauthorized'],
  [403, {e: 'forbidden'}, 'forbidden'],
  [404, {e: 'not found'}, 'notFound'],
  [409, {e: 'device bound'}, 'conflict'],
  [413, {e: 'too large'}, 'tooLarge'],
  [415, {e: 'unsupported media type'}, 'unsupportedMedia'],
  [429, {e: 'rate limited'}, 'rateLimited'],
  [500, {e: 'boom'}, 'server'],
  [503, {e: 'nope'}, 'server'],
];

test.each(cases)('HTTP %i 映射成 %s', (status, body, kind) => {
  const err = apiErrorFromResponse(status, body);
  expect(err.kind).toBe(kind);
});

test('400 的 detail 要原样保留——服务端在这里给出期望值', () => {
  const err = apiErrorFromResponse(400, {
    e: 'bad raster',
    detail: '魔数不匹配：期望 UNIRAST\\0，实际 %PDF-1.7',
  });
  expect(err.kind).toBe('badRequest');
  if (err.kind === 'badRequest') {
    expect(err.detail).toContain('UNIRAST');
  }
});

test('429 的 detail 说明撞的是哪道闸', () => {
  const err = apiErrorFromResponse(429, {e: 'rate limited', detail: '同号码 60 秒间隔'});
  expect(err.kind).toBe('rateLimited');
  if (err.kind === 'rateLimited') {
    expect(err.detail).toBe('同号码 60 秒间隔');
  }
});

test('409 不能落进 server——那会让 UI 说「稍后重试」，而重试永远不会成功', () => {
  const err = apiErrorFromResponse(409, {e: 'device bound'});
  expect(err.kind).toBe('conflict');
  expect(describeApiError(err)).toContain('原持有人');
});

test('body 不是 JSON 时不能炸，退回 server', () => {
  const err = apiErrorFromResponse(500, undefined);
  expect(err.kind).toBe('server');
});

test('401 不携带任何区分「设备不存在」和「密钥错」的信息', () => {
  // 文档 4.1：不给探测者提供信息。UI 也不该有能力区分。
  const err = apiErrorFromResponse(401, {e: 'unauthorized', detail: 'device not found'});
  expect(Object.keys(err)).toEqual(['kind']);
});

test('每种错误都有面向用户的文案', () => {
  for (const [status, body] of cases) {
    const text = describeApiError(apiErrorFromResponse(status, body));
    expect(text.length).toBeGreaterThan(0);
  }
  expect(describeApiError({kind: 'network'}).length).toBeGreaterThan(0);
});
