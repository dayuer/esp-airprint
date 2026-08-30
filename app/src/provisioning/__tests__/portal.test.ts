/**
 * @jest-environment node
 */
import {
  PortalFailure, getPortalStatus, isSetupSsid, readDeviceId, scanNetworks, sendCredentials,
} from '../portal';
import {FakePortal, startPortal} from './helpers/portal';

let p: FakePortal | undefined;
afterEach(async () => {
  if (p) await p.stop();
  p = undefined;   // 不置空的话，没起门户的测试会去 stop 一个已经退出的进程
});

test('认得出配网热点，也认得出改名前后的两个前缀', () => {
  expect(isSetupSsid('StickBox-Setup-C9E0')).toBe(true);
  expect(isSetupSsid('AirPrint-Setup-C9E0')).toBe(true);
  expect(isSetupSsid('StickBox-Setup-c9e0')).toBe(false);   // 固件用大写
  expect(isSetupSsid('StickBox-Setup-C9E')).toBe(false);    // 后缀是 4 位
  expect(isSetupSsid('办公室 5G')).toBe(false);
  expect(isSetupSsid('StickBox')).toBe(false);
});

test('扫描结果按信号从强到弱', async () => {
  p = await startPortal();
  const list = await scanNetworks({base: p.base});
  expect(list.map(n => n.s)).toEqual(['办公室 5G', 'ChinaNet-8x2k', 'Guest']);
  expect(list[0].r).toBeGreaterThan(list[2].r);
  expect(list[2].k).toBe(0);   // 开放网络
});

test('读得到设备 ID', async () => {
  p = await startPortal();
  await expect(readDeviceId({base: p.base})).resolves.toBe('f412fa87c9e0');
});

test('老固件没有 dev 字段时报错，绝不编一个出来', async () => {
  // 编一个的后果：enroll 到一台不存在的设备上，密钥写进去也连不上，
  // 而用户完全看不出问题在哪。见 API 文档 5.0b 的缺口。
  p = await startPortal({noDev: true});
  await expect(readDeviceId({base: p.base})).rejects.toMatchObject({
    error: {kind: 'malformed'},
  });
  // 但 /status 本身还是能读的——试连结果不依赖 dev。
  await expect(getPortalStatus({base: p.base})).resolves.toMatchObject({st: 0});
});

test('凭据按固件认得的三个单字母字段发出去', async () => {
  p = await startPortal();
  await sendCredentials(
    {ssid: '办公室 5G', pass: 'hunter2', devKey: 'a3f91c04bd77.kJ8x'},
    {base: p.base},
  );
  // 固件用的是极简 JSON 取值，字段名写错它就当空——而空 SSID 会被直接拒。
  const {lastBody} = await p.probe();
  expect(lastBody).toEqual({s: '办公室 5G', p: 'hunter2', k: 'a3f91c04bd77.kJ8x'});
});

test('SSID 为空时设备回 ok:0，客户端要报错而不是当成功', async () => {
  p = await startPortal();
  await expect(
    sendCredentials({ssid: '', pass: '', devKey: ''}, {base: p.base}),
  ).rejects.toBeInstanceOf(PortalFailure);
});

test('连不上门户是 unreachable，不是崩溃', async () => {
  await expect(getPortalStatus({base: 'http://127.0.0.1:1'})).rejects.toMatchObject({
    error: {kind: 'unreachable'},
  });
});

test('超时和连不上要分开——给用户的下一步不一样', async () => {
  // SoftAP 没有互联网，路由错了（走了蜂窝）请求会一直挂着，
  // 那是「超时」；没连上热点是「连不上」。
  // 桩要像真 fetch 一样响应 abort 信号——不响应的桩永远不 reject，
  // 那验的就不是超时逻辑而是桩自己的行为。
  const hang: typeof fetch = (_url, init) =>
    new Promise((_resolve, reject) => {
      init?.signal?.addEventListener('abort', () => {
        const e = new Error('aborted');
        e.name = 'AbortError';
        reject(e);
      });
    });
  await expect(
    getPortalStatus({base: 'http://192.168.4.1', timeoutMs: 30, fetchImpl: hang}),
  ).rejects.toMatchObject({error: {kind: 'timeout'}});
});

test('回的不是 JSON 时报 malformed，并带上开头的内容', async () => {
  // 连到了运营商劫持页或别的设备时会这样。
  const html: typeof fetch = async () => new Response('<html>需要登录</html>', {status: 200});
  await expect(
    getPortalStatus({base: 'http://192.168.4.1', fetchImpl: html}),
  ).rejects.toMatchObject({error: {kind: 'malformed', detail: expect.stringContaining('html')}});
});
