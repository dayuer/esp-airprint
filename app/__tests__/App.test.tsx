/**
 * 冒烟测试：把整个 App 渲染一次。
 *
 * 它不断言任何界面内容——那种断言会在每次调文案时假红。它只保证
 * 「App 能挂起来」：import 没写错、组件没写错、样式对象没写错。
 * 这三类错在真机上表现为白屏或红屏，在这里是一条明确的失败。
 */
import React from 'react';
import ReactTestRenderer from 'react-test-renderer';
import App from '../App';

test('整个 App 能渲染，不抛异常', async () => {
  await ReactTestRenderer.act(() => {
    ReactTestRenderer.create(<App />);
  });
});
