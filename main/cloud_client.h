#pragma once
#include <stdint.h>
/* 起云端长连接（MQTT 信令 + HTTPS 取文档） */
void cloud_client_start(const uint8_t mac[6]);
const char *cloud_device_id(void);
