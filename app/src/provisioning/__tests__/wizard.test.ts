/**
 * @jest-environment node
 */
import {ApiFailure, enrollDevice, sendSms, verifySms} from '../../api';
import {confirmProvisioned, enrollForProvisioning, readPortal, writeAndWait} from '../wizard';
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

test('完整配网：先拿密钥，再连热点写入', async () => {
  p = await startPortal({verifyMs: 100});
  const stages: string[] = [];

  // ① 拿密钥——此时手机还在自己的网络上。
  const key = await enrollForProvisioning(api, '工位打印机');
  expect(key.deviceKey).toMatch(/^[0-9a-f]{12}\.[A-Za-z0-9_-]{32}$/);
  expect(key.reset).toBe(false);

  // ② 之后才连热点写入。
  const outcome = await writeAndWait(
    {ssid: '办公室 5G', pass: 'hunter2', devKey: key.deviceKey},
    {base: p.base, pollMs: 50, totalMs: 3000},
    s => stages.push(s),
  );
  expect(outcome).toEqual({kind: 'connected', ip: '192.168.1.37'});
  expect(stages).toEqual(['sending', 'waiting']);

  // 密钥必须真的写进设备了，不然设备连上 Wi-Fi 也认证不了云端。
  const {lastBody} = await p.probe();
  expect(lastBody).toEqual({s: '办公室 5G', p: 'hunter2', k: key.deviceKey});
});

test('拿密钥不需要知道设备 MAC——这是「只连一次热点」的前提', async () => {
  // enroll 不带 dev，签发一把待认领的密钥。绑定发生在设备首次连 MQTT 时：
  // 服务端从设备自报的 username 学到 MAC。
  //
  // 要是 enroll 非要 dev 不可，就必须先连热点去读，而连上热点手机就断网了
  // ——那正是这个流程一开始踩的坑。
  const key = await enrollForProvisioning(api, '不知道 MAC 也能拿');
  expect(key.deviceKey).toBeTruthy();
});

test('写入阶段不再申请密钥——它只写不问', async () => {
  // writeAndWait 拿到的是已经在手上的密钥。这条守的是「密钥前置」：
  // 一旦有人把 enroll 挪回写入阶段，手机那时已经连在热点上、没有互联网，
  // 而失败信息只会是一句「连不上服务器」，指不到顺序上。
  p = await startPortal({verifyMs: 50});
  const stages: string[] = [];
  await writeAndWait(
    {ssid: '办公室 5G', pass: 'x', devKey: 'aaaaaaaaaaaa.bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb'},
    {base: p.base, pollMs: 30, totalMs: 2000},
    s => stages.push(s),
  );
  // 只有写和等两个阶段，没有 enrolling。
  expect(stages).toEqual(['sending', 'waiting']);
});

test('结果是 lost 时，设备列表能给出确定答案', async () => {
  // 门户在设备重启后就消失了，从那边看不出「配好了」和「掉电了」的区别。
  p = await startPortal({verifyMs: 50, rebootMs: 0});
  const key = await enrollForProvisioning(api, '仓库机');
  const outcome = await writeAndWait(
    {ssid: '办公室 5G', pass: 'x', devKey: key.deviceKey},
    {base: p.base, pollMs: 200, totalMs: 3000, lostAfter: 2, timeoutMs: 300},
  );
  expect(outcome.kind).toBe('lost');
  // 密钥已经签发，但设备还没认领——列表里查不到，这就是那个确定答案。
  await expect(confirmProvisioned(api, '000000000000')).resolves.toBe(false);
});

test('未登录时确认不会抛，只是回答不了', async () => {
  await expect(confirmProvisioned({baseUrl: cloud.baseUrl}, 'f412fa87c9e0'))
    .resolves.toBe(false);
});
