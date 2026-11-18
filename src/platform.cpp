#include "platform.h"
#include "project_presets.h"

#if defined(ARDUINO_ARCH_RP2040) && defined(ARDUINO_ARCH_MBED)
#include <new>
#endif

#if defined(ARDUINO_ARCH_ESP32)
#include <esp_system.h>
#if defined(CONFIG_IDF_TARGET_ESP32C3) && !ARDUINO_USB_CDC_ON_BOOT
#include <esp_log.h>
#include <esp32-hal-uart.h>
#include <freertos/semphr.h>
#include "PCF8575_PinMap.h"
#include "log_bus.h"
#if defined(BMCU_USB_CDC_AUTO)
#include <driver/usb_serial_jtag.h>
#endif
#endif
#endif

namespace {
#if defined(ARDUINO_ARCH_ESP32)
bool console_enabled = false;
#if defined(CONFIG_IDF_TARGET_ESP32C3) && !ARDUINO_USB_CDC_ON_BOOT
StaticSemaphore_t console_mutex_storage;
SemaphoreHandle_t console_mutex = nullptr;
vprintf_like_t console_sdk_output = nullptr;
bool console_muted = false, console_reported = false;
#if defined(BMCU_USB_CDC_AUTO)
bool console_usb_enabled = false, console_usb_reported = false;
#endif

/**
 * 丢弃 UART0 被接线占用时的 SDK 控制台输出
 * @param format 日志格式
 * @param args 格式参数
 * @return 零表示未输出任何字符
 */
int console_discard(const char *format, va_list args) {
    (void)format; (void)args;
    return 0;
}

/**
 * 判断一根引脚是否属于指定控制台引脚对
 * @param pin 已保存的 GPIO 编号
 * @param first 控制台的第一根引脚
 * @param second 控制台的第二根引脚
 * @return 与任一控制台引脚相同时返回 true
 */
bool console_pin(const int pin, const int first, const int second) { return pin == first || pin == second; }

/**
 * 判断当前启用接线是否占用指定控制台引脚对
 * @param first 控制台的第一根引脚
 * @param second 控制台的第二根引脚
 * @return 任一启用的通道、总微动、I2C 或 BambuBus 使用任一控制台引脚时返回 true
 * 未启用通道和未启用总线的 DE 不占用引脚
 */
bool console_pins_used(const int first, const int second) {
    if (console_pin(IO_CONFIG.switch_pin, first, second)) return true;
    if (PCF_SDA != PIN_NONE && PCF_SCL != PIN_NONE && PCF_SDA != PCF_SCL &&
        (console_pin(PCF_SDA, first, second) || console_pin(PCF_SCL, first, second))) return true;
    if (io_config_bus_enabled() && (console_pin(BUS_RX, first, second) || console_pin(BUS_TX, first, second) ||
        (RS485_MODE == RS485_MODE_DE && console_pin(BUS_DE, first, second)))) return true;
    for (int ch = 0; ch < active_channel_count(); ++ch) {
        if (console_pin(IO_CONFIG.motor[ch][0], first, second) || console_pin(IO_CONFIG.motor[ch][1], first, second) ||
            console_pin(IO_CONFIG.microswitch[ch], first, second) || console_pin(IO_CONFIG.hall[ch], first, second)) return true;
    }
    return false;
}

/**
 * 停用 UART0 并释放控制台引脚
 * 调用方持有控制台互斥锁；暂停 Arduino 和 SDK 输出，不影响网页日志
 */
void console_uart_stop() {
    if (!console_muted) {
        console_sdk_output = esp_log_set_vprintf(console_discard);
        console_muted = true;
    }
    uartSetDebug(nullptr);
    Serial.end();
    console_enabled = false;
    pinMode_PCF8575(20, INPUT);
    pinMode_PCF8575(21, INPUT);
}

#if defined(BMCU_USB_CDC_AUTO)
/**
 * 停用原生 USB 日志并把 GPIO18/19 交回普通接线
 * 调用方持有控制台互斥锁；驱动卸载和引脚释放均由启动或主循环执行
 */
void console_usb_stop() {
    if (console_usb_enabled) usb_serial_jtag_driver_uninstall();
    console_usb_enabled = false;
    hal_prepare_gpio(); // 驱动卸载保留 USB 焊盘状态，须显式关闭后再设置 GPIO
    pinMode_PCF8575(18, INPUT);
    pinMode_PCF8575(19, INPUT);
}
#endif
#endif
#endif
} // namespace

/**
 * 在读取接线配置前准备控制台
 * C3 UART0 先暂停应用、Arduino 和 SDK 输出，合宙同时停用原生 USB 并释放引脚；其他平台沿用原控制台初始化
 * 仅在启动或主循环确认控制台可交接时调用，不影响网页日志，也不改变 ROM/Bootloader 启动输出
 */
void hal_console_prepare() {
#if defined(CONFIG_IDF_TARGET_ESP32C3) && !ARDUINO_USB_CDC_ON_BOOT
    if (!console_mutex) console_mutex = xSemaphoreCreateMutexStatic(&console_mutex_storage);
    xSemaphoreTake(console_mutex, portMAX_DELAY);
    console_uart_stop();
#if defined(BMCU_USB_CDC_AUTO)
    console_usb_stop();
#endif
    xSemaphoreGive(console_mutex);
#else
    hal_console_begin();
#endif
}

/**
 * 根据已加载接线启用或关闭控制台
 * C3 UART0 按 GPIO20/21 占用启停，合宙原生 USB 按 GPIO18/19 占用独立启停，切换与网络任务的日志写入互斥
 * 主循环须在释放旧通道后、绑定新通道前调用；其他环境的固定原生 USB、S3 和 RP2040 保持原输出方式
 */
void hal_console_begin() {
#if defined(CONFIG_IDF_TARGET_ESP32C3) && !ARDUINO_USB_CDC_ON_BOOT
    if (!console_mutex) hal_console_prepare();
    const bool uart_used = console_pins_used(20, 21);
    const bool uart_changed = !console_reported || console_enabled == uart_used;
    xSemaphoreTake(console_mutex, portMAX_DELAY);
    if (uart_used && console_enabled) console_uart_stop();
#if defined(BMCU_USB_CDC_AUTO)
    const bool usb_used = console_pins_used(18, 19);
    const bool usb_changed = !console_usb_reported || console_usb_enabled == usb_used;
    if (usb_used && console_usb_enabled) console_usb_stop();
    if (!usb_used && !console_usb_enabled) {
        pinMode_PCF8575(18, INPUT);
        pinMode_PCF8575(19, INPUT);
        usb_serial_jtag_driver_config_t config = {.tx_buffer_size = 1024, .rx_buffer_size = 128};
        console_usb_enabled = usb_serial_jtag_driver_install(&config) == ESP_OK;
        if (!console_usb_enabled) console_usb_stop();
    }
    console_usb_reported = true;
#endif
    if (!uart_used && !console_enabled) {
        Serial.begin(115200);
        console_enabled = static_cast<bool>(Serial);
        if (console_enabled) {
            Serial.setDebugOutput(true);
            esp_log_set_vprintf(console_sdk_output);
            console_muted = false;
        }
    }
    xSemaphoreGive(console_mutex);
    console_reported = true;
    if (uart_changed) {
        if (uart_used) LOG_MAIN("[system] GPIO20/21 已分配，UART0 控制台已关闭，日志请在网页查看");
        else LOG_MAIN("[system] UART0 控制台%s", console_enabled ? "已启用，GPIO20/21 未被占用" : "初始化失败，日志请在网页查看");
    }
#if defined(BMCU_USB_CDC_AUTO)
    if (usb_changed) {
        if (usb_used) LOG_MAIN("[system] GPIO18/19 已分配，原生 USB 日志已关闭，日志请在网页查看");
        else LOG_MAIN("[system] 原生 USB 日志%s", console_usb_enabled ? "已启用，GPIO18/19 未被占用" : "初始化失败，日志请在网页查看");
    }
#endif
#elif defined(ARDUINO_ARCH_ESP32)
    if (!console_enabled) {
        Serial.begin(115200);
        console_enabled = true;
    }
#endif
}

/**
 * 向当前可用的控制台写入日志
 * @param data 日志字节
 * @param len 字节数
 * C3 跳过被接线占用的控制台；合宙 USB 未连接或缓冲已满时丢弃回显，不等待主机，网页日志不受影响
 */
void hal_console_write(const char *data, size_t len) {
#if defined(CONFIG_IDF_TARGET_ESP32C3) && !ARDUINO_USB_CDC_ON_BOOT
    if (!console_mutex) return;
    xSemaphoreTake(console_mutex, portMAX_DELAY);
    if (console_enabled) Serial.write(reinterpret_cast<const uint8_t *>(data), len);
#if defined(BMCU_USB_CDC_AUTO)
    if (console_usb_enabled && usb_serial_jtag_is_connected()) usb_serial_jtag_write_bytes(data, len, 0);
#endif
    xSemaphoreGive(console_mutex);
#elif defined(ARDUINO_ARCH_ESP32)
    if (console_enabled) Serial.write(reinterpret_cast<const uint8_t *>(data), len);
#else
    fwrite(data, 1, len, stdout);
#endif
}

/**
 * 按配置引脚初始化共享 I2C 总线
 * @param sda 已通过接线校验的 SDA 引脚
 * @param scl 已通过接线校验的 SCL 引脚
 * @return true 表示初始化完成并设置为 400kHz
 * Mbed 只能在构造时指定引脚；先释放旧驱动，再原址重建 Wire，保持 PCF8575 等持有的指针有效
 * 仅由主循环在没有 I2C 传输时调用
 */
bool hal_i2c_begin(const int sda, const int scl) {
#if defined(ARDUINO_ARCH_ESP32)
    if (!Wire.begin(sda, scl)) return false;
#elif defined(ARDUINO_ARCH_RP2040) && defined(ARDUINO_ARCH_MBED)
    Wire.end();
    Wire.~MbedI2C();
    new (&Wire) MbedI2C(sda, scl);
    Wire.begin();
#elif defined(ARDUINO_ARCH_RP2040)
    if (!Wire.setSDA(sda) || !Wire.setSCL(scl)) return false;
    Wire.begin();
#else
    return false;
#endif
    Wire.setClock(400000);
    return true;
}

/**
 * 判断主控引脚是否支持霍尔模拟采样
 * @param pin 主控 GPIO 编号
 * @return true 表示具有当前方案支持的 ADC 输入
 * ESP32 仅使用 ADC1，C3 排除 ADC2 的 GPIO5；保留脚和其他功能占用由接线层继续检查
 */
bool hal_adc_pin_available(const int pin) {
#if defined(CONFIG_IDF_TARGET_ESP32C3)
    return pin >= 0 && pin <= 4;
#elif defined(CONFIG_IDF_TARGET_ESP32S3)
    return pin >= 1 && pin <= 10;
#elif defined(ARDUINO_ARCH_RP2040)
    return pin >= 26 && pin <= 29;
#else
    return false;
#endif
}

/**
 * 初始化一个霍尔 ADC 输入
 * @param pin 已通过整组接线校验的 GPIO 编号
 * @return true 表示引脚支持 ADC 并已设置采样参数
 * 在主循环应用接线时设置十二位分辨率和最高衰减默认值，供首次读取时初始化 ADC
 * Arduino 3 的单引脚衰减接口要求通道已初始化，因此这里统一设置默认值且不额外触发转换
 */
bool hal_adc_prepare(const int pin) {
    if (!hal_adc_pin_available(pin)) return false;
    analogReadResolution(12);
#if defined(ARDUINO_ARCH_ESP32)
    analogSetAttenuation(ADC_11db);
#endif
    return true;
}

/**
 * 读取一次霍尔 ADC 原始值
 * @param pin 已初始化的主控 ADC 引脚
 * @param raw 输出十二位原始值，范围零到四千零九十五
 * @return true 表示引脚和读数有效，无法判断模拟传感器是否实际接入
 */
bool hal_adc_read(const int pin, uint16_t &raw) {
    if (!hal_adc_pin_available(pin)) return false;
    const int value = analogRead(pin);
    if (value < 0 || value > 4095) return false;
    raw = static_cast<uint16_t>(value);
    return true;
}

/**
 * 生成本次启动的霍尔历史标识
 * @return 非零标识，用于使网页丢弃重启前的增量游标
 * ESP32 使用硬件随机源；没有网页的 RP2040 使用芯片标识和启动时刻混合
 */
uint32_t hal_hall_epoch() {
#if defined(ARDUINO_ARCH_ESP32)
    const uint32_t epoch = esp_random();
#else
    const uint64_t id = hal_get_device_id();
    const uint32_t epoch = static_cast<uint32_t>(id) ^ static_cast<uint32_t>(id >> 32) ^ micros();
#endif
    return epoch ? epoch : 1;
}

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
 * C3 直接复用项目表接线，其他平台由默认电机和微动数组生成接线及通道数
 */
const ProjectPreset &hal_default_project_preset() {
#if defined(CONFIG_IDF_TARGET_ESP32C3)
    return PROJECT_PRESETS[0];
#else
    static constexpr int motors[IO_CONFIG_MAX_CHANNELS][2] = {
        MOTOR_PINS_1_4, MOTOR_PINS_5_8, MOTOR_PINS_9_12, MOTOR_PINS_13_16};
    static constexpr int switches[IO_CONFIG_MAX_CHANNELS] = {
        MSW_PINS_1_4, MSW_PINS_5_8, MSW_PINS_9_12, MSW_PINS_13_16};
    static const ProjectPreset preset = {"", "平台默认", hal_project_target(), DEFAULT_AMS_COUNT,
        {PIN_BUS_RX, PIN_BUS_TX, PIN_BUS_DE, DEFAULT_RS485_MODE, PIN_NONE, PIN_NONE},
        {motors, switches}};
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
