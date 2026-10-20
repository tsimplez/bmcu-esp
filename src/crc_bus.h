// 同步于 PJarczak 2026/3/9 07:05 V10.2
#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

uint8_t  bus_crc8(const uint8_t* data, uint32_t len);
uint16_t bus_crc16(const uint8_t* data, uint32_t len);

#ifdef __cplusplus
}
#endif
