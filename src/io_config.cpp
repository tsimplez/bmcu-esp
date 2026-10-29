#include "io_config.h"
#include "platform.h"
#include "settings_storage.h"
#include "PCF8575_PinMap.h"
#include "project_presets.h"

// 是否是预留引脚
/**
 * 判断引脚是否为系统保留脚
 * @param pin 引脚号
 * @return true 表示不可分配
 * 按平台过滤微动专用脚、Flash、USB、控制台串口和启动绑带等保留引脚
 */
static bool is_reserved_pin(int pin) {
#if defined(PIN_SWITCH)
    if (pin != PIN_NONE && pin == PIN_SWITCH) return true;
#endif

#if defined(CONFIG_IDF_TARGET_ESP32C3)
    // Flash 和控制台不可重分配；经典款的 USB 焊盘可用作普通 GPIO
    if (pin == 11 || (pin >= 14 && pin <= 17)) return true;
#if C3_BOARD_USB_TTL && !ARDUINO_USB_CDC_ON_BOOT
    if (pin == 20 || pin == 21) return true;
#else
    if (pin == 18 || pin == 19) return true;
#if !ARDUINO_USB_CDC_ON_BOOT
    if (pin == 20 || pin == 21) return true;
#endif
#endif
#elif defined(CONFIG_IDF_TARGET_ESP32S3)
    // S3 DevKit 的 26-37 接 Flash/PSRAM，19/20 保留给原生 USB
    if ((pin >= 26 && pin <= 37) || pin == 19 || pin == 20 || pin == 43 || pin == 44) return true;
#endif
    return false;
}

// 是否是真正的io引脚
/**
 * 判断引脚是否为可用真实 GPIO
 * @param pin 引脚号
 * @return true 表示真实且未保留
 * 结合平台引脚范围与保留脚列表，过滤不存在的编号
 */
static bool is_real_gpio(int pin) {
    if (pin < 0 || pin >= 255 || is_reserved_pin(pin)) return false;

#if defined(CONFIG_IDF_TARGET_ESP32C3)
    // C3 的可选范围只保留实际引出的 GPIO；保留 2/8 是为了兼容现有默认映射，
    // 由网页另外显示启动风险提示
    return pin <= 21;
#elif defined(ARDUINO_ARCH_ESP32)
    return digitalPinIsValid(pin);
#elif defined(NUM_DIGITAL_PINS)
    return pin < NUM_DIGITAL_PINS;
#else
    // RP2040 Arduino core 没有统一导出 NUM_DIGITAL_PINS 时，板上 GPIO 为 0-29
    return pin < 30;
#endif
}

// io配置设置默认值
/**
 * 生成平台默认引脚配置
 * @param config 待填充的 IO 配置
 * 由平台选择默认项目，C3 和网页复用同一份预设接线
 */
void io_config_set_defaults(IoConfig &config) {
    project_preset_io(hal_default_project_preset(), config);
}

// 该引脚在 I2C 总线状态变化后是否还能用：配置后 SDA/SCL 不能再用作通道引脚，
// 未配置时扩展虚拟引脚不再存在
/**
 * 判断引脚在 I2C 总线状态切换后是否失效
 * @param pin 引脚号
 * @param i2c_configured 切换后的 I2C 总线是否启用
 * @param sda I2C SDA 引脚
 * @param scl I2C SCL 引脚
 * @return true 表示该引脚不再可用
 * 启用 I2C 时 SDA/SCL 不能再作通道脚；关闭时扩展虚拟引脚全部失效
 */
static bool pin_invalid_after_switch(int pin, bool i2c_configured, int sda, int scl) {
    if (pin == PIN_NONE) return false;
    if (!i2c_configured) return IS_PCF8575_VIRTUAL_PIN(pin);
    return pin == sda || pin == scl;
}

// 切换 I2C 总线配置时只清掉不再合法的引脚，其余接线原样保留，避免覆盖用户已接线
// 电机是一对，任一脚被占用就整对清掉；清成 PIN_NONE(无) 表示该通道待配置
// 返回清掉的引脚数量，供日志使用
/**
 * 清理切换后失效的通道引脚
 * @param config 待清理的 IO 配置
 * @param i2c_configured I2C 总线是否启用
 * @param sda I2C SDA 引脚
 * @param scl I2C SCL 引脚
 * @return 被清空的引脚数量
 * 电机任一脚失效就整对清成 PIN_NONE，微动脚单独清理，其余接线保持不变
 */
int io_config_clear_invalid_pins(IoConfig &config, bool i2c_configured, int sda, int scl) {
    int cleared = 0;
    for (int i = 0; i < IO_CONFIG_MAX_CHANNELS; i++) {
        if (pin_invalid_after_switch(config.motor[i][0], i2c_configured, sda, scl) ||
            pin_invalid_after_switch(config.motor[i][1], i2c_configured, sda, scl)) {
            if (config.motor[i][0] != PIN_NONE) cleared++;
            if (config.motor[i][1] != PIN_NONE) cleared++;
            config.motor[i][0] = PIN_NONE;
            config.motor[i][1] = PIN_NONE;
        }
        if (pin_invalid_after_switch(config.microswitch[i], i2c_configured, sda, scl)) {
            config.microswitch[i] = PIN_NONE;
            cleared++;
        }
    }
    return cleared;
}

/**
 * 判断引脚是否被 BambuBus 占用
 * @param pin 引脚号
 * @param bus 可选候选接线，空指针使用当前配置
 * @return true 表示已被占用
 * RX/TX 始终占用；只有 DE 模式才占用 DE 引脚
 */
static bool bus_uses_pin(int pin, const IoBusConfig *bus = nullptr) {
    if (pin == PIN_NONE) return false;
    if (bus) return pin == bus->rx || pin == bus->tx || (bus->mode == RS485_MODE_DE && pin == bus->de);
    return pin == BUS_RX || pin == BUS_TX || (RS485_MODE == RS485_MODE_DE && pin == BUS_DE);
}

// io配置是否引脚可用
/**
 * 判断引脚能否出现在网页选择列表中
 * @param pin 引脚号
 * @param allow_none 是否允许 PIN_NONE
 * @param bus 可选候选总线接线，空指针使用当前配置
 * @return true 表示可用
 * 综合检查虚拟引脚开关、I2C 占用、系统保留脚和总线占用
 */
bool io_config_pin_available(int pin, bool allow_none, const IoBusConfig *bus) {
    if (allow_none && pin == PIN_NONE) return true;
    // 虚拟引脚只在 I2C 总线已配置后可选；未配置时 I2C 引脚可作普通 IO
    const int sda = bus ? bus->sda : PCF_SDA, scl = bus ? bus->scl : PCF_SCL;
    const bool configured = sda != PIN_NONE && scl != PIN_NONE && sda >= 0 && scl >= 0 && sda != scl;
    if (IS_PCF8575_VIRTUAL_PIN(pin)) return configured;
    if (configured && (pin == sda || pin == scl)) return false;
    return is_real_gpio(pin) && !bus_uses_pin(pin, bus);
}

/**
 * 判断引脚能否作为 I2C 引脚
 * @param pin 引脚号
 * @param sda true 表示按 SDA 校验，false 表示按 SCL 校验
 * @return true 表示可用
 * RP2040 额外校验 I2C0 的固定 SDA/SCL 复用位置
 */
static bool i2c_pin_available(int pin, bool sda) {
#if defined(ARDUINO_ARCH_RP2040)
    // Wire 使用 I2C0，RP2040 的 SDA/SCL 复用位置固定
    return is_real_gpio(pin) && pin % 4 == (sda ? 0 : 1);
#else
    return is_real_gpio(pin);
#endif
}

/**
 * 校验 I2C 接线配置
 * @param sda SDA 引脚
 * @param scl SCL 引脚
 * @param config 当前通道接线
 * @param active_channels 启用通道数
 * @param error 可选的错误文本输出
 * @param bus 可选候选总线接线，空指针使用当前配置
 * @return true 表示配置合法
 * 检查通道数、与总线/通道引脚的冲突、SDA/SCL 是否不同以及平台复用限制
 */
bool io_config_i2c_validate(int sda, int scl, const IoConfig &config, int active_channels, String *error,
                            const IoBusConfig *bus) {
    if (active_channels < 1 || active_channels > IO_CONFIG_MAX_CHANNELS) {
        if (error) *error = "通道数量无效";
        return false;
    }
    if (bus_uses_pin(sda, bus) || bus_uses_pin(scl, bus)) {
        if (error) *error = "所选 GPIO 已被 BambuBus 占用，请先修改并保存总线接线";
        return false;
    }
    if (!i2c_pin_available(sda, true) || !i2c_pin_available(scl, false) || sda == scl) {
        if (error) *error = "SDA、SCL 必须使用不同的可用主控 GPIO，不能使用系统保留引脚或 PCF 扩展引脚";
        return false;
    }
    for (int ch = 0; ch < active_channels; ch++) {
        for (int role = 0; role < 3; role++) {
            const int pin = role < 2 ? config.motor[ch][role] : config.microswitch[ch];
            if (pin == sda || pin == scl) {
                if (error) *error = io_config_pin_label(pin) + " 已被通道 " + String(ch + 1) +
                    (role == 0 ? " 进料" : role == 1 ? " 退料" : " 微动") + " 使用，请先修改通道配置";
                return false;
            }
        }
    }
    return true;
}

// pin警告文本
/**
 * 获取引脚风险提示文本
 * @param pin 引脚号
 * @return 中文提示；无风险时返回空串
 * 为 ESP32-C3/S3 的启动绑带脚提供网页提示
 */
static const char *pin_warning_text(int pin) {
#if defined(CONFIG_IDF_TARGET_ESP32C3)
    if (pin == 2) return "ESP32-C3 GPIO2 是启动绑带引脚，建议上电或复位时保持高电平，避免外部电路影响启动正常运行时可作为普通 IO";
    if (pin == 9) return "ESP32-C3 GPIO9 连接 BOOT 按键，是启动绑带引脚；上电或复位时拉低会进入下载模式，外部电路需保持高电平才能正常启动";
    if (pin == 8) return "ESP32-C3 GPIO8：进入串口下载模式时必须为高电平；复位时被微动开关等外部电路拉低，会导致无法进入下载模式、无法刷入固件刷机前请释放该引脚正常运行时可作为普通 IO";
#elif defined(CONFIG_IDF_TARGET_ESP32S3)
    if (pin == 0) return "ESP32-S3 GPIO0：上电或复位时拉低会选择下载启动模式；正常运行固件时应避免外部电路在复位时将其拉低";
#else
    (void) pin;
#endif
    return "";
}

// io配置引脚有警告
/**
 * 判断引脚是否需要风险提示
 * @param pin 引脚号
 * @return true 表示存在风险提示
 * 根据 pin_warning_text() 是否为空判断
 */
bool io_config_pin_has_warning(int pin) {
    return pin_warning_text(pin)[0] != '\0';
}

// io配置引脚标签
/**
 * 生成引脚可读名称
 * @param pin 引脚号
 * @return “无”、IOx 或带芯片地址的 PCF8575 Px
 * 网页和日志按地址区分四片扩展器；面板名称与寄存器位序分开转换
 */
String io_config_pin_label(int pin) {
    if (pin == PIN_NONE) return "无";
    if (IS_PCF8575_VIRTUAL_PIN(pin)) {
        char label[24];
        snprintf(label, sizeof(label), "PCF8575 0x%02X P%d",
                 PCF8575_ADDR + VIRTUAL_TO_PCF8575_DEVICE(pin), VIRTUAL_TO_PCF8575_PIN(pin));
        return String(label);
    }
    return String("IO") + String(pin);
}

// io配置验证
/**
 * 校验整组通道接线
 * @param config 通道接线配置
 * @param active_channels 启用通道数
 * @param error 可选的错误文本输出
 * @param bus 可选候选总线接线，空指针使用当前配置
 * @return true 表示配置合法
 * 检查电机和微动引脚是否可用、是否重复占用同一引脚
 */
bool io_config_validate(const IoConfig &config, int active_channels, String *error, const IoBusConfig *bus) {
    if (active_channels < 1 || active_channels > IO_CONFIG_MAX_CHANNELS) {
        if (error) *error = "通道数量无效";
        return false;
    }

    int assigned[IO_CONFIG_MAX_CHANNELS * 3] = {};
    int assigned_count = 0;
    for (int ch = 0; ch < active_channels; ch++) {
        for (int role = 0; role < 3; role++) {
            const int pin = role < 2 ? config.motor[ch][role] : config.microswitch[ch];
            // "无"(PIN_NONE) 合法：切换 I2C 配置会先清空不再可用的引脚，
            // 该通道等用户在网页里补配置
            if (!io_config_pin_available(pin, true, bus)) {
                if (error)
                    *error = String("通道 ") + String(ch + 1) + " 的" +
                             (role == 0 ? "进料" : role == 1 ? "退料" : "微动") +
                             "引脚不可用：" + io_config_pin_label(pin);
                return false;
            }
            if (pin == PIN_NONE) continue;
            for (int i = 0; i < assigned_count; i++) {
                if (assigned[i] == pin) {
                    if (error) *error = String("引脚重复使用：") + io_config_pin_label(pin);
                    return false;
                }
            }
            assigned[assigned_count++] = pin;
        }
    }
    return true;
}

// io配置附加引脚选项
/**
 * 生成网页可选引脚 JSON 数组
 * @param json 待追加的 JSON 字符串
 * @param i2c_only true 时只列真实 GPIO
 * 把引脚编号、显示名称和风险提示拼接为网页下拉框需要的数组
 */
void io_config_append_pin_options(String &json, bool i2c_only) {
    json += "[";
    bool first = true;
    for (int pin = 0; pin < 255; pin++) {
        if (i2c_only ? !is_real_gpio(pin) : !io_config_pin_available(pin)) continue;
        if (!first) json += ',';
        first = false;
        json += "{\"id\":" + String(pin) +
                ",\"label\":\"" + io_config_pin_label(pin) +
                "\",\"warning\":" + String(io_config_pin_has_warning(pin) ? "true" : "false") +
                ",\"warning_text\":\"" + pin_warning_text(pin) + "\"}";
    }
    json += "]";
}


/**
 * 校验 BambuBus 总线接线
 * @param rx 接收引脚
 * @param tx 发送引脚
 * @param de 方向控制引脚
 * @param mode RS485 收发模式
 * @param sda I2C SDA 引脚
 * @param scl I2C SCL 引脚
 * @param config 通道接线配置
 * @param active_channels 启用通道数
 * @param error 可选的错误文本输出
 * @return true 表示配置合法
 * 按传入的候选 I2C 接线检查冲突，同时检查 RX/TX/DE 合法性、重复占用和平台复用限制
 */
bool io_config_bus_validate(int rx, int tx, int de, int mode, int sda, int scl,
                            const IoConfig &config, int active_channels, String *error) {
    if ((mode != RS485_MODE_RXTX && mode != RS485_MODE_DE) ||
        active_channels < 1 || active_channels > IO_CONFIG_MAX_CHANNELS) {
        if (error) *error = "总线模式或通道数量无效";
        return false;
    }
    const int pins[] = {rx, tx, de};
    const char *roles[] = {"RX", "TX", "DE"};
    for (int i = 0; i < (mode == RS485_MODE_DE ? 3 : 2); i++) {
        const int pin = pins[i];
        bool available = is_real_gpio(pin);
#if defined(ARDUINO_ARCH_RP2040)
        // 兼容 Pico 旧板级映射中 PIN_SWITCH 与默认 UART0 TX 共用 GPIO0 的布局
        // 该引脚仍不开放给通道和 I2C；不在本次 C3 配置中改变旧板行为
        if (i == 1 && pin == PIN_BUS_TX && pin == PIN_SWITCH) available = true;
        // 固定 UART0：RX=1/13/17/29，TX=0/12/16/28
        if (i == 0) available = available && (pin == 1 || pin == 13 || pin == 17 || pin == 29);
        if (i == 1) available = available && (pin == 0 || pin == 12 || pin == 16 || pin == 28);
#endif
        if (!available) {
            if (error) *error = String(roles[i]) + " 必须使用支持该功能的主控 GPIO，不能使用系统保留脚或 PCF 扩展脚";
            return false;
        }
        for (int j = 0; j < i; j++) if (pin == pins[j]) {
            if (error) *error = "RX、TX、DE 不能重复";
            return false;
        }
        // 只有 SDA/SCL 同时配置后 I2C 才真正占用引脚
        if (sda != PIN_NONE && scl != PIN_NONE && sda >= 0 && scl >= 0 && sda != scl &&
            (pin == sda || pin == scl)) {
            if (error) *error = io_config_pin_label(pin) + " 已被 I2C SDA/SCL 占用";
            return false;
        }
        for (int ch = 0; ch < active_channels; ch++) {
            if (pin == config.motor[ch][0] || pin == config.motor[ch][1] || pin == config.microswitch[ch]) {
                if (error) *error = io_config_pin_label(pin) + " 已被通道 " + String(ch + 1) + " 占用，请先修改并保存通道接线";
                return false;
            }
        }
    }
    return true;
}
