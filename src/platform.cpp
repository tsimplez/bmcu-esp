#include "platform.h"

#if defined(ARDUINO_ARCH_RP2040)
#include <hardware/flash.h>
#endif

// ============================================================
// 非易失存储实现
// ============================================================

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
static const char *region_ns(int region) {
    return region == HAL_REGION_SETTINGS ? "settings" : "filament";
}

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

void hal_storage_read(int region, void *buf, uint32_t len) {
    if (!buf || !len) return;
    uint32_t off = (region == HAL_REGION_SETTINGS) ? FLASH_OFFSET_SETTINGS : FLASH_OFFSET_FILAMENT;
    memcpy(buf, (const void *)(XIP_BASE + off), len);
}

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
