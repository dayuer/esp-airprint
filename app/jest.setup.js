// 原生模块在 jest 里不存在。桩掉它们，好让整个 App 能被渲染一次——
// 那一次渲染能抓到 import 错、组件写错、样式对象写错，成本几乎为零。

jest.mock('react-native-keychain', () => ({
  getGenericPassword: jest.fn(async () => false),
  setGenericPassword: jest.fn(async () => true),
  resetGenericPassword: jest.fn(async () => true),
  ACCESSIBLE: {WHEN_UNLOCKED_THIS_DEVICE_ONLY: 'whenUnlockedThisDeviceOnly'},
}));

jest.mock('react-native-safe-area-context', () => {
  const inset = {top: 0, right: 0, bottom: 0, left: 0};
  const React = require('react');
  return {
    SafeAreaProvider: ({children}) => React.createElement(React.Fragment, null, children),
    useSafeAreaInsets: () => inset,
  };
});
