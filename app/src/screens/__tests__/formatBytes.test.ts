/**
 * @jest-environment node
 */
import {formatBytes} from '../DeviceDetailScreen';

// URF 是光栅：纯文字页 200KB~1MB，整页照片 5~15MB（文档第 6 节第 5 条）。
// 按字节显示没人看得懂，而量纲是用户判断「这单要不要用流量传」的唯一依据。
test('小于 1KB 显示字节', () => {
  expect(formatBytes(0)).toBe('0 B');
  expect(formatBytes(552)).toBe('552 B');
});

test('KB 不留小数', () => {
  expect(formatBytes(1024)).toBe('1 KB');
  expect(formatBytes(284160)).toBe('278 KB');
});

test('MB 留一位小数', () => {
  expect(formatBytes(1024 * 1024)).toBe('1.0 MB');
  expect(formatBytes(15 * 1024 * 1024)).toBe('15.0 MB');
  expect(formatBytes(209715200)).toBe('200.0 MB');   // 服务端上限
});
