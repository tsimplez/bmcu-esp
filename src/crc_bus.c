#include "crc_bus.h"

// 共用表格
// CRC8 查表：多项式 0x39，共 256 项，由 bus_crc_init() 生成
static uint8_t  g_crc8_tbl[256]  __attribute__((aligned(4)));
// CRC16 查表：多项式 0x1021，共 256 项，由 bus_crc_init() 生成
static uint16_t g_crc16_tbl[256] __attribute__((aligned(4)));
// 查表初始化标志：0=尚未生成，1=已生成，防止重复计算
static uint8_t  g_inited = 0;

/**
 * 初始化 BambuBus 的 CRC 查表
 * 按 CRC8 多项式 0x39 和 CRC16 多项式 0x1021 生成 256 项查表，仅在首次调用时执行。
 */
void bus_crc_init(void)
{
    if (g_inited) return;
    g_inited = 1;

    for (uint32_t d = 0; d < 256; d++) {
        uint8_t r = (uint8_t)d;
        for (uint8_t b = 0; b < 8; b++) {
            if (r & 0x80u) r = (uint8_t)((uint8_t)(r << 1) ^ 0x39u);
            else          r = (uint8_t)(r << 1);
        }
        g_crc8_tbl[d] = r;
    }

    for (uint32_t d = 0; d < 256; d++) {
        uint16_t r = (uint16_t)(d << 8);
        for (uint8_t b = 0; b < 8; b++) {
            if (r & 0x8000u) r = (uint16_t)((uint16_t)(r << 1) ^ 0x1021u);
            else             r = (uint16_t)(r << 1);
        }
        g_crc16_tbl[d] = r;
    }
}

/**
 * 计算 CRC8 校验值
 * @param data 待校验的数据指针
 * @param len 数据长度
 * @return 计算得到的 8 位 CRC 值
 * 使用初值 0x66 和查表法计算 BambuBus 报文的 CRC8。
 */
uint8_t bus_crc8(const uint8_t* data, uint32_t len)
{
    uint8_t r = 0x66u;
    for (uint32_t i = 0; i < len; i++) {
        r = g_crc8_tbl[(uint8_t)(r ^ data[i])];
    }
    return r;
}

/**
 * 计算 CRC16 校验值
 * @param data 待校验的数据指针
 * @param len 数据长度
 * @return 计算得到的 16 位 CRC 值
 * 使用初值 0x913D 和查表法计算 BambuBus 报文的 CRC16。
 */
uint16_t bus_crc16(const uint8_t* data, uint32_t len)
{
    uint16_t r = 0x913Du;
    for (uint32_t i = 0; i < len; i++) {
        const uint8_t idx = (uint8_t)((r >> 8) ^ data[i]);
        r = (uint16_t)(g_crc16_tbl[idx] ^ (uint16_t)(r << 8));
    }
    return r;
}
