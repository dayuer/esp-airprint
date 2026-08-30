#pragma once
#include <stdbool.h>
#include <stddef.h>

/* 从 NVS 读取已保存的 Wi-Fi 凭据，无则返回 false */
bool prov_load(char *ssid, size_t ssid_len, char *pass, size_t pass_len);

/* 起配网门户（开放热点 + DNS 劫持 + 网页），阻塞直到用户配好并验证通过。
 * 返回后凭据已写入 NVS。 */
void prov_portal_run(void);

/* 读设备密钥（API 文档 1.3：NVS namespace "cloud" / key "devkey"）。
 * 格式是 {key_id}.{secret}，45 字符。**按不透明字符串处理，不解析不截断。**
 * 没配过返回 false —— 此时固件不要尝试连云端，否则会陷入
 * 「认证被拒 → 重连」的死循环。 */
bool prov_load_devkey(char *out, size_t cap);

/* 清空已保存的凭据（开机时按住 MENU 键触发）。Wi-Fi 和设备密钥一起清。 */
void prov_erase(void);
