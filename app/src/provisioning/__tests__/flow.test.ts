/**
 * @jest-environment node
 */
import {ConnectOutcome, describeOutcome, provisionDevice, waitForConnectResult} from '../flow';
import {FakePortal, startPortal} from './helpers/portal';

let p: FakePortal | undefined;
afterEach(async () => {
  if (p) await p.stop();
  p = undefined;
});

const CREDS = {ssid: '办公室 5G', pass: 'hunter2', devKey: 'a3f91c04bd77.kJ8x'};

test('设备连上后拿到 connected 和分到的 IP', async () => {
  p = await startPortal({verifyMs: 100});
  const r = await provisionDevice(CREDS, {base: p.base, pollMs: 50, totalMs: 3000});
  expect(r).toEqual({kind: 'connected', ip: '192.168.1.37'});
});

test('密码错时拿到 failed 和设备给的原因', async () => {
  p = await startPortal({fail: true, verifyMs: 100});
  const r = await provisionDevice(CREDS, {base: p.base, pollMs: 50, totalMs: 3000});
  expect(r.kind).toBe('failed');
  if (r.kind === 'failed') expect(r.reason).toContain('AUTH_FAIL');
});

test('错过那 3 秒窗口时返回 lost，不猜成功也不猜失败', async () => {
  // 固件：验证通过 → st=1 → 3 秒后 esp_restart()，热点消失。
  // 这里把重启压到 0ms，模拟 App 被切到后台、正好没读到 st=1 的情况。
  // 我们看到的只有「门户不见了」——那既可能是配好了，也可能是设备掉电。
  p = await startPortal({verifyMs: 50, rebootMs: 0});
  const r = await provisionDevice(CREDS, {
    base: p.base, pollMs: 200, totalMs: 5000, lostAfter: 2, timeoutMs: 300,
  });
  expect(r.kind).toBe('lost');
});

test('一次都没连上门户时是 timeout 而不是 lost', async () => {
  // 手机压根没在配网热点上。这跟「连上过又失联」是两回事，
  // 给用户的下一步也不一样：前者去连热点，后者去看设备列表。
  const r = await waitForConnectResult({
    base: 'http://127.0.0.1:1', pollMs: 20, totalMs: 200, lostAfter: 2,
  });
  expect(r.kind).toBe('timeout');
});

test('还在试连时不提前下结论', async () => {
  p = await startPortal({verifyMs: 10_000});   // 一直停在 st=0
  const r = await waitForConnectResult({base: p.base, pollMs: 30, totalMs: 300});
  expect(r.kind).toBe('timeout');
});

test('轮询用注入的时钟，不真的等', async () => {
  // 真实参数是 pollMs 500 / totalMs 40000。用真时钟测这个要等 40 秒。
  let t = 0;
  const r = await waitForConnectResult({
    base: 'http://127.0.0.1:1',
    now: () => t,
    sleep: async ms => {
      t += ms;
    },
  });
  expect(r.kind).toBe('timeout');
  expect(t).toBeGreaterThanOrEqual(40_000);
});

test('四种结果都有给用户看的一句话和下一步', () => {
  const all: ConnectOutcome[] = [
    {kind: 'connected', ip: '192.168.1.37'},
    {kind: 'failed', reason: 'AUTH_FAIL(202)'},
    {kind: 'lost'},
    {kind: 'timeout'},
  ];
  for (const o of all) {
    const d = describeOutcome(o);
    expect(d.text.length).toBeGreaterThan(0);
    expect(d.nextStep.length).toBeGreaterThan(0);
  }
});

test('lost 的文案要说不知道，并指向唯一的权威答案', () => {
  const d = describeOutcome({kind: 'lost'});
  // 猜成功会让用户以为配好了然后发现打不了印；猜失败会让他把一台已经
  // 配好的设备重配一遍。设备列表是唯一能回答这个问题的地方。
  expect(d.text).toContain('可能');
  expect(d.nextStep).toContain('设备列表');
});
