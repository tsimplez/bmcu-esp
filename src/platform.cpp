#include "platform.h"

#if defined(ARDUINO_ARCH_RP2040)
#include <hardware/flash.h>
#endif

// ============================================================
// 非易失存储实现
// ============================================================

/**
 * 读取芯片唯一 ID
 * @return 64 位设备唯一标识；平台不支持时返回 0
 * ESP32 读取 eFuse MAC，RP2040 读取 Flash 唯一 ID，供序列号生成使用。
 */
uint64_t hal_get_device_id() {
#if defined(ARDUINO_ARCH_ESP32)
    return ESP.getEfuseMac();
#elif defined(ARDUINO_ARCH_RP2040)
    uint8_t id[8] = {};
    flash_get_unique_id(id);
    uint64_t value = 0;
    for (size_t i = 0; i < sizeof(id); i++) value = (value << 8) | id[i];
    return value;
#else
    return 0;
#endif
}

#if defined(ARDUINO_ARCH_ESP32)

#include <Preferences.h>

// NVS 键值存储；未写入过时返回全 0xFF，与空片 Flash 语义一致，
// 上层原有的 magic / checksum 校验逻辑无需改动。
/**
 * 把存储区域映射为 NVS 命名空间
 * @param region HAL_REGION_SETTINGS 或 HAL_REGION_FILAMENT
 * @return 对应的 NVS 命名空间名称
 * ESP32 平台用于区分设置记录和耗材记录两个存储区域。
 */
static const char *region_ns(int region) {
    return region == HAL_REGION_SETTINGS ? "settings" : "filament";
}

/**
 * 从 NVS 读取二进制记录
 * @param region 存储区域
 * @param buf 输出缓冲区
 * @param len 期望读取的字节数
 * ESP32 实现：读取 blob；未写入过或长度不足的部分保持 0xFF，兼容旧记录。
 */
void hal_storage_read(int region, void *buf, uint32_t len) {
    if (!buf || !len) return;
    memset(buf, 0xFF, len);
    Preferences nvs;
    if (nvs.begin(region_ns(region), true)) {
        // 短于结构体的旧 blob 也能读进来，尾部保持 0xFF；
        // 上层只用魔数 + CRC32 判断记录是否有效，对不上就恢复默认值。
        const size_t stored_len = nvs.getBytesLength("blob");
        if (stored_len) nvs.getBytes("blob", buf, stored_len < len ? stored_len : len);
        nvs.end();
    }
}

/**
 * 写入二进制记录到 NVS
 * @param region 存储区域
 * @param buf 待写入数据
 * @param len 数据长度
 * @return 是否完整写入
 * ESP32 实现：把整块数据写入 NVS 的 blob 键。
 */
bool hal_storage_write(int region, const void *buf, uint32_t len) {
    if (!buf || !len) return false;
    Preferences nvs;
    if (!nvs.begin(region_ns(region), false)) return false;
    const size_t written = nvs.putBytes("blob", buf, len);
    nvs.end();
    return written == len;
}

#else

// RP2040 直接操作 Flash：程序占用前 1MB，数据放在其后
#define FLASH_OFFSET_SETTINGS (1024 * 1024)
#define FLASH_OFFSET_FILAMENT (1536 * 1024)

/**
 * 从 RP2040 Flash 读取数据
 * @param region 存储区域
 * @param buf 输出缓冲区
 * @param len 读取长度
 * RP2040 实现：从 XIP 映射的固定 Flash 偏移直接复制设置或耗材记录。
 */
void hal_storage_read(int region, void *buf, uint32_t len) {
    if (!buf || !len) return;
    uint32_t off = (region == HAL_REGION_SETTINGS) ? FLASH_OFFSET_SETTINGS : FLASH_OFFSET_FILAMENT;
    memcpy(buf, (const void *)(XIP_BASE + off), len);
}

/**
 * 写入数据到 RP2040 Flash
 * @param region 存储区域
 * @param buf 待写入数据
 * @param len 数据长度
 * @return 写入后回读是否一致
 * RP2040 实现：先擦除整个扇区，再按 256 字节对齐写入，最后回读校验。
 */
bool hal_storage_write(int region, const void *buf, uint32_t len) {
    // 准备缓冲区（必须是Flash页大小的倍数，256字节对齐）
    static uint8_t page[FLASH_SECTOR_SIZE] __attribute__((aligned(256)));
    if (!buf || !len || len > sizeof(page)) return false;

    uint32_t off = (region == HAL_REGION_SETTINGS) ? FLASH_OFFSET_SETTINGS : FLASH_OFFSET_FILAMENT;
    uint32_t count = (len + 255u) & ~255u;  // flash_range_program 要求 256 字节对齐

    // 复制设置数据到缓冲区开头
    memcpy(page, buf, len);

    // 将写入范围的剩余字节填充为 0xFF
    memset(page + len, 0xFF, count - len);

    // 禁用中断并保存状态
    uint32_t primask = __get_PRIMASK();
    __disable_irq();

    // 擦除扇区
    flash_range_erase(off, FLASH_SECTOR_SIZE);

    // 写入数据
    flash_range_program(off, page, count);

    // 恢复中断状态
    __set_PRIMASK(primask);

    return memcmp((const void *)(XIP_BASE + off), page, count) == 0;
}

#endif
