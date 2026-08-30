/**
 * @jest-environment node
 */
import {enrollDevice, sendSms, verifySms} from '../auth';
import {getPrinter, getRenderProfile, listDevices, listPrinters, unbindDevice} from '../devices';
import {getStatus} from '../jobs';
import {ApiFailure, ClientConfig} from '../http';
import {MOCK_CODE, MockHandle, bringOnline, startMock} from './helpers/mock';

const DEV = 'f412fa87c9e0';
let mock: MockHandle;
let cfg: ClientConfig;

beforeAll(async () => {
  mock = await startMock();
  const anon = {baseUrl: mock.baseUrl};
  await sendSms(anon, '13800001234');
  const {token} = await verifySms(anon, '13800001234', MOCK_CODE);
  cfg = {baseUrl: mock.baseUrl, token};
  await enrollDevice(cfg, DEV, '工位打印机');
  // 在线且插着打印机是要显式造的状态，不是 enroll 的副产品。
  await bringOnline(mock.baseUrl, token, DEV);
});
afterAll(() => mock.stop());

test('刚 enroll 的设备是离线的、没插打印机的', async () => {
  // 它从没连过 MQTT、也从没上报过 ident——服务端不知道它插的是什么。
  // 这里如果是「在线 + 有打印机」，App 的连接线就会对每台新设备撒谎。
  const anon = {baseUrl: mock.baseUrl};
  await sendSms(anon, '13800007070');
  const {token} = await verifySms(anon, '13800007070', MOCK_CODE);
  const c = {baseUrl: mock.baseUrl, token};
  await enrollDevice(c, '0011deadbeef', '刚绑的');

  const [d] = await listDevices(c);
  expect(d.online).toBe(false);
  expect(d.printer).toBeNull();
  expect(d.state).toBe('offline');
});

test('一台设备都没有时返回空数组，不是 404', async () => {
  const anon = {baseUrl: mock.baseUrl};
  await sendSms(anon, '13800005678');
  const {token} = await verifySms(anon, '13800005678', MOCK_CODE);
  await expect(listDevices({baseUrl: mock.baseUrl, token})).resolves.toEqual([]);
});

test('设备列表带上当前插着的打印机和排队数', async () => {
  const list = await listDevices(cfg);
  expect(list).toHaveLength(1);
  expect(list[0].dev).toBe(DEV);
  expect(list[0].name).toBe('工位打印机');
  expect(list[0].printer).toEqual({
    serial: 'CNB9K1P2X4', make: 'HP', model: 'HP Laser MFP 136a', attached: true,
  });
  expect(list[0].queued_jobs).toBe(0);
});

test('render-profile 给出 600dpi、gray8 和各纸张像素尺寸', async () => {
  const p = await getRenderProfile(cfg, DEV);
  expect(p.dpi).toBe(600);
  expect(p.color).toBe('gray8');
  expect(p.serial).toBe('CNB9K1P2X4');
  expect(p.pages.a4).toEqual({w_px: 4962, h_px: 7014});
  expect(p.margins_mm).toEqual([4, 4, 4, 4]);
});

test('别人的设备返回 403，不存在的设备返回 404', async () => {
  const anon = {baseUrl: mock.baseUrl};
  await sendSms(anon, '13800009999');
  const {token} = await verifySms(anon, '13800009999', MOCK_CODE);
  const other = {baseUrl: mock.baseUrl, token};
  await expect(getRenderProfile(other, DEV)).rejects.toMatchObject({
    error: {kind: 'forbidden'},
  });
  await expect(getRenderProfile(cfg, '000000000000')).rejects.toMatchObject({
    error: {kind: 'notFound'},
  });
});

test('打印机档案带来源可信度', async () => {
  const d = await getPrinter(cfg, DEV);
  expect(d.printer.model).toBe('HP Laser MFP 136a');
  expect(d.profile.src).toBe('model');
  expect(d.profile.disputed).toBe(false);
});

test('打印机列表里 attached 是当前插着那台的序列号', async () => {
  const l = await listPrinters(cfg, DEV);
  expect(l.attached).toBe('CNB9K1P2X4');
  expect(l.printers[0].queued_jobs).toBe(0);
});

test('status 返回设备状态与作业列表', async () => {
  const s = await getStatus(cfg, DEV);
  expect(s.device.dev).toBe(DEV);
  expect(s.device.online).toBe(true);
  expect(s.device.prn?.display).toBe('Ready');
  expect(Array.isArray(s.jobs)).toBe(true);
});

test('解绑是幂等的，解绑后设备从列表里消失', async () => {
  const anon = {baseUrl: mock.baseUrl};
  await sendSms(anon, '13811112222');
  const {token} = await verifySms(anon, '13811112222', MOCK_CODE);
  const c = {baseUrl: mock.baseUrl, token};
  await enrollDevice(c, 'ffeeddccbbaa', '临时');
  await expect(unbindDevice(c, 'ffeeddccbbaa')).resolves.toEqual({ok: 1});
  await expect(unbindDevice(c, 'ffeeddccbbaa')).resolves.toEqual({ok: 1});
  await expect(listDevices(c)).resolves.toEqual([]);
});

test('ApiFailure 不会把 TypeError 漏到 UI——连不上时是 network', async () => {
  const dead = {baseUrl: 'http://127.0.0.1:1/api'};
  try {
    await listDevices(dead);
    throw new Error('本该失败');
  } catch (e) {
    expect(e).toBeInstanceOf(ApiFailure);
    expect((e as ApiFailure).error.kind).toBe('network');
  }
});

test('档案是可编排的动作序列，不是布尔开关表', async () => {
  // 文档 4.7 画的是 {uel_job_end, margins_mm, ...} 那种参数表，但 3.7b 早就
  // 把档案改成了动作序列，文档自己前后矛盾。真服务端返回的是动作序列。
  //
  // 这条不是在验 mock，是在钉住「以真服务端为准」这个决定：照 4.7 写类型的
  // 话，详情页会对 undefined 调 .join 直接崩——真机上就是这么崩的。
  const d = await getPrinter(cfg, DEV);
  expect(d.profile.hooks).toBeDefined();
  expect(Array.isArray(d.profile.hooks.job_end)).toBe(true);
  expect(d.profile.flags).toEqual({unidir: false, pjl_ok: true});
  expect(typeof d.profile.rev).toBe('number');
});

test('status 只保证 online，seen/state 可能没有', async () => {
  // 真服务端返回 {dev, online, serial, prn}——没有 seen 和 state。
  // 把它们当必填会让「在线」显示成 undefined。
  const s = await getStatus(cfg, DEV);
  expect(typeof s.device.online).toBe('boolean');
  expect(s.device.dev).toBe(DEV);
});
