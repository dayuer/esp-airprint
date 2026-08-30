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

/** 显示用：138 0000 8888 */
export function formatPhone(raw: string): string {
  const digits = raw.replace(/\D/g, '').slice(0, 11);
  const parts = [digits.slice(0, 3), digits.slice(3, 7), digits.slice(7, 11)];
  return parts.filter(Boolean).join(' ');
}

export const CODE_LENGTH = 6;

export function isCompleteCode(code: string): boolean {
  return new RegExp(`^\\d{${CODE_LENGTH}}$`).test(code);
}
