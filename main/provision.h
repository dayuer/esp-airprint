#pragma once
#include <stdbool.h>
#include <stddef.h>

/* 从 NVS 读取已保存的 Wi-Fi 凭据，无则返回 false */
bool prov_load(char *ssid, size_t ssid_len, char *pass, size_t pass_len);

/* 起配网门户（开放热点 + DNS 劫持 + 网页），阻塞直到用户配好并验证通过。
 * 返回后凭据已写入 NVS。 */
void prov_portal_run(void);

/* 清空已保存的凭据（开机时按住 MENU 键触发） */
void prov_erase(void);
