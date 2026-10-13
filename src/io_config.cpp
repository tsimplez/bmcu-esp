#include "io_config.h"
#include "platform.h"
#include "settings_storage.h"
#include "PCF8575_PinMap.h"

// 是否是预留引脚
static bool is_reserved_pin(int pin) {
#if defined(PIN_SWITCH)
    if (pin != PIN_NONE && pin == PIN_SWITCH) return true;
#endif

#if defined(CONFIG_IDF_TARGET_ESP32C3)
    // Flash 和控制台不可重分配；经典款的 USB 焊盘可用作普通 GPIO。
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
    // S3 DevKit 的 26-37 接 Flash/PSRAM，19/20 保留给原生 USB。
    if ((pin >= 26 && pin <= 37) || pin == 19 || pin == 20 || pin == 43 || pin == 44) return true;
#endif
    return false;
}

// 是否是真正的io引脚
static bool is_real_gpio(int pin) {
    if (pin < 0 || pin >= 255 || is_reserved_pin(pin)) return false;

#if defined(CONFIG_IDF_TARGET_ESP32C3)
    // C3 的可选范围只保留实际引出的 GPIO；保留 2/8 是为了兼容现有默认映射，
    // 由网页另外显示启动风险提示。
    return pin <= 21;
#elif defined(ARDUINO_ARCH_ESP32)
    return digitalPinIsValid(pin);
#elif defined(NUM_DIGITAL_PINS)
    return pin < NUM_DIGITAL_PINS;
#else
    // RP2040 Arduino core 没有统一导出 NUM_DIGITAL_PINS 时，板上 GPIO 为 0-29。
    return pin < 30;
#endif
}

// io配置设置默认值
void io_config_set_defaults(IoConfig &config) {
    static const int default_motor[IO_CONFIG_MAX_CHANNELS][2] = {
        MOTOR_PINS_1_4,MOTOR_PINS_5_8,MOTOR_PINS_9_12,MOTOR_PINS_13_16
    };
    static const int default_switch[IO_CONFIG_MAX_CHANNELS] = {MSW_PINS_1_4, MSW_PINS_5_8,MSW_PINS_9_12,MSW_PINS_13_16};

    for (int i = 0; i < IO_CONFIG_MAX_CHANNELS; i++) {
        config.motor[i][0] = default_motor[i][0];
        config.motor[i][1] = default_motor[i][1];
        config.microswitch[i] = default_switch[i];
    }
}

// 该引脚在切换 PCF8575 开关后是否还能用：启用后 SDA/SCL 不能再用作通道引脚，
// 关闭后扩展虚拟引脚不再存在。
static bool pin_invalid_after_switch(int pin, bool pcf_enable, int sda, int scl) {
    if (pin == PIN_NONE) return false;
    if (!pcf_enable) return IS_PCF8575_VIRTUAL_PIN(pin);
    return pin == sda || pin == scl;
}

// 切换 PCF8575 开关时只清掉不再合法的引脚，其余接线原样保留，避免覆盖用户已接线。
// 电机是一对，任一脚被占用就整对清掉；清成 PIN_NONE(无) 表示该通道待配置。
// 返回清掉的引脚数量，供日志使用。
int io_config_clear_invalid_pins(IoConfig &config, bool pcf_enable, int sda, int scl) {
    int cleared = 0;
    for (int i = 0; i < IO_CONFIG_MAX_CHANNELS; i++) {
        if (pin_invalid_after_switch(config.motor[i][0], pcf_enable, sda, scl) ||
            pin_invalid_after_switch(config.motor[i][1], pcf_enable, sda, scl)) {
            if (config.motor[i][0] != PIN_NONE) cleared++;
            if (config.motor[i][1] != PIN_NONE) cleared++;
            config.motor[i][0] = PIN_NONE;
            config.motor[i][1] = PIN_NONE;
        }
        if (pin_invalid_after_switch(config.microswitch[i], pcf_enable, sda, scl)) {
            config.microswitch[i] = PIN_NONE;
            cleared++;
        }
    }
    return cleared;
}

static bool bus_uses_pin(int pin) {
    return pin == BUS_RX || pin == BUS_TX || (RS485_MODE == RS485_MODE_DE && pin == BUS_DE);
}

// io配置是否引脚可用
bool io_config_pin_available(int pin, bool allow_none) {
    if (allow_none && pin == PIN_NONE) return true;
    // 虚拟引脚只在 Web 开关打开 PCF8575 后可选；关闭时 I2C 引脚可作普通 IO。
    if (IS_PCF8575_VIRTUAL_PIN(pin)) return PCF_ENABLE != 0;
    if (PCF_ENABLE && (pin == PCF_SDA || pin == PCF_SCL)) return false;
    return is_real_gpio(pin) && !bus_uses_pin(pin);
}

static bool i2c_pin_available(int pin, bool sda) {
#if defined(ARDUINO_ARCH_RP2040)
    // Wire 使用 I2C0，RP2040 的 SDA/SCL 复用位置固定。
    return is_real_gpio(pin) && pin % 4 == (sda ? 0 : 1);
#else
    return is_real_gpio(pin);
#endif
}

bool io_config_i2c_validate(int sda, int scl, const IoConfig &config, int active_channels, String *error) {
    if (active_channels < 1 || active_channels > IO_CONFIG_MAX_CHANNELS) {
        if (error) *error = "通道数量无效";
        return false;
    }
    if (bus_uses_pin(sda) || bus_uses_pin(scl)) {
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
static const char *pin_warning_text(int pin) {
#if defined(CONFIG_IDF_TARGET_ESP32C3)
    if (pin == 2) return "ESP32-C3 GPIO2 是启动绑带引脚，建议上电或复位时保持高电平，避免外部电路影响启动。正常运行时可作为普通 IO。";
    if (pin == 9) return "ESP32-C3 GPIO9 连接 BOOT 按键，是启动绑带引脚；上电或复位时拉低会进入下载模式，外部电路需保持高电平才能正常启动。";
    if (pin == 8) return "ESP32-C3 GPIO8：进入串口下载模式时必须为高电平；复位时被微动开关等外部电路拉低，会导致无法进入下载模式、无法刷入固件。刷机前请释放该引脚。正常运行时可作为普通 IO。";
#elif defined(CONFIG_IDF_TARGET_ESP32S3)
    if (pin == 0) return "ESP32-S3 GPIO0：上电或复位时拉低会选择下载启动模式；正常运行固件时应避免外部电路在复位时将其拉低。";
#else
    (void) pin;
#endif
    return "";
}

// io配置引脚有警告
bool io_config_pin_has_warning(int pin) {
    return pin_warning_text(pin)[0] != '\0';
}

// io配置引脚标签
String io_config_pin_label(int pin) {
    if (pin == PIN_NONE) return "无";
    if (IS_PCF8575_VIRTUAL_PIN(pin)) return String("PCF8575 P") + String(VIRTUAL_TO_PCF8575_PIN(pin));
    return String("IO") + String(pin);
}

// io配置验证
bool io_config_validate(const IoConfig &config, int active_channels, String *error) {
    if (active_channels < 1 || active_channels > IO_CONFIG_MAX_CHANNELS) {
        if (error) *error = "通道数量无效";
        return false;
    }

    int assigned[IO_CONFIG_MAX_CHANNELS * 3] = {};
    int assigned_count = 0;
    for (int ch = 0; ch < active_channels; ch++) {
        for (int role = 0; role < 3; role++) {
            const int pin = role < 2 ? config.motor[ch][role] : config.microswitch[ch];
            // "无"(PIN_NONE) 合法：切换 PCF8575 开关会先清空不再可用的引脚，
            // 该通道等用户在网页里补配置。
            if (!io_config_pin_available(pin, true)) {
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
        // 兼容 Pico 旧板级映射中 PIN_SWITCH 与默认 UART0 TX 共用 GPIO0 的布局。
        // 该引脚仍不开放给通道和 I2C；不在本次 C3 配置中改变旧板行为。
        if (i == 1 && pin == PIN_BUS_TX && pin == PIN_SWITCH) available = true;
        // 固定 UART0：RX=1/13/17/29，TX=0/12/16/28。
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
        // 只有启用 PCF8575 后 SDA/SCL 才真正占用引脚。
        if (PCF_ENABLE && (pin == sda || pin == scl)) {
            if (error) *error = io_config_pin_label(pin) + " 已被 PCF8575 SDA/SCL 占用";
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
