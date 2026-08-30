/**
 * @jest-environment node
 */
import {formatPhone, isCompleteCode, isValidPhone, normalizePhone, toDigits} from '../phone';

test('规整掉空格、连字符和 +86 前缀', () => {
  for (const raw of ['13800008888', '138 0000 8888', '138-0000-8888',
                     '+8613800008888', '86 138 0000 8888']) {
    expect(normalizePhone(raw)).toBe('13800008888');
  }
});

test('非大陆号码格式返回 null', () => {
  for (const raw of ['12345', '12800008888', '1380000888', '138000088888', 'abcdefghijk']) {
    expect(normalizePhone(raw)).toBeNull();
  }
});

test('toDigits 只留数字且最多 11 位', () => {
  // 受控输入的 state 只存它的结果。存格式化字符串再重新格式化，
  // 快速输入时原生文本和 JS state 会分叉——iOS 上 11 位只进得去 9 位。
  expect(toDigits('139 0000 1111')).toBe('13900001111');
  expect(toDigits('1390000111199999')).toBe('13900001111');
  expect(toDigits('a1b3c9')).toBe('139');
});

test('格式化成 3-4-4', () => {
  expect(formatPhone('13800008888')).toBe('138 0000 8888');
  expect(formatPhone('138')).toBe('138');
  expect(formatPhone('1380')).toBe('138 0');
  expect(formatPhone('')).toBe('');
});

test('格式化是幂等的——反复喂回自己不会变形', () => {
  const once = formatPhone('13800008888');
  expect(formatPhone(once)).toBe(once);
});

test('验证码必须是 6 位数字', () => {
  expect(isCompleteCode('123456')).toBe(true);
  expect(isCompleteCode('12345')).toBe(false);
  expect(isCompleteCode('1234567')).toBe(false);
  expect(isCompleteCode('12345a')).toBe(false);
});

test('isValidPhone 接受带空格的输入', () => {
  expect(isValidPhone('138 0000 8888')).toBe(true);
  expect(isValidPhone('138 0000 888')).toBe(false);
});
