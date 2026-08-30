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

// ---- 当前插着 vs 最近插过 ----

import {isPrinterAttached} from '../DeviceDetailScreen';

test('serial 对上才算插着', () => {
  expect(isPrinterAttached('CNB9K1P2X4', 'CNB9K1P2X4')).toBe(true);
});

test('没插打印机时 status.serial 是空串——此时拿到的档案是「上次插的那台」', () => {
  // 服务端的 /printer 在没插时会退回最近插过的那台。把它当成当前插着显示，
  // 用户就会照着一台不在场的打印机排查，而设备面板上写的是「打印机没连接」。
  expect(isPrinterAttached('', 'CNB9K1P2X4')).toBe(false);
  expect(isPrinterAttached(undefined, 'CNB9K1P2X4')).toBe(false);
});

test('换了一台打印机时不算插着', () => {
  expect(isPrinterAttached('VNC7J2Q9Y1', 'CNB9K1P2X4')).toBe(false);
});

test('两边都没有时不算插着', () => {
  expect(isPrinterAttached('', '')).toBe(false);
  expect(isPrinterAttached(undefined, undefined)).toBe(false);
});
