/**
 * @jest-environment node
 */
import {ApiFailure} from '../http';
import {deleteAccount, enrollDevice, logout, sendSms, verifySms} from '../auth';
import {listDevices} from '../devices';
import {MOCK_CODE, MockHandle, startMock} from './helpers/mock';

const PHONE = '13800008888';
let mock: MockHandle;
let base: string;

beforeAll(async () => {
  mock = await startMock();
  base = mock.baseUrl;
});
afterAll(() => mock.stop());

async function login(phone = PHONE) {
  const cfg = {baseUrl: base};
  await sendSms(cfg, phone);
  const r = await verifySms(cfg, phone, MOCK_CODE, 'jest');
  return {token: r.token, verify: r};
}

test('发码返回有效期', async () => {
  const r = await sendSms({baseUrl: base}, '13900001111');
  expect(r.ok).toBe(1);
  expect(r.ttl).toBe(300);
});

test('同号码 60 秒内再发被限流，detail 说明撞了哪道闸', async () => {
  const cfg = {baseUrl: base};
  await sendSms(cfg, '13900002222');
  await expect(sendSms(cfg, '13900002222')).rejects.toThrow(ApiFailure);
  try {
    await sendSms(cfg, '13900002222');
  } catch (e) {
    const err = (e as ApiFailure).error;
    expect(err.kind).toBe('rateLimited');
    if (err.kind === 'rateLimited') expect(err.detail).toContain('60');
  }
});

test('验码成功返回 token 与手机尾号，首次登录 new_user 为 true', async () => {
  const {verify} = await login('13700003333');
  expect(verify.token).toMatch(/^[0-9a-f]{12}\.[A-Za-z0-9_-]{32}$/);
  expect(verify.phone_tail).toBe('3333');
  expect(verify.new_user).toBe(true);
});

test('验码错误返回 401', async () => {
  const cfg = {baseUrl: base};
  await sendSms(cfg, '13700004444');
  try {
    await verifySms(cfg, '13700004444', '000000');
    throw new Error('本该 401');
  } catch (e) {
    expect((e as ApiFailure).error.kind).toBe('unauthorized');
  }
});

test('没有 token 调受保护端点返回 401', async () => {
  try {
    await listDevices({baseUrl: base});
    throw new Error('本该 401');
  } catch (e) {
    expect((e as ApiFailure).error.kind).toBe('unauthorized');
  }
});

test('登出后同一个 token 立刻失效', async () => {
  const {token} = await login('13700005555');
  const cfg = {baseUrl: base, token};
  await expect(listDevices(cfg)).resolves.toEqual([]);
  await logout(cfg);
  try {
    await listDevices(cfg);
    throw new Error('本该 401');
  } catch (e) {
    expect((e as ApiFailure).error.kind).toBe('unauthorized');
  }
});

test('enroll 同一台设备第二次返回 reset:true 且旧密钥换掉', async () => {
  const {token} = await login('13700006666');
  const cfg = {baseUrl: base, token};
  const first = await enrollDevice(cfg, 'f412fa87c9e0', '工位打印机');
  expect(first.reset).toBe(false);
  const second = await enrollDevice(cfg, 'f412fa87c9e0', '工位打印机');
  expect(second.reset).toBe(true);
  expect(second.device_key).not.toBe(first.device_key);
});

test('enroll 别人账号下的设备返回 409', async () => {
  const a = await login('13700007777');
  await enrollDevice({baseUrl: base, token: a.token}, 'aabbccddeeff', 'A 的设备');
  const b = await login('13700008888');
  try {
    await enrollDevice({baseUrl: base, token: b.token}, 'aabbccddeeff', '抢绑');
    throw new Error('本该 409');
  } catch (e) {
    const err = (e as ApiFailure).error;
    expect(err.kind).toBe('conflict');
    if (err.kind === 'conflict') expect(err.detail).toBe('device bound');
  }
});

test('注销账号后 token 失效且报告删除的作业数', async () => {
  const {token} = await login('13700009999');
  const cfg = {baseUrl: base, token};
  await enrollDevice(cfg, '112233445566', '待注销');
  const r = await deleteAccount(cfg);
  expect(r.ok).toBe(1);
  expect(typeof r.jobs_deleted).toBe('number');
  try {
    await listDevices(cfg);
    throw new Error('本该 401');
  } catch (e) {
    expect((e as ApiFailure).error.kind).toBe('unauthorized');
  }
});
