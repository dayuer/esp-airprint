/**
 * 服务端地址。
 *
 * 服务端还在写，开发期指向本地 mock（npm run mock）。
 * iOS 模拟器与本机同网络栈，直接 127.0.0.1 就行；Android 模拟器要用 10.0.2.2；
 * Android 真机走 `adb reverse tcp:9443 tcp:9443` 之后也能用 127.0.0.1。
 */
const DEV_API = 'http://127.0.0.1:9443/api';
const PROD_API = 'https://mqtt.silkline.id:9443/api';

/**
 * 开发期默认指本地 mock。想对着真服务端跑，把下面这行改成 PROD_API。
 *
 * 注意真服务端目前**还没实现 GET /api/devices**（文档 4.5b）——Go 侧的实现
 * 已经写好并测过（server/go/internal/httpapi/devices_endpoint.go），但那台
 * 机器上跑的还是旧二进制。没部署之前指过去，登录后会看到一个空列表。
 */
/**
 * 指真服务端。
 *
 * 固件连的是 mqtt.silkline.id 的 MQTT broker，所以**设备密钥必须由那台
 * 服务器签发**。对着本地 mock enroll 出来的密钥，broker 不认识，设备一连
 * 就是 CONNACK 0x05「密钥被拒」——mock 上做的一切对板子来说都是空转。
 *
 * 已知代价：那台服务器还没部署 GET /api/devices（文档 4.5b，Go 侧实现和
 * 测试都在仓库里了）。所以设备列表暂时空着，配网和打印不受影响。
 */
export const API_BASE_URL = PROD_API;

/**
 * 设备配网门户的地址。
 *
 * 真实设备的热点固定是 192.168.4.1。开发期可以指向 `npm run portal` 起的假门户
 * ——手机连在普通 Wi-Fi 上就能把整个向导走一遍，不需要真板子。
 *
 * **它不是绕过。** 假门户按 main/provision.c 的行为写，包括「验证通过后 3 秒
 * 重启、热点消失」那一段。走通它不等于走通真设备，但走不通它一定也走不通真设备。
 */
// 真实设备固定在 192.168.4.1。开发期想不用板子走一遍向导的话，
// 把这里换成 `npm run portal` 起的假门户地址（http://<开发机 IP>:8888）——
// 那份假门户按 main/provision.c 的行为写，包括「验证通过后 3 秒重启、
// 热点消失」那一段。走通它不等于走通真设备，但走不通它一定也走不通真设备。
export const PORTAL_BASE_URL = 'http://192.168.4.1';
