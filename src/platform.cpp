#include "platform.h"
#include "project_presets.h"

/**
 * 获取项目预设使用的芯片标识
 * @return 与项目表 target 字段对应的标识
 * 板型差异留在平台层，网页只消费兼容项目列表
 */
const char *hal_project_target() {
#if defined(CONFIG_IDF_TARGET_ESP32C3)
    return "esp32c3";
#elif defined(ARDUINO_ARCH_ESP32)
    return "esp32s3";
#else
    return "pico";
#endif
}

/**
 * 获取平台默认项目接线
 * @return 生命周期覆盖整个固件运行期的默认预设
 * C3 复用项目表；原生 USB 板保留 UART 适配，其他平台沿用现有默认值
 */
const ProjectPreset &hal_default_project_preset() {
#if defined(CONFIG_IDF_TARGET_ESP32C3)
#if C3_BOARD_USB_TTL && !ARDUINO_USB_CDC_ON_BOOT
    return PROJECT_PRESETS[0];
#else
    static ProjectPreset native_usb = PROJECT_PRESETS[0];
    native_usb.bus.rx = 20;
    native_usb.bus.tx = 21;
    return native_usb;
#endif
#else
    static ProjectPreset preset = {"", "平台默认", hal_project_target(), DEFAULT_AMS_COUNT,
        {PIN_BUS_RX, PIN_BUS_TX, PIN_BUS_DE, DEFAULT_RS485_MODE, PIN_NONE, PIN_NONE},
        IO_CONFIG_MAX_CHANNELS, {}};
    static const int motors[IO_CONFIG_MAX_CHANNELS][2] = {
        MOTOR_PINS_1_4, MOTOR_PINS_5_8, MOTOR_PINS_9_12, MOTOR_PINS_13_16};
    static const int switches[IO_CONFIG_MAX_CHANNELS] = {
        MSW_PINS_1_4, MSW_PINS_5_8, MSW_PINS_9_12, MSW_PINS_13_16};
    for (int ch = 0; ch < IO_CONFIG_MAX_CHANNELS; ++ch) {
        preset.pins[ch][0] = motors[ch][0];
        preset.pins[ch][1] = motors[ch][1];
        preset.pins[ch][2] = switches[ch];
    }
    return preset;
#endif
}

#if defined(ARDUINO_ARCH_RP2040)
#include <hardware/flash.h>
#endif

// ============================================================
// 非易失存储实现
// ============================================================

/**
 * 读取芯片唯一 ID
 * @return 64 位设备唯一标识；平台不支持时返回 0
 * ESP32 读取 eFuse MAC，RP2040 读取 Flash 唯一 ID，供序列号生成使用
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
// 上层原有的 magic / checksum 校验逻辑无需改动
/**
 * 把存储区域映射为 NVS 命名空间
 * @param region HAL_REGION_SETTINGS 或 HAL_REGION_FILAMENT
 * @return 对应的 NVS 命名空间名称
 * ESP32 平台用于区分设置记录和耗材记录两个存储区域
 */
static const char *region_ns(int region) {
    return region == HAL_REGION_SETTINGS ? "settings" : "filament";
}

/**
 * 从 NVS 读取二进制记录
 * @param region 存储区域
 * @param buf 输出缓冲区
 * @param len 期望读取的字节数
 * ESP32 只读取长度完全匹配的 blob；缺失、长度错误或读取不完整时返回全 0xFF
 */
void hal_storage_read(int region, void *buf, uint32_t len) {
    if (!buf || !len) return;
    memset(buf, 0xFF, len);
    Preferences nvs;
    if (nvs.begin(region_ns(region), true)) {
        // 不拼接或截断记录；上层继续校验当前格式标识与完整 CRC32
        const size_t stored_len = nvs.getBytesLength("blob");
        if (stored_len == len && nvs.getBytes("blob", buf, len) != len)
            memset(buf, 0xFF, len);
        nvs.end();
    }
}

/**
 * 写入二进制记录到 NVS
 * @param region 存储区域
 * @param buf 待写入数据
 * @param len 数据长度
 * @return 是否完整写入
 * ESP32 实现：把整块数据写入 NVS 的 blob 键
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
 * RP2040 实现：从 XIP 映射的固定 Flash 偏移直接复制设置或耗材记录
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
 * RP2040 实现：先擦除整个扇区，再按 256 字节对齐写入，最后回读校验
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
