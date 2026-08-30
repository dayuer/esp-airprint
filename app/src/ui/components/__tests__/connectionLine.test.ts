/**
 * @jest-environment node
 */
import {brokenSegment, isNodeLit} from '../ConnectionLine';

// 节点顺序：0=手机 1=云 2=桥 3=打印机
test('链路全通时没有断段', () => {
  expect(brokenSegment('none')).toBe(-1);
});

test('链路全通时一个节点都不点亮', () => {
  // 曾经的 bug：seg 为 -1 时 `i === seg + 1` 命中 i=0，「手机」被标成红的。
  // 类型和 linkBreakOf 的测试都不会报错，只有看屏幕才发现。
  expect([0, 1, 2, 3].map(i => isNodeLit('none', i))).toEqual([false, false, false, false]);
});

test('桥离线点亮云与桥', () => {
  expect(brokenSegment('bridge')).toBe(1);
  expect([0, 1, 2, 3].map(i => isNodeLit('bridge', i))).toEqual([false, true, true, false]);
});

test('没插打印机点亮桥与打印机', () => {
  expect(brokenSegment('printer')).toBe(2);
  expect([0, 1, 2, 3].map(i => isNodeLit('printer', i))).toEqual([false, false, true, true]);
});
