import {spawn} from 'node:child_process';
import path from 'node:path';

export interface FakePortal {
  base: string;
  /** 设备收到了什么。真固件没有这个端点，它只给测试用。 */
  probe(): Promise<{connectCalls: number; lastBody: unknown}>;
  stop(): Promise<void>;
}

export interface PortalArgs {
  /** 老固件不返回 dev。 */
  noDev?: boolean;
  /** 设备连不上目标网络。 */
  fail?: boolean;
  /** 多久出结果。 */
  verifyMs?: number;
  /** 出结果后多久重启（固件是 3000）。设成很小可以模拟「错过那 3 秒窗口」。 */
  rebootMs?: number;
}

/**
 * 起一个按 main/provision.c 行为写的假门户，打真 HTTP。
 *
 * 不打桩 fetch：这里要验的恰恰是「设备重启之后 fetch 会怎样」，
 * 打桩就等于自己规定了答案。
 */
export function startPortal(a: PortalArgs = {}): Promise<FakePortal> {
  const entry = path.resolve(__dirname, '../../../../mock-server/portal.mjs');
  const argv = [entry, '0'];
  if (a.noDev) argv.push('--no-dev');
  if (a.fail) argv.push('--fail');
  if (a.verifyMs !== undefined) argv.push(`--verify-ms=${a.verifyMs}`);
  if (a.rebootMs !== undefined) argv.push(`--reboot-ms=${a.rebootMs}`);

  return new Promise((resolve, reject) => {
    const child = spawn(process.execPath, argv, {stdio: ['ignore', 'pipe', 'pipe']});
    let out = '';
    const timer = setTimeout(() => reject(new Error(`假门户起不来:\n${out}`)), 10_000);
    child.stdout.on('data', d => {
      out += String(d);
      const m = /http:\/\/127\.0\.0\.1:(\d+)/.exec(out);
      if (!m) return;
      clearTimeout(timer);
      const base = `http://127.0.0.1:${m[1]}`;
      resolve({
        base,
        probe: async () => (await fetch(`${base}/__probe`)).json(),
        stop: () =>
          new Promise<void>(done => {
            child.once('exit', () => done());
            child.kill();
          }),
      });
    });
    child.stderr.on('data', d => (out += String(d)));
    child.on('error', e => {
      clearTimeout(timer);
      reject(e);
    });
  });
}
