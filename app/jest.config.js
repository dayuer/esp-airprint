module.exports = {
  preset: '@react-native/jest-preset',
  // __tests__/helpers 里放的是工具不是测试，所以只匹配 *.test.*
  testMatch: ['**/*.test.[jt]s?(x)'],
  setupFiles: ['<rootDir>/jest.setup.js'],
  // 这几个包发的是 ESM，要跟着一起过 babel。
  transformIgnorePatterns: [
    'node_modules/(?!(?:@react-native|react-native|@react-navigation|react-native-screens|react-native-safe-area-context|react-native-keychain)/)',
  ],
};
