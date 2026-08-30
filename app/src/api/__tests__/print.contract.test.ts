/**
 * @jest-environment node
 */
import {readFileSync} from 'node:fs';
import path from 'node:path';
import {enrollDevice, sendSms, verifySms} from '../auth';
import {getRenderProfile} from '../devices';
import {getStatus} from '../jobs';
import {buildPrintRequest, submitPrint} from '../print';
import {answerTest, listTests, startTest} from '../tests';
import {ApiFailure, ClientConfig} from '../http';
import {MOCK_CODE, MockHandle, startMock} from './helpers/mock';

const DEV = 'f412fa87c9e0';
const SERIAL = 'CNB9K1P2X4';
let mock: MockHandle;
let cfg: ClientConfig;

/**
 * 阶段一的 C++ 编码器真产出的 URF，不是手搓的假字节。
 * 用 Apple 光栅器的样本当上传体，能同时验到入口校验的魔数和页数两条。
 */
const realUrf = readFileSync(
  path.resolve(__dirname, '../../../shared/urf/tests/fixtures/apple-gray8-2480x3507-300dpi.urf'),
);

beforeAll(async () => {
  mock = await startMock();
  const anon = {baseUrl: mock.baseUrl};
  await sendSms(anon, '13800002222');
  const {token} = await verifySms(anon, '13800002222', MOCK_CODE);
  cfg = {baseUrl: mock.baseUrl, token, device: DEV};
  await enrollDevice(cfg, DEV, '工位打印机');
});
afterAll(() => mock.stop());

// ---- buildPrintRequest 是纯函数，原生上传器和这里用同一份 ----

test('请求头带 Content-Type、X-Device、X-Printer-Serial', () => {
  const r = buildPrintRequest({baseUrl: 'https://h/api', token: 'a.b', device: DEV}, {
    device: DEV, printerSerial: SERIAL, contentType: 'image/urf',
  });
  expect(r.url).toBe('https://h/api/print');
  expect(r.headers['Content-Type']).toBe('image/urf');
  expect(r.headers['X-Device']).toBe(DEV);
  expect(r.headers['X-Printer-Serial']).toBe(SERIAL);
  expect(r.headers.Authorization).toBe('Bearer a.b');
});

test('serial 为空直接抛，不发请求——那会把作业打到别的打印机上', () => {
  expect(() =>
    buildPrintRequest({baseUrl: 'https://h/api', device: DEV}, {
      device: DEV, printerSerial: '', contentType: 'image/urf',
    }),
  ).toThrow(ApiFailure);
});

test('中文文件名走 RFC 5987', () => {
  const r = buildPrintRequest({baseUrl: 'https://h/api', device: DEV}, {
    device: DEV, printerSerial: SERIAL, contentType: 'image/urf', filename: '报告.pdf',
  });
  expect(r.headers['X-Filename']).toBe("UTF-8''%E6%8A%A5%E5%91%8A.pdf");
});

test('X-Test-Run 只在传了 testRunId 时出现', () => {
  const base = {baseUrl: 'https://h/api', device: DEV};
  const input = {device: DEV, printerSerial: SERIAL, contentType: 'image/urf' as const};
  expect(buildPrintRequest(base, input).headers['X-Test-Run']).toBeUndefined();
  expect(
    buildPrintRequest(base, {...input, testRunId: '3f9a1c04bd77'}).headers['X-Test-Run'],
  ).toBe('3f9a1c04bd77');
});

// ---- 入口校验 ----

test('上传真实 URF 成功，服务端读出的页数要和文件一致', async () => {
  const r = await submitPrint(cfg, {
    device: DEV, printerSerial: SERIAL, contentType: 'image/urf', filename: '报告.pdf',
  }, realUrf);
  expect(r.state).toBe('queued');
  expect(r.size).toBe(realUrf.length);
  expect(r.pages).toBe(1);              // 与 fixture 的页数字段一致
  expect(r.printer_attached).toBe(true);
});

test('PDF 冒充 URF 返回 400，detail 给出期望的魔数', async () => {
  try {
    await submitPrint(cfg, {
      device: DEV, printerSerial: SERIAL, contentType: 'image/urf',
    }, Buffer.from('%PDF-1.7 fake'));
    throw new Error('本该 400');
  } catch (e) {
    const err = (e as ApiFailure).error;
    expect(err.kind).toBe('badRequest');
    if (err.kind === 'badRequest') expect(err.detail).toContain('UNIRAST');
  }
});

test('页数字段为 0 返回 400——打印机会认为文档为空，什么都不打', async () => {
  const zeroPages = Buffer.from(realUrf);
  zeroPages.writeUInt32BE(0, 8);
  try {
    await submitPrint(cfg, {
      device: DEV, printerSerial: SERIAL, contentType: 'image/urf',
    }, zeroPages);
    throw new Error('本该 400');
  } catch (e) {
    const err = (e as ApiFailure).error;
    expect(err.kind).toBe('badRequest');
    if (err.kind === 'badRequest') expect(err.detail).toContain('页数');
  }
});

test('PDF 的 Content-Type 直接 415', async () => {
  try {
    await submitPrint(cfg, {
      device: DEV, printerSerial: SERIAL,
      contentType: 'application/pdf' as unknown as 'image/urf',
    }, realUrf);
    throw new Error('本该 415');
  } catch (e) {
    expect((e as ApiFailure).error.kind).toBe('unsupportedMedia');
  }
});

test('serial 与当前插着的不符时入队但 printer_attached 为 false', async () => {
  const r = await submitPrint(cfg, {
    device: DEV, printerSerial: 'VNC7J2Q9Y1', contentType: 'image/urf',
  }, realUrf);
  expect(r.state).toBe('queued');
  expect(r.printer_attached).toBe(false);
});

// ---- 光栅参数与上传的联动 ----

test('先拉 render-profile，再用它的 serial 上传', async () => {
  const p = await getRenderProfile(cfg, DEV);
  const r = await submitPrint(cfg, {
    device: DEV, printerSerial: p.serial, contentType: 'image/urf',
  }, realUrf);
  expect(r.printer_attached).toBe(true);
});

// ---- 适配测试 ----

test('测试清单里 double_print 是必做的', async () => {
  const tests = await listTests(cfg, DEV);
  const dp = tests.find(t => t.id === 'double_print');
  expect(dp?.required).toBe(true);
  expect(dp?.jobs_needed).toBe(2);
  expect(dp?.affects).toContain('uel_job_end');
});

test('开始一轮测试拿到 run_id 与变体，重复开返回 conflict', async () => {
  const r = await startTest(cfg, DEV, 'double_print');
  expect(r.run_id).toMatch(/^[0-9a-f]{12}$/);
  expect(r.jobs_needed).toBe(2);
  expect(r.baseline_ok).toBe(true);
  expect(r.variant).toEqual({uel_job_end: false});

  try {
    await startTest(cfg, DEV, 'wake');
    throw new Error('本该 409');
  } catch (e) {
    expect((e as ApiFailure).error.kind).toBe('conflict');
  }

  await answerTest(cfg, r.run_id, 'fail', {pages_printed: 1});
});

test('unclear 是合法判断——猜出来的数据比没有数据更糟', async () => {
  const r = await startTest(cfg, DEV, 'wake');
  const a = await answerTest(cfg, r.run_id, 'unclear', {printed: false});
  expect(a.scope).toBe('serial');
  const tests = await listTests(cfg, DEV);
  expect(tests.find(t => t.id === 'wake')?.state).toBe('unclear');
});

// ---- 文件名的编解码往返 ----

/**
 * 上传一份并按作业 ID 取回它的名字。
 *
 * 不按列表位置断言：/api/status 按 created 排序，而它是秒级的，
 * 同一秒入队的作业顺序不确定——那种断言本身就是脆的。
 */
async function roundTripName(filename: string): Promise<string> {
  const r = await submitPrint(cfg, {
    device: DEV, printerSerial: SERIAL, contentType: 'image/urf', filename,
  }, realUrf);
  const s = await getStatus(cfg, DEV);
  const job = s.jobs.find(j => j.id === r.job);
  if (!job) throw new Error(`作业 ${r.job} 不在列表里`);
  return job.name;
}

// 这是 encodeRfc5987 唯一有意义的验证方式：编码器和一个会解码的服务端对上。
// 只断言编码器输出的字符串，等于只验证了它和自己的想象一致。
test('中文文件名上传后在作业列表里显示为原文', async () => {
  expect(await roundTripName('季度报告.pdf')).toBe('季度报告.pdf');
});

test('文件名里的空格和引号也要能还原', async () => {
  expect(await roundTripName('my report".pdf')).toBe('my report".pdf');
});

test('emoji 文件名走多字节 UTF-8 也能还原', async () => {
  expect(await roundTripName('🖨️打印.pdf')).toBe('🖨️打印.pdf');
});
