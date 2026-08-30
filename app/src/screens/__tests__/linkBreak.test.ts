/**
 * @jest-environment node
 */
import {DeviceListItem} from '../../api';
import {linkBreakOf} from '../DeviceListScreen';

const base: DeviceListItem = {
  dev: 'f412fa87c9e0', name: '工位打印机', online: true, seen: 0,
  state: 'ready', bound: 0, queued_jobs: 0,
  printer: {serial: 'CNB9K1P2X4', make: 'HP', model: 'HP Laser MFP 136a', attached: true},
};

// 服务端的 offline 既表示「桥断了」也表示「桥在但没插打印机」，它不区分。
// 用户必须能区分：前者要去看设备，后者只要把 USB 插上。
test('全通时不断', () => {
  expect(linkBreakOf(base)).toBe('none');
});

test('桥离线断在第三段', () => {
  expect(linkBreakOf({...base, online: false})).toBe('bridge');
});

test('桥在线但没插打印机断在第四段', () => {
  expect(linkBreakOf({...base, printer: null})).toBe('printer');
});

test('打印机记录还在但 attached 为 false 也算没插', () => {
  expect(linkBreakOf({
    ...base, printer: {...base.printer!, attached: false},
  })).toBe('printer');
});

test('桥离线优先于打印机——先修外层那一环', () => {
  expect(linkBreakOf({...base, online: false, printer: null})).toBe('bridge');
});
