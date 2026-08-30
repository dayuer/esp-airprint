#pragma once
#include <stdint.h>
/* 起云端长连接（MQTT 信令 + HTTPS 取文档） */
void cloud_client_start(const uint8_t mac[6]);
const char *cloud_device_id(void);

/* 开机时把上次下发的怪癖档案从 NVS 读回来。
 * 设备可能在没有网络的情况下先插上打印机开印，所以不能等 MQTT 连上再说。 */
void cloud_profile_restore(void);
