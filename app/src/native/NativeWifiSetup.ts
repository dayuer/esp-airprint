import type {TurboModule} from 'react-native';
import {TurboModuleRegistry} from 'react-native';

/**
 * 加入设备的配网热点，并把门户请求绑到那个热点上。
 *
 * 为什么要原生做而不是让用户去系统设置里连：
 *
 * 1. **captive portal 劫持。** 固件把 /generate_204 之类的联网检测地址重定向到
 *    配网页，Android 一连上就弹「登录到网络」，用浏览器盖在 App 上面。
 * 2. **默认路由被抢走。** 系统认为这个 Wi-Fi 需要登录，会把默认网络留在它上面，
 *    而配网热点没有互联网——enroll 那一步就调不到服务端。
 *
 * `WifiNetworkSpecifier` 两个问题一起解决：它申请的是**局部网络**，系统不弹
 * captive portal，也不动默认路由，手机继续用蜂窝或家里的 Wi-Fi 上网；
 * 只有显式绑到这个网络的 socket 才走热点。
 */
export interface PortalResponse {
  status: number;
  body: string;
}

export interface Spec extends TurboModule {
  /**
   * 申请加入前缀匹配的配网热点。系统会弹一个只列出匹配网络的选择框，
   * 用户点一下确认——这一步 Android 10+ 无法省略，也不该省略。
   * 返回实际加入的 SSID。
   */
  joinSetupNetwork(ssidPrefix: string, timeoutMs: number): Promise<string>;

  /** 通过绑定到配网热点的连接发请求。url 是完整地址。 */
  portalRequest(
    method: string,
    url: string,
    body: string | null,
    timeoutMs: number,
  ): Promise<PortalResponse>;

  /** 放开这个网络。设备重启后热点消失，不放开的话请求会一直挂着。 */
  leave(): void;

  isJoined(): boolean;
}

// 用可空的 get：getEnforcing 在导入时就抛，没有原生模块的环境里拦不住。
export default TurboModuleRegistry.get<Spec>('WifiSetup');
