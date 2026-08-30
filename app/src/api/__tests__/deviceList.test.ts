/**
 * @jest-environment node
 */
import {ApiFailure} from '../http';
import {fetchDeviceList, memoryKnownDevices} from '../deviceList';
import {enrollDevice, sendSms, verifySms} from '../auth';
import {MOCK_CODE, MockHandle, bringOnline, startMock} from './helpers/mock';

let mock: MockHandle;
let cfg: {baseUrl: string; token: string};

beforeAll(async () => {
  mock = await startMock();
  const anon = {baseUrl: mock.baseUrl};
  await sendSms(anon, '13500001111');
  const {token} = await verifySms(anon, '13500001111', MOCK_CODE);
  cfg = {baseUrl: mock.baseUrl, token};
  await enrollDevice(cfg, 'f412fa87c9e0', '工位打印机');
});
afterAll(() => mock.stop());

test('端点存在时直接用它，并顺手对齐本地记录', async () => {
  const known = memoryKnownDevices();
  const r = await fetchDeviceList(cfg, known);
  expect(r.degraded).toBe(false);
  expect(r.devices.map(d => d.dev)).toEqual(['f412fa87c9e0']);
  // 对齐之后，将来真降级时本地记录也是新的。
  await expect(known.load()).resolves.toEqual(['f412fa87c9e0']);
});

/** 一个没有 4.5b 的服务端：/devices 一律 404，其余照常。 */
function serverWithout45b(base: string): typeof fetch {
  return async (input, init) => {
    const url = typeof input === 'string' ? input : String(input);
    if (url.endsWith('/devices')) {
      return new Response(JSON.stringify({e: 'not found'}), {status: 404});
    }
    return fetch(url.replace(/^[^]*?\/api/, `${base}`), init);
  };
}

test('服务端没有 4.5b 时降级到逐台查 status，并明说降级了', async () => {
  const old = {...cfg, fetchImpl: serverWithout45b(`${mock.baseUrl}`)};
  const known = memoryKnownDevices(['f412fa87c9e0']);

  const r = await fetchDeviceList(old, known);
  // degraded 要明说：悄悄兜底的话，用户换手机之后才发现设备全没了，
  // 而那时候完全看不出是服务端的问题。
  expect(r.degraded).toBe(true);
  expect(r.devices.map(d => d.dev)).toEqual(['f412fa87c9e0']);
});

test('降级时本地没记录就是空列表，不报错', async () => {
  const old = {...cfg, fetchImpl: serverWithout45b(`${mock.baseUrl}`)};
  const r = await fetchDeviceList(old, memoryKnownDevices());
  expect(r.degraded).toBe(true);
  expect(r.devices).toEqual([]);
});

test('降级时某一台查不通，不该让整个列表空掉', async () => {
  const old = {...cfg, fetchImpl: serverWithout45b(`${mock.baseUrl}`)};
  const known = memoryKnownDevices(['f412fa87c9e0', '000000000000']);
  const r = await fetchDeviceList(old, known);
  expect(r.devices.map(d => d.dev)).toEqual(['f412fa87c9e0']);
});

test('401 照常抛出去，不能被当成降级', async () => {
  // token 失效要让会话作废回登录页。当成「服务端没这个端点」的话，
  // 用户会看到一个空列表，永远不知道自己该重新登录。
  const bad = {baseUrl: mock.baseUrl, token: 'aaaaaaaaaaaa.bogus'};
  await expect(fetchDeviceList(bad, memoryKnownDevices())).rejects.toMatchObject({
    error: {kind: 'unauthorized'},
  });
});

test('网络不通照常抛出去，不能被当成降级', async () => {
  // 一次断网看起来像一次服务端降级的话，用户会以为要重新配网。
  const dead = {baseUrl: 'http://127.0.0.1:1/api'};
  await expect(fetchDeviceList(dead, memoryKnownDevices())).rejects.toBeInstanceOf(
    ApiFailure,
  );
});

test('降级列表里的在线状态是真的', async () => {
  await bringOnline(mock.baseUrl, cfg.token, 'f412fa87c9e0');
  const old = {...cfg, fetchImpl: serverWithout45b(`${mock.baseUrl}`)};
  const r = await fetchDeviceList(old, memoryKnownDevices(['f412fa87c9e0']));
  expect(r.devices[0].online).toBe(true);
});
