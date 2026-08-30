/**
 * @jest-environment node
 */
import {createSessionStore, SMS_COOLDOWN_SEC} from '../session';
import {memoryTokenStore} from '../tokenStore';
import {MOCK_CODE, MockHandle, startMock} from '../../api/__tests__/helpers/mock';

let mock: MockHandle;
beforeAll(async () => {
  mock = await startMock();
});
afterAll(() => mock.stop());

const newStore = (token: string | null = null) => {
  const store = memoryTokenStore(token);
  return {store, session: createSessionStore({baseUrl: mock.baseUrl, store})};
};

test('冷启动时没有 token 就是未登录', async () => {
  const {session} = newStore();
  expect(session.getState().phase).toBe('booting');
  await session.getState().bootstrap();
  expect(session.getState().phase).toBe('signedOut');
});

test('冷启动时钥匙串里有 token 就直接进已登录', async () => {
  const {session} = newStore('aaaaaaaaaaaa.bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb');
  await session.getState().bootstrap();
  expect(session.getState().phase).toBe('signedIn');
});

test('登录成功后 token 写进存储，重启还在', async () => {
  const {store, session} = newStore();
  await session.getState().bootstrap();
  await session.getState().requestCode('138 0000 1111');
  await session.getState().submitCode('13800001111', MOCK_CODE, 'jest');
  expect(session.getState().phase).toBe('signedIn');
  expect(session.getState().phoneTail).toBe('1111');
  expect(await store.load()).toBe(session.getState().token);

  const restarted = createSessionStore({baseUrl: mock.baseUrl, store});
  await restarted.getState().bootstrap();
  expect(restarted.getState().phase).toBe('signedIn');
});

test('手机号格式不对时不发请求，直接给错误', async () => {
  const {session} = newStore();
  await session.getState().requestCode('12345');
  expect(session.getState().lastError).toBe('手机号格式不对');
  expect(session.getState().smsCooldown).toBe(0);
});

test('发码成功后进入冷却', async () => {
  const {session} = newStore();
  await session.getState().requestCode('13800002222');
  expect(session.getState().smsCooldown).toBe(SMS_COOLDOWN_SEC);
});

test('429 也要进冷却并显示原因——不自动重试', async () => {
  const {session} = newStore();
  await session.getState().requestCode('13800003333');
  session.setState({smsCooldown: 0, lastError: null});
  await session.getState().requestCode('13800003333');   // 60 秒内第二次
  expect(session.getState().smsCooldown).toBe(SMS_COOLDOWN_SEC);
  expect(session.getState().lastError).toContain('60');
});

test('验证码错误给出可读文案，不进已登录', async () => {
  const {session} = newStore();
  await session.getState().bootstrap();
  await session.getState().requestCode('13800004444');
  await session.getState().submitCode('13800004444', '000000');
  expect(session.getState().phase).toBe('signedOut');
  expect(session.getState().lastError).toBe('登录已失效，请重新登录');
});

test('登出清掉存储里的 token', async () => {
  const {store, session} = newStore();
  await session.getState().bootstrap();
  await session.getState().requestCode('13800005555');
  await session.getState().submitCode('13800005555', MOCK_CODE);
  await session.getState().signOut();
  expect(session.getState().phase).toBe('signedOut');
  expect(await store.load()).toBeNull();
});

test('服务端登出失败也要清本地——否则用户被困在已登录态', async () => {
  const {store, session} = newStore();
  await session.getState().bootstrap();
  await session.getState().requestCode('13800006666');
  await session.getState().submitCode('13800006666', MOCK_CODE);

  // 换成一个连不上的地址模拟服务端不可达
  const offline = createSessionStore({baseUrl: 'http://127.0.0.1:1/api', store});
  offline.setState({token: session.getState().token, phase: 'signedIn'});
  await offline.getState().signOut();
  expect(offline.getState().phase).toBe('signedOut');
  expect(await store.load()).toBeNull();
});

test('冷却每秒递减到 0 就停住', () => {
  const {session} = newStore();
  session.setState({smsCooldown: 2});
  session.getState().tickCooldown();
  session.getState().tickCooldown();
  session.getState().tickCooldown();
  expect(session.getState().smsCooldown).toBe(0);
});
