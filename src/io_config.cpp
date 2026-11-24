#include "io_config.h"
#include "platform.h"
#include "settings_storage.h"
#include "PCF8575_PinMap.h"
#include "project_presets.h"

/**
 * 判断引脚是否为系统保留脚
 * @param pin 引脚号
 * @return true 表示不可分配
 * 按平台过滤 Flash、USB、控制台串口等保留引脚；C3 UART0 和合宙原生 USB 由平台层按接线占用自动停用
 * 总微动与其他输入统一校验
 */
static bool is_reserved_pin(const int pin) {
#if defined(CONFIG_IDF_TARGET_ESP32C3)
    // Flash 不可重分配；UART0 可交给接线，合宙 USB 随 GPIO18/19 占用自动启停
    if (pin == 11 || (pin >= 14 && pin <= 17)) return true;
#if ARDUINO_USB_CDC_ON_BOOT
    if (pin == 18 || pin == 19) return true;
#endif
#elif defined(CONFIG_IDF_TARGET_ESP32S3)
    // S3 DevKit 的 26-37 接 Flash/PSRAM，19/20 保留给原生 USB
    if ((pin >= 26 && pin <= 37) || pin == 19 || pin == 20 || pin == 43 || pin == 44) return true;
#endif
    return false;
}

/**
 * 判断引脚是否为可用真实 GPIO
 * @param pin 引脚号
 * @return true 表示真实且未保留
 * 结合平台引脚范围与保留脚列表，过滤扩展编号及其空隙和不存在的 GPIO
 */
static bool is_real_gpio(const int pin) {
    if (pin < 0 || pin >= PCF8575_VIRTUAL_PIN_BASE || is_reserved_pin(pin)) return false;

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

/**
 * 生成平台默认引脚配置
 * @param config 待填充的 IO 配置
 * 由平台选择默认项目，C3 和网页复用同一份预设接线
 */
void io_config_set_defaults(IoConfig &config) {
    project_preset_io(hal_default_project_preset(), config);
}

/**
 * 初始化电磁离合配置的默认值
 * @param config 要补充默认值的接线配置
 * 保留独立电机、微动和霍尔接线，默认关闭离合模式并使用高电平闭合及 100ms 稳定等待
 */
void io_config_set_clutch_defaults(IoConfig &config) {
    config.clutch_enabled = 0;
    config.shared_motor[0] = config.shared_motor[1] = PIN_NONE;
    for (int32_t &pin : config.clutch) pin = PIN_NONE;
    config.clutch_active_level = HIGH;
    config.clutch_engage_ms = config.clutch_release_ms = DEFAULT_CLUTCH_DELAY_MS;
}

/**
 * 校验电磁离合模式及稳定时长
 * @param config 待校验的配置
 * @return 模式、电平和两个时长均在允许范围时返回 true
 * 引脚可用性和重复占用由整组接线校验处理
 */
bool io_config_clutch_valid(const IoConfig &config) {
    return (config.clutch_enabled == 0 || config.clutch_enabled == 1) &&
        (config.clutch_active_level == LOW || config.clutch_active_level == HIGH) &&
        config.clutch_engage_ms >= 0 && config.clutch_engage_ms <= MAX_CLUTCH_DELAY_MS &&
        config.clutch_release_ms >= 0 && config.clutch_release_ms <= MAX_CLUTCH_DELAY_MS;
}

/**
 * 查询当前模式的通道驱动引脚
 * @param config 通道配置
 * @param channel 通道编号
 * @param role 0=进料或离合，1=退料，2=微动，3=霍尔
 * @return 当前模式占用的引脚，离合模式没有通道退料引脚
 */
static int channel_pin(const IoConfig &config, const int channel, const int role) {
    if (role == 2) return config.microswitch[channel];
    if (role == 3) return config.hall[channel];
    return config.clutch_enabled ? (role == 0 ? config.clutch[channel] : PIN_NONE) : config.motor[channel][role];
}

/**
 * 判断当前生效的整套通道接线是否占用一根引脚
 * @param config 要查询的接线配置
 * @param active_channels 启用通道数
 * @param pin 要查询的引脚
 * @return 当前模式的驱动、微动或霍尔使用该引脚时返回 true
 * 未启用模式的接线只保存而不占用硬件
 */
bool io_config_uses_pin(const IoConfig &config, const int active_channels, const int pin) {
    if (pin == PIN_NONE) return false;
    if (pin == config.switch_pin) return true;
    if (config.clutch_enabled && (pin == config.shared_motor[0] || pin == config.shared_motor[1])) return true;
    for (int ch = 0; ch < min(active_channels, IO_CONFIG_MAX_CHANNELS); ++ch)
        for (int role = 0; role < 4; ++role)
            if (pin == channel_pin(config, ch, role)) return true;
    return false;
}

/**
 * 判断引脚在 I2C 总线状态切换后是否失效
 * @param pin 引脚号
 * @param i2c_configured 切换后的 I2C 总线是否启用
 * @param sda I2C SDA 引脚
 * @param scl I2C SCL 引脚
 * @return true 表示该引脚不再可用
 * 启用 I2C 时 SDA/SCL 不能再作通道脚；关闭时扩展虚拟引脚全部失效
 */
static bool pin_invalid_after_switch(const int pin, const bool i2c_configured, const int sda, const int scl) {
    if (pin == PIN_NONE) return false;
    if (!i2c_configured) return IS_PCF8575_VIRTUAL_PIN(pin);
    return pin == sda || pin == scl;
}

/**
 * 清理切换后失效的通道、霍尔和总微动引脚
 * @param config 待清理的 IO 配置
 * @param i2c_configured I2C 总线是否启用
 * @param sda I2C SDA 引脚
 * @param scl I2C SCL 引脚
 * @return 被清空的引脚数量，供日志使用
 * 独立或共享电机任一脚失效就整对清成 PIN_NONE；离合、微动、霍尔和总微动单独清理，包含未启用模式的缓存接线
 */
int io_config_clear_invalid_pins(IoConfig &config, const bool i2c_configured, const int sda, const int scl) {
    int cleared = 0;
    if (pin_invalid_after_switch(config.shared_motor[0], i2c_configured, sda, scl) ||
        pin_invalid_after_switch(config.shared_motor[1], i2c_configured, sda, scl)) {
        if (config.shared_motor[0] != PIN_NONE) cleared++;
        if (config.shared_motor[1] != PIN_NONE) cleared++;
        config.shared_motor[0] = config.shared_motor[1] = PIN_NONE;
    }
    if (pin_invalid_after_switch(config.switch_pin, i2c_configured, sda, scl)) {
        config.switch_pin = PIN_NONE;
        cleared++;
    }
    for (int i = 0; i < IO_CONFIG_MAX_CHANNELS; i++) {
        if (pin_invalid_after_switch(config.clutch[i], i2c_configured, sda, scl)) {
            config.clutch[i] = PIN_NONE;
            cleared++;
        }
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
        if (pin_invalid_after_switch(config.hall[i], i2c_configured, sda, scl)) {
            config.hall[i] = PIN_NONE;
            cleared++;
        }
    }
    return cleared;
}

/**
 * 判断 BambuBus 是否已配置 RX/TX
 * @param bus 可选候选接线，空指针使用当前配置
 * @return RX/TX 均不是 PIN_NONE 时返回 true
 * 仅查询配置状态，引脚合法性、单空和冲突由完整接线校验处理
 */
bool io_config_bus_enabled(const IoBusConfig *bus) {
    return (bus ? bus->rx : BUS_RX) != PIN_NONE && (bus ? bus->tx : BUS_TX) != PIN_NONE;
}

/**
 * 判断引脚是否被 BambuBus 占用
 * @param pin 引脚号
 * @param bus 可选候选接线，空指针使用当前配置
 * @return true 表示已被占用
 * 已启用有线总线时 RX/TX 占用，只有 DE 模式才占用 DE；双空 RX/TX 时全部释放
 */
static bool bus_uses_pin(const int pin, const IoBusConfig *bus = nullptr) {
    if (pin == PIN_NONE || !io_config_bus_enabled(bus)) return false;
    if (bus) return pin == bus->rx || pin == bus->tx || (bus->mode == RS485_MODE_DE && pin == bus->de);
    return pin == BUS_RX || pin == BUS_TX || (RS485_MODE == RS485_MODE_DE && pin == BUS_DE);
}

/**
 * 判断引脚能否出现在网页选择列表中
 * @param pin 引脚号
 * @param allow_none 是否允许 PIN_NONE
 * @param bus 可选候选总线接线，空指针使用当前配置
 * @return true 表示可用
 * 综合检查虚拟引脚开关、I2C 占用、系统保留脚和总线占用
 */
bool io_config_pin_available(const int pin, const bool allow_none, const IoBusConfig *bus) {
    if (allow_none && pin == PIN_NONE) return true;
    // 虚拟引脚只在 I2C 总线已配置后可选；未配置时 I2C 引脚可作普通 IO
    const int sda = bus ? bus->sda : PCF_SDA, scl = bus ? bus->scl : PCF_SCL;
    const bool configured = sda != PIN_NONE && scl != PIN_NONE && sda >= 0 && scl >= 0 && sda != scl;
    if (IS_PCF8575_VIRTUAL_PIN(pin)) return configured;
    if (configured && (pin == sda || pin == scl)) return false;
    return is_real_gpio(pin) && !bus_uses_pin(pin, bus);
}

/**
 * 判断引脚能否用于线性霍尔输入
 * @param pin 主控 ADC 引脚编号
 * @param allow_none 是否允许未配置
 * @param bus 可选候选总线接线，空指针使用当前配置
 * @return true 表示支持 ADC 且未被系统、总线或 I2C 占用
 * 通道之间的重复占用由整组接线校验检查，PCF8575 不提供模拟输入
 */
bool io_config_hall_pin_available(const int pin, const bool allow_none, const IoBusConfig *bus) {
    if (allow_none && pin == PIN_NONE) return true;
    return hal_adc_pin_available(pin) && io_config_pin_available(pin, false, bus);
}

/**
 * 清理升级后无法继续使用的生效引脚
 * @param config 要修复的接线配置
 * @param active_channels 启用通道数
 * @param bus 候选总线接线
 * @return 清空的不可用或重复引脚数量
 * 保留每根仍有效的引脚和未启用模式接线，仅把当前模式不可用或重复的字段置为 PIN_NONE
 */
int io_config_sanitize_pins(IoConfig &config, const int active_channels, const IoBusConfig *bus) {
    int32_t *pins[IO_CONFIG_MAX_CHANNELS * 4 + 3];
    bool hall[IO_CONFIG_MAX_CHANNELS * 4 + 3] = {};
    int count = 0, cleared = 0;
    pins[count++] = &config.switch_pin;
    if (config.clutch_enabled) {
        pins[count++] = &config.shared_motor[0];
        pins[count++] = &config.shared_motor[1];
    }
    for (int ch = 0; ch < min(active_channels, IO_CONFIG_MAX_CHANNELS); ++ch) {
        if (config.clutch_enabled) pins[count++] = &config.clutch[ch];
        else {
            pins[count++] = &config.motor[ch][0];
            pins[count++] = &config.motor[ch][1];
        }
        pins[count++] = &config.microswitch[ch];
        hall[count] = true;
        pins[count++] = &config.hall[ch];
    }
    for (int i = 0; i < count; ++i) {
        if (*pins[i] == PIN_NONE) continue;
        bool valid = hall[i] ? io_config_hall_pin_available(*pins[i], false, bus) : io_config_pin_available(*pins[i], false, bus);
        for (int prev = 0; prev < i; ++prev) if (*pins[prev] == *pins[i]) valid = false;
        if (!valid) { *pins[i] = PIN_NONE; ++cleared; }
    }
    return cleared;
}

/**
 * 判断引脚能否作为 I2C 引脚
 * @param pin 引脚号
 * @param sda true 表示按 SDA 校验，false 表示按 SCL 校验
 * @return true 表示可用
 * RP2040 额外校验 I2C0 的固定 SDA/SCL 复用位置
 */
static bool i2c_pin_available(const int pin, bool sda) {
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
 * 检查通道数、与总线/通道/霍尔/总微动引脚的冲突、SDA/SCL 是否不同以及平台复用限制
 */
bool io_config_i2c_validate(const int sda, const int scl, const IoConfig &config, const int active_channels,
                            String *error,
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
    if (config.switch_pin != PIN_NONE && (config.switch_pin == sda || config.switch_pin == scl)) {
        if (error) *error = "所选 GPIO 已被五通总微动占用，请先修改总微动接线";
        return false;
    }
    if (config.clutch_enabled && (config.shared_motor[0] == sda || config.shared_motor[0] == scl ||
        config.shared_motor[1] == sda || config.shared_motor[1] == scl)) {
        if (error) *error = "所选 GPIO 已被共享电机占用，请先修改共享电机接线";
        return false;
    }
    for (int ch = 0; ch < active_channels; ch++) {
        for (int role = 0; role < 4; role++) {
            const int pin = channel_pin(config, ch, role);
            if (pin == sda || pin == scl) {
                if (error)
                    *error = io_config_pin_label(pin) + " 已被通道 " + String(ch + 1) +
                             (role == 0 ? (config.clutch_enabled ? " 离合" : " 进料") : role == 1 ? " 退料" : role == 2 ? " 微动" : " 霍尔") + " 使用，请先修改通道配置";
                return false;
            }
        }
    }
    return true;
}

/**
 * 获取引脚风险提示文本
 * @param pin 引脚号
 * @return 中文提示；无风险时返回空串
 * 为 ESP32-C3/S3 的启动绑带脚及 C3 控制台复用提供网页提示
 */
static const char *pin_warning_text(int pin) {
#if defined(CONFIG_IDF_TARGET_ESP32C3)
#if !ARDUINO_USB_CDC_ON_BOOT
    if (pin == 20 || pin == 21) return "GPIO20/21 可用作普通 IO；占用任一脚时自动关闭 UART0 控制台，主板日志仍可在网页查看";
#if defined(BMCU_USB_CDC_AUTO)
    if (pin == 18 || pin == 19) return "GPIO18/19 可用作普通 IO；占用任一脚时自动关闭原生 USB 日志并释放两脚，两脚空闲时恢复；主板日志仍可在网页查看";
#endif
#endif
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

/**
 * 判断引脚是否需要风险提示
 * @param pin 引脚号
 * @return true 表示存在风险提示
 * 根据 pin_warning_text() 是否为空判断
 */
bool io_config_pin_has_warning(const int pin) {
    return pin_warning_text(pin)[0] != '\0';
}

/**
 * 生成引脚可读名称
 * @param pin 引脚号
 * @return “无”、IOx 或带虚拟编号、扩展器序号和 Px 的名称
 * 网页和日志按扩展器序号区分四片芯片；面板名称与寄存器位序分开转换
 */
String io_config_pin_label(const int pin) {
    if (pin == PIN_NONE) return "无";
    if (IS_PCF8575_VIRTUAL_PIN(pin)) {
        char label[40];
        snprintf(label, sizeof(label), "%d 扩展%d(P%d)", pin,
                 VIRTUAL_TO_PCF8575_DEVICE(pin) + 1, VIRTUAL_TO_PCF8575_PIN(pin));
        return {label};
    }
    return String("IO") + String(pin);
}

/**
 * 校验整组通道接线
 * @param config 通道接线配置
 * @param active_channels 启用通道数
 * @param error 可选的错误文本输出
 * @param bus 可选候选总线接线，空指针使用当前配置
 * @return true 表示配置合法
 * 按生效模式检查独立电机或共享电机与离合，并校验微动、霍尔和总微动的可用性及重复占用；另一模式接线保留但不占用硬件
 */
bool io_config_validate(const IoConfig &config, const int active_channels, String *error, const IoBusConfig *bus) {
    if (active_channels < 1 || active_channels > IO_CONFIG_MAX_CHANNELS) {
        if (error) *error = "通道数量无效";
        return false;
    }

    if (!io_config_clutch_valid(config)) {
        if (error) *error = "电磁离合模式、电平或稳定时长无效";
        return false;
    }
    if (!io_config_pin_available(config.switch_pin, true, bus)) {
        if (error) *error = "五通总微动引脚不可用：" + io_config_pin_label(config.switch_pin);
        return false;
    }
    int assigned[IO_CONFIG_MAX_CHANNELS * 4 + 3] = {};
    int assigned_count = 0;
    if (config.switch_pin != PIN_NONE) assigned[assigned_count++] = config.switch_pin;
    if (config.clutch_enabled) {
        for (int role = 0; role < 2; ++role) {
            const int pin = config.shared_motor[role];
            if (!io_config_pin_available(pin, true, bus)) {
                if (error) *error = String("共享电机") + (role == 0 ? "进料" : "退料") + "引脚不可用";
                return false;
            }
            if (pin == PIN_NONE) continue;
            for (int i = 0; i < assigned_count; ++i) if (assigned[i] == pin) {
                if (error) *error = "引脚重复使用：" + io_config_pin_label(pin);
                return false;
            }
            assigned[assigned_count++] = pin;
        }
    }
    for (int ch = 0; ch < active_channels; ch++) {
        for (int role = 0; role < 4; role++) {
            const int pin = channel_pin(config, ch, role);
            // "无"(PIN_NONE) 合法：切换 I2C 配置会先清空不再可用的引脚，
            // 该通道等用户在网页里补配置
            if (role == 3 ? !io_config_hall_pin_available(pin, true, bus) : !io_config_pin_available(pin, true, bus)) {
                if (error)
                    *error = String("通道 ") + String(ch + 1) + " 的" +
                             (role == 0 ? (config.clutch_enabled ? "离合" : "进料") : role == 1 ? "退料" : role == 2 ? "微动" : "霍尔 ADC") +
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

/**
 * 生成网页可选引脚 JSON 数组
 * @param json 待追加的 JSON 字符串
 * @param i2c_only true 时只列真实 GPIO
 * 把引脚编号、显示名称和风险提示拼接为网页下拉框需要的数组
 */
void io_config_append_pin_options(String &json, const bool i2c_only) {
    json += "[";
    bool first = true;
    for (int pin = 0; pin < 255; pin++) {
        if (i2c_only ? !is_real_gpio(pin) : !io_config_pin_available(pin)) continue;
        if (!first) json += ',';
        first = false;
        json += "{\"id\":" + String(pin) +
                R"(,"label":")" + io_config_pin_label(pin) +
                R"(","warning":)" + String(io_config_pin_has_warning(pin) ? "true" : "false") +
                R"(,"warning_text":")" + pin_warning_text(pin) + "\"}";
    }
    json += "]";
}

/**
 * 生成霍尔 ADC 可选引脚 JSON 数组
 * @param json 待追加的 JSON 字符串
 * 使用与普通引脚相同的字段和启动风险提示，仅保留主控支持的可用模拟输入
 */
void io_config_append_hall_pin_options(String &json) {
    json += "[";
    bool first = true;
    for (int pin = 0; pin < 255; ++pin) {
        if (!io_config_hall_pin_available(pin)) continue;
        if (!first) json += ',';
        first = false;
        json += "{\"id\":" + String(pin) +
                R"(,"label":")" + io_config_pin_label(pin) +
                R"(","warning":)" + String(io_config_pin_has_warning(pin) ? "true" : "false") +
                R"(,"warning_text":")" + pin_warning_text(pin) + "\"}";
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
 * 按候选 I2C、通道电机/微动/霍尔和总微动接线检查冲突，同时检查 RX/TX/DE 合法性和平台复用限制
 * ESP32 允许 RX/TX 同时为 PIN_NONE 并忽略 DE；其他平台保持有线要求，单空 RX/TX 一律拒绝
 */
bool io_config_bus_validate(const int rx, const int tx, const int de, const int mode, const int sda, const int scl,
                            const IoConfig &config, const int active_channels, String *error) {
    if ((mode != RS485_MODE_RXTX && mode != RS485_MODE_DE) ||
        active_channels < 1 || active_channels > IO_CONFIG_MAX_CHANNELS) {
        if (error) *error = "总线模式或通道数量无效";
        return false;
    }
    if ((rx == PIN_NONE) != (tx == PIN_NONE)) {
        if (error) *error = "RX、TX 必须同时配置或同时为无";
        return false;
    }
#if defined(ARDUINO_ARCH_ESP32)
    if (rx == PIN_NONE && tx == PIN_NONE) return true;
#endif
    const int pins[] = {rx, tx, de};
    for (int i = 0; i < (mode == RS485_MODE_DE ? 3 : 2); i++) {
        const int pin = pins[i];
        bool available = is_real_gpio(pin);
#if defined(ARDUINO_ARCH_RP2040)
        // 固定 UART0：RX=1/13/17/29，TX=0/12/16/28
        if (i == 0) available = available && (pin == 1 || pin == 13 || pin == 17 || pin == 29);
        if (i == 1) available = available && (pin == 0 || pin == 12 || pin == 16 || pin == 28);
#endif
        if (!available) {
            const char *roles[] = {"RX", "TX", "DE"};
            if (error) *error = String(roles[i]) + " 必须使用支持该功能的主控 GPIO，不能使用系统保留脚或 PCF 扩展脚";
            return false;
        }
        if (pin == config.switch_pin) {
            if (error) *error = io_config_pin_label(pin) + " 已被五通总微动占用";
            return false;
        }
        for (int j = 0; j < i; j++)
            if (pin == pins[j]) {
                if (error) *error = "RX、TX、DE 不能重复";
                return false;
            }
        // 只有 SDA/SCL 同时配置后 I2C 才真正占用引脚
        if (sda != PIN_NONE && scl != PIN_NONE && sda >= 0 && scl >= 0 && sda != scl &&
            (pin == sda || pin == scl)) {
            if (error) *error = io_config_pin_label(pin) + " 已被 I2C SDA/SCL 占用";
            return false;
        }
        if (io_config_uses_pin(config, active_channels, pin)) {
            if (error) *error = io_config_pin_label(pin) + " 已被电机、离合或通道输入占用，请先修改通道配置";
            return false;
        }
    }
    return true;
}
