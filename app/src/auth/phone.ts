/**
 * 手机号规整与校验。服务端只接受中国大陆号码（docs/API-cloud-print.md 4.1b），
 * `+86` / `86` 前缀和空格会被规整掉——客户端先做一遍，别让用户等一个网络往返
 * 才知道号码格式不对。
 */

/** 规整成 11 位，规整不出来返回 null。 */
export function normalizePhone(raw: string): string | null {
  const s = raw.replace(/[\s-]/g, '').replace(/^\+?86/, '');
  return /^1[3-9]\d{9}$/.test(s) ? s : null;
}

export function isValidPhone(raw: string): boolean {
  return normalizePhone(raw) !== null;
}

export const PHONE_DIGITS = 11;

/**
 * 从任意输入里取出数字，最多 11 位。
 *
 * 受控输入的 state 只存它的结果，**不要存格式化后的字符串**——存格式化结果
 * 再重新格式化，快速输入时原生文本和 JS state 会分叉，字符会丢。
 * iOS 上用 simctl 注入文本时 11 位只进得去 9 位，就是这么来的。
 */
export function toDigits(input: string): string {
  return input.replace(/\D/g, '').slice(0, PHONE_DIGITS);
}

/** 显示用：138 0000 8888 */
export function formatPhone(raw: string): string {
  const digits = toDigits(raw);
  const parts = [digits.slice(0, 3), digits.slice(3, 7), digits.slice(7, 11)];
  return parts.filter(Boolean).join(' ');
}

export const CODE_LENGTH = 6;

export function isCompleteCode(code: string): boolean {
  return new RegExp(`^\\d{${CODE_LENGTH}}$`).test(code);
}
