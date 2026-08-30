import {ClientConfig, request} from './http';
import {AdaptTest, DeviceId, TestAnswerResponse, TestStartResponse, Verdict} from './types';

/** 4.8 测试清单。 */
export async function listTests(cfg: ClientConfig, dev: DeviceId): Promise<AdaptTest[]> {
  const r = await request<{tests: AdaptTest[]}>(cfg, {path: `/device/${dev}/tests`, device: null});
  return r.tests;
}

/**
 * 4.9 开始一轮测试。409 表示该设备已有一轮在进行中。
 *
 * `baseline_ok:false` 表示当前生效配置本身就打不出来。此时不要继续测变体——
 * 在一个本来就坏的链路上测，得到的全是噪音。
 */
export function startTest(cfg: ClientConfig, dev: DeviceId, test: string) {
  return request<TestStartResponse>(cfg, {
    path: `/device/${dev}/tests/${test}/start`, body: {}, device: null,
  });
}

/**
 * 4.10 提交用户判断。
 *
 * 用户不确定时必须让他选 `unclear`，不要逼他二选一——猜出来的数据比没有数据更糟。
 */
export function answerTest(
  cfg: ClientConfig,
  runId: string,
  verdict: Verdict,
  detail: Record<string, unknown> = {},
) {
  return request<TestAnswerResponse>(cfg, {
    path: `/tests/${runId}/answer`, body: {verdict, detail}, device: null,
  });
}
