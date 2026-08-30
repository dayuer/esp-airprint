/**
 * 服务端地址。
 *
 * 服务端还在写，开发期指向本地 mock（npm run mock）。
 * Android 模拟器用 10.0.2.2 回连宿主机；真机要改成宿主机在局域网里的 IP。
 */
// Android 模拟器用 10.0.2.2 回连宿主机；真机要用宿主机在局域网里的 IP。
// 改这一行就能切到别的开发机或真服务端。
const DEV_API = 'http://192.168.3.237:9443/api';

export const API_BASE_URL = __DEV__ ? DEV_API : 'https://mqtt.silkline.id:9443/api';
