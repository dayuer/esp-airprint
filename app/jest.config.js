module.exports = {
  preset: '@react-native/jest-preset',
  // __tests__/helpers 里放的是工具不是测试，所以只匹配 *.test.*
  testMatch: ['**/*.test.[jt]s?(x)'],
};
