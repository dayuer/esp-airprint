/**
 * @jest-environment node
 */
import {ProfileSrc} from '../types';
import {describeProfileSrc, shouldSuggestTest} from '../profileSrc';

const ALL: ProfileSrc[] = ['serial', 'model', 'authoritative', 'quirks', 'default'];

test('每种来源都有面向用户的文案', () => {
  for (const src of ALL) {
    expect(describeProfileSrc(src).length).toBeGreaterThan(0);
  }
});

test('文案逐条对上文档 4.7 的表', () => {
  expect(describeProfileSrc('serial')).toBe('已针对你的打印机校准');
  expect(describeProfileSrc('model')).toBe('已验证的机型配置');
  expect(describeProfileSrc('authoritative')).toBe('官方配置');
  expect(describeProfileSrc('quirks')).toBe('通用配置，建议做一次测试');
  expect(describeProfileSrc('default')).toBe('未适配，建议做一次测试');
});

test('quirks 和 default 要提示做测试，已校准的不用', () => {
  expect(shouldSuggestTest('quirks', false)).toBe(true);
  expect(shouldSuggestTest('default', false)).toBe(true);
  expect(shouldSuggestTest('serial', false)).toBe(false);
  expect(shouldSuggestTest('model', false)).toBe(false);
  expect(shouldSuggestTest('authoritative', false)).toBe(false);
});

test('disputed 时无论哪一层都要提示——同型号出现过相反结论', () => {
  for (const src of ALL) {
    expect(shouldSuggestTest(src, true)).toBe(true);
  }
});
