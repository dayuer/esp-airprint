import {spawn} from 'node:child_process';
import path from 'node:path';

export interface MockHandle {
  baseUrl: string;
  stop(): Promise<void>;
}

/**
 * 起一个真的 mock server 子进程，打真 HTTP。
 *
 * 不用 in-process 的 fetch 打桩：那样测的是「client 和自己的想象一致」，
 * 而不是「client 和一个按文档实现的服务端一致」。URF 那一轮的教训——
 * 34 个测试自洽地验证了一个错误的格式。
 */
export function startMock(): Promise<MockHandle> {
  // 服务端可用之后，拿同一套契约测试打真服务端，不要再写一套。
  const external = process.env.API_BASE;
  if (external) {
    return Promise.resolve({baseUrl: external, stop: async () => {}});
  }
  const entry = path.resolve(__dirname, '../../../../mock-server/server.mjs');
  return new Promise((resolve, reject) => {
    const child = spawn(process.execPath, [entry, '0'], {stdio: ['ignore', 'pipe', 'pipe']});
    let out = '';
    const timer = setTimeout(
      () => reject(new Error(`mock server 起不来:\n${out}`)),
      15_000,
    );
    child.stdout.on('data', d => {
      out += String(d);
      const m = /http:\/\/127\.0\.0\.1:(\d+)\/api/.exec(out);
      if (!m) return;
      clearTimeout(timer);
      resolve({
        baseUrl: `http://127.0.0.1:${m[1]}/api`,
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

/** mock 的验证码固定值，对应文档第 7 节「开发环境验证码只打日志」。 */
export const MOCK_CODE = '123456';
