/**
 * 服务端地址。
 *
 * 服务端还在写，开发期指向本地 mock（npm run mock）。
 * Android 模拟器用 10.0.2.2 回连宿主机；真机要改成宿主机在局域网里的 IP。
 */
// iOS 模拟器与本机同网络栈，直接 127.0.0.1 就行。
// Android 模拟器要用 10.0.2.2；Android 真机走 `adb reverse tcp:9443 tcp:9443`
// 之后也能用 127.0.0.1，省得每换一个网络就改这里的 IP。
const DEV_API = 'http://127.0.0.1:9443/api';

export const API_BASE_URL = __DEV__ ? DEV_API : 'https://mqtt.silkline.id:9443/api';
