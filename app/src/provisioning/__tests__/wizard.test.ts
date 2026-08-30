/**
 * @jest-environment node
 */
import {ApiFailure, enrollDevice, sendSms, verifySms} from '../../api';
import {completeProvisioning, confirmProvisioned, readPortal} from '../wizard';
import {MOCK_CODE, MockHandle, startMock} from '../../api/__tests__/helpers/mock';
import {FakePortal, startPortal} from './helpers/portal';

let cloud: MockHandle;
let api: {baseUrl: string; token: string};
let p: FakePortal | undefined;

beforeAll(async () => {
  cloud = await startMock();
  await sendSms({baseUrl: cloud.baseUrl}, '13600001111');
  const r = await verifySms({baseUrl: cloud.baseUrl}, '13600001111', MOCK_CODE);
  api = {baseUrl: cloud.baseUrl, token: r.token};
});
afterAll(() => cloud.stop());
afterEach(async () => {
  if (p) await p.stop();
  p = undefined;
});

test('读门户拿到设备 ID 和设备扫到的网络', async () => {
  p = await startPortal();
  const info = await readPortal({base: p.base});
  expect(info.dev).toBe('f412fa87c9e0');
  // 用设备扫到的，不是手机扫到的——设备的天线和位置跟手机不同。
  expect(info.networks.map(n => n.s)).toContain('办公室 5G');
});

test('老固件读不到设备 ID 时整个流程在第一步就停住', async () => {
  p = await startPortal({noDev: true});
  await expect(readPortal({base: p.base})).rejects.toMatchObject({
    error: {kind: 'malformed'},
  });
});

test('完整配网：enroll 换密钥、写进设备、等到连上', async () => {
  p = await startPortal({verifyMs: 100});
  const stages: string[] = [];
  const r = await completeProvisioning(
    api,
    {dev: 'f412fa87c9e0', ssid: '办公室 5G', pass: 'hunter2', name: '工位打印机'},
    {base: p.base, pollMs: 50, totalMs: 3000},
    s => stages.push(s),
  );
  expect(r.outcome).toEqual({kind: 'connected', ip: '192.168.1.37'});
  expect(r.reset).toBe(false);
  expect(stages).toEqual(['enrolling', 'sending', 'waiting']);

  // 密钥必须真的写进设备了，不然设备连上 Wi-Fi 也认证不了云端。
  const {lastBody} = await p.probe();
  expect(lastBody).toMatchObject({s: '办公室 5G', p: 'hunter2'});
  expect((lastBody as {k: string}).k).toMatch(/^[0-9a-f]{12}\.[A-Za-z0-9_-]{32}$/);
});

test('重复配同一台设备返回 reset，提示用户已重新绑定', async () => {
  p = await startPortal({verifyMs: 50});
  const input = {dev: 'aabbccdd1122', ssid: '办公室 5G', pass: 'x', name: '再配一次'};
  await completeProvisioning(api, input, {base: p.base, pollMs: 30, totalMs: 2000});
  await p.stop();

  p = await startPortal({verifyMs: 50});
  const again = await completeProvisioning(api, input, {
    base: p.base, pollMs: 30, totalMs: 2000,
  });
  expect(again.reset).toBe(true);
});

test('设备属于别人时 enroll 就被拒，凭据不会写进去', async () => {
  // 抢绑防护：写进去等于把一台不属于自己的设备指向自己的账号。
  const other = {baseUrl: cloud.baseUrl};
  await sendSms(other, '13600002222');
  const o = await verifySms(other, '13600002222', MOCK_CODE);
  await enrollDevice({baseUrl: cloud.baseUrl, token: o.token}, 'eeff00112233', '别人的');

  p = await startPortal();
  await expect(
    completeProvisioning(
      api,
      {dev: 'eeff00112233', ssid: 'x', pass: '', name: '抢'},
      {base: p.base, pollMs: 30, totalMs: 500},
    ),
  ).rejects.toMatchObject({error: {kind: 'conflict'}});

  const {connectCalls} = await p.probe();
  expect(connectCalls).toBe(0);
});

test('结果是 lost 时，设备列表能给出确定答案', async () => {
  // 门户在设备重启后就消失了，从那边看不出「配好了」和「掉电了」的区别。
  p = await startPortal({verifyMs: 50, rebootMs: 0});
  const r = await completeProvisioning(
    api,
    {dev: 'ccddeeff0011', ssid: '办公室 5G', pass: 'x', name: '仓库机'},
    {base: p.base, pollMs: 200, totalMs: 3000, lostAfter: 2, timeoutMs: 300},
  );
  expect(r.outcome.kind).toBe('lost');

  // enroll 成功过，所以设备已经在服务端了——这就是那个确定答案。
  await expect(confirmProvisioned(api, 'ccddeeff0011')).resolves.toBe(true);
  await expect(confirmProvisioned(api, '000000000000')).resolves.toBe(false);
});

test('未登录时确认不会抛，只是回答不了', async () => {
  await expect(confirmProvisioned({baseUrl: cloud.baseUrl}, 'f412fa87c9e0'))
    .resolves.toBe(false);
});

test('密钥必须在连热点之前申请到手', async () => {
  // 顺序错了不会有编译错误，也不会在有蜂窝的手机上失败——只会在没 SIM、
  // 没开数据、或者 Wi-Fi-only 的设备上调不通 enroll，而那时候用户看到的
  // 只是一句「申请密钥时连不上服务器」，完全指不到顺序上。
  p = await startPortal({verifyMs: 50});
  const order: string[] = [];

  await completeProvisioning(
    api,
    {ssid: '办公室 5G', pass: 'x', name: '顺序测试'},   // 不带 dev
    {base: p.base, pollMs: 30, totalMs: 2000},
    s => order.push(`stage:${s}`),
    async () => {
      order.push('join');
    },
  );

  expect(order).toEqual([
    'stage:enrolling',   // 先拿密钥——此时手机还在自己的网络上
    'stage:joining',
    'join',              // 唯一一次连热点
    'stage:sending',     // 然后才写凭据
    'stage:waiting',
  ]);
});

test('不带 dev 也能配网——密钥待认领，绑定发生在设备首次连 MQTT 时', async () => {
  // 这是整个「只连一次热点」的前提：enroll 不需要知道 MAC。
  // 服务端本来就能从设备自报的 username 学到它。
  p = await startPortal({verifyMs: 50});
  const r = await completeProvisioning(
    api,
    {ssid: '办公室 5G', pass: 'hunter2', name: '待认领'},
    {base: p.base, pollMs: 30, totalMs: 2000},
  );
  expect(r.outcome.kind).toBe('connected');

  const {lastBody} = await p.probe();
  expect((lastBody as {k: string}).k).toMatch(/^[0-9a-f]{12}\.[A-Za-z0-9_-]{32}$/);
});

test('enroll 失败时根本不去连热点，也不会写任何凭据', async () => {
  p = await startPortal();
  let rejoined = false;
  const badApi = {baseUrl: cloud.baseUrl};   // 没有 token

  await expect(
    completeProvisioning(
      badApi,
      {dev: '112233445566', ssid: 'x', pass: '', name: 'y'},
      {base: p.base, pollMs: 30, totalMs: 500},
      undefined,
      async () => {
        rejoined = true;
      },
    ),
  ).rejects.toBeDefined();

  expect(rejoined).toBe(false);
  const {connectCalls} = await p.probe();
  expect(connectCalls).toBe(0);
});
