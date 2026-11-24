#include "PCF8575_PinMap.h"
#include "PCF8575.h"
#include "platform.h"
#include "i2c_bus.h"
#include "log_bus.h"
#include "settings_storage.h"

// 对象和输出状态归映射层管理，地址固定为 0x20～0x23
static PCF8575 pcf8575[PCF8575_DEVICE_COUNT] = {
    PCF8575(PCF8575_ADDR), PCF8575(PCF8575_ADDR + 1),
    PCF8575(PCF8575_ADDR + 2), PCF8575(PCF8575_ADDR + 3)
};
static uint16_t pcf8575_pin_states[PCF8575_DEVICE_COUNT] = {};
static uint16_t pcf8575_pin_modes[PCF8575_DEVICE_COUNT] = {}; // 1=输出，0=输入
static uint16_t pcf8575_idle_states[PCF8575_DEVICE_COUNT] = {};
static uint16_t pcf8575_idle_known[PCF8575_DEVICE_COUNT] = {};
static bool pcf8575_initialized[PCF8575_DEVICE_COUNT] = {};
static uint32_t pcf8575_last_attempt_ms[PCF8575_DEVICE_COUNT] = {};
static constexpr uint32_t PCF8575_PROBE_INTERVAL_MS = 1000;
static constexpr uint32_t PCF8575_RETRY_INTERVAL_MS = 5000;

/**
 * 生成扩展器启动、故障与恢复时的安全空闲电平
 * @param device 芯片索引
 * @return 电机方向全部低，低电平闭合的离合保持高电平释放
 * 显式绑定过的引脚使用各自空闲电平；首次启动从已加载接线补齐，避免低有效离合短暂误闭合
 */
static uint16_t pcf8575_safe_states(const uint8_t device) {
    uint16_t states = 0;
    if (IO_CONFIG.clutch_enabled && IO_CONFIG.clutch_active_level == LOW) {
        for (int ch = 0; ch < active_channel_count(); ++ch) {
            const int pin = IO_CONFIG.clutch[ch];
            if (IS_PCF8575_VIRTUAL_PIN(pin) && VIRTUAL_TO_PCF8575_DEVICE(pin) == device)
                states |= static_cast<uint16_t>(1U << VIRTUAL_TO_PCF8575_BIT(pin));
        }
    }
    return (states & ~pcf8575_idle_known[device]) | (pcf8575_idle_states[device] & pcf8575_idle_known[device]);
}

/**
 * 配置扩展引脚的安全空闲电平
 * @param pin 物理或扩展引脚编号
 * @param value 停机、故障和恢复时所需的电平
 * 仅登记扩展引脚，不直接写入；真实 GPIO 和未配置引脚忽略，绑定新功能时可覆盖旧用途
 */
void PCF8575_set_idle_level(const uint8_t pin, const uint8_t value) {
    if (!IS_PCF8575_VIRTUAL_PIN(pin)) return;
    const uint8_t device = VIRTUAL_TO_PCF8575_DEVICE(pin);
    const auto mask = static_cast<uint16_t>(1U << VIRTUAL_TO_PCF8575_BIT(pin));
    pcf8575_idle_known[device] |= mask;
    if (value == HIGH) pcf8575_idle_states[device] |= mask;
    else pcf8575_idle_states[device] &= ~mask;
}

/**
 * 标记单片 PCF8575 离线并丢弃旧输出
 * @param device 已校验的芯片索引 0～3
 * 只在在线转离线时记录日志，从失败时刻开始等待重试，不影响其他芯片
 * 尝试一次安全空闲电平写入，关闭电机并释放高或低有效离合；无论结果如何都等待重新初始化
 */
static void pcf8575_mark_offline(const uint8_t device) {
    if (pcf8575_initialized[device])
        LOG_MAIN("[i2c] PCF8575 0x%02X 失去应答，暂停使用并稍后重试", PCF8575_ADDR + device);
    pcf8575_initialized[device] = false;
    pcf8575_pin_states[device] = pcf8575_safe_states(device);
    pcf8575[device].write16(pcf8575_pin_states[device]);
    pcf8575[device].lastError();
    pcf8575_last_attempt_ms[device] = millis();
}

/**
 * 初始化单片 PCF8575
 * @param device 已校验的芯片索引 0～3
 * @return true 表示设备应答且安全空闲电平写入成功
 * 恢复时丢弃离线期间缓存的输出，保留通道已配置的输入输出模式
 */
static bool pcf8575_init_device(const uint8_t device) {
    if (pcf8575_initialized[device]) return true;
    pcf8575_last_attempt_ms[device] = millis();
    pcf8575_pin_states[device] = pcf8575_safe_states(device);
    pcf8575_initialized[device] = pcf8575[device].begin(pcf8575_pin_states[device]) &&
                                 pcf8575[device].lastError() == PCF8575_OK;
    return pcf8575_initialized[device];
}

/**
 * 初始化四个 PCF8575 地址槽
 * @return true 表示至少一片可以使用
 * 仅访问已初始化的共享总线；在线芯片保持现有输出，缺失芯片独立等待重试
 */
bool PCF8575_init() {
    if (!i2c_bus_ready()) return false;
    bool online = false;
    for (uint8_t device = 0; device < PCF8575_DEVICE_COUNT; ++device)
        online = pcf8575_init_device(device) || online;
    return online;
}

/**
 * 查询指定 PCF8575 是否在线
 * @param device 芯片索引 0～3
 * @return true 表示总线已初始化且该芯片最近一次初始化、探测或读写未失败
 * 只读取缓存，由主循环定期探测和读写结果更新；非法索引返回 false
 */
bool PCF8575_is_online(const uint8_t device) {
    return device < PCF8575_DEVICE_COUNT && i2c_bus_ready() && pcf8575_initialized[device];
}

/**
 * 查询虚拟引脚所属芯片是否在线
 * @param pin 虚拟引脚号
 * @return true 表示引脚有效且所属芯片当前缓存状态为在线
 * 真实 GPIO 和 PIN_NONE 不属于扩展芯片，返回 false
 */
bool PCF8575_pin_is_online(const uint8_t pin) {
    return IS_PCF8575_VIRTUAL_PIN(pin) && PCF8575_is_online(VIRTUAL_TO_PCF8575_DEVICE(pin));
}

/**
 * 定期探测在线芯片并重试离线 PCF8575
 * 在线芯片每秒检查应答且不改动输出，离线芯片每 5 秒独立重试；仅由主循环调用
 */
void PCF8575_poll() {
    if (!i2c_bus_ready()) return;
    const uint32_t now = millis();
    for (uint8_t device = 0; device < PCF8575_DEVICE_COUNT; ++device) {
        const uint32_t interval = pcf8575_initialized[device] ?
                                  PCF8575_PROBE_INTERVAL_MS : PCF8575_RETRY_INTERVAL_MS;
        if (now - pcf8575_last_attempt_ms[device] < interval) continue;
        if (pcf8575_initialized[device]) {
            pcf8575_last_attempt_ms[device] = now;
            if (!i2c_bus_probe(PCF8575_ADDR + device)) pcf8575_mark_offline(device);
        } else if (pcf8575_init_device(device)) {
            LOG_MAIN("[i2c] 已重新检测到 PCF8575 0x%02X，输出已恢复安全空闲电平", PCF8575_ADDR + device);
        }
    }
}

/**
 * 写入单片 PCF8575 并检查传输结果
 * @param device 已校验的芯片索引 0～3
 * @param states 要写入的 16 位电平
 * @return true 表示芯片在线且写入成功
 * 写入失败立即标记离线，避免后续操作覆盖本次错误
 */
static bool pcf8575_write_device(const uint8_t device, const uint16_t states) {
    if (!PCF8575_is_online(device)) return false;
    pcf8575[device].write16(states);
    if (pcf8575[device].lastError() == PCF8575_OK) return true;
    pcf8575_mark_offline(device);
    return false;
}

/**
 * 刷新所有在线 PCF8575 的输出状态
 * 每片只写入自身的 16 位缓存，离线芯片不访问总线，写入失败立即更新在线状态
 */
void PCF8575_update() {
    for (uint8_t device = 0; device < PCF8575_DEVICE_COUNT; ++device) {
        pcf8575_write_device(device, pcf8575_pin_states[device]);
    }
}

/**
 * 设置 PCF8575 虚拟引脚模式
 * @param pin 虚拟引脚号
 * @param mode INPUT 或 OUTPUT
 * 按芯片保存模式，离线时也可配置；模式设置不直接操作硬件
 */
void PCF8575_pinMode(const uint8_t pin, const uint8_t mode) {
    if (!IS_PCF8575_VIRTUAL_PIN(pin)) return;
    const uint8_t device = VIRTUAL_TO_PCF8575_DEVICE(pin);
    const auto mask = static_cast<uint16_t>(1U << VIRTUAL_TO_PCF8575_BIT(pin));
    if (mode == OUTPUT) pcf8575_pin_modes[device] |= mask;
    else pcf8575_pin_modes[device] &= ~mask;
}

/**
 * 设置 PCF8575 虚拟引脚电平
 * @param pin 虚拟引脚号
 * @param value HIGH 或 LOW
 * 只更新所属芯片的输出缓存并写入该芯片，失败标记离线；P10～P17 使用位序 8～15
 */
void PCF8575_digitalWrite(const uint8_t pin, const uint8_t value) {
    if (!IS_PCF8575_VIRTUAL_PIN(pin)) return;
    const uint8_t device = VIRTUAL_TO_PCF8575_DEVICE(pin);
    const auto mask = static_cast<uint16_t>(1U << VIRTUAL_TO_PCF8575_BIT(pin));
    if (value == HIGH) pcf8575_pin_states[device] |= mask;
    else pcf8575_pin_states[device] &= ~mask;
    pcf8575_write_device(device, pcf8575_pin_states[device]);
}

/**
 * 读取 PCF8575 虚拟引脚电平
 * @param pin 虚拟引脚号
 * @return HIGH 或 LOW；无效、离线或传输失败返回 LOW，调用方须在读取后检查在线状态
 * 输出返回自身缓存；输入先释放对应位，读取后恢复该芯片的原输出，不影响其他芯片
 * 每次传输后立即检查错误，失败标记离线，不把库缓存的旧输入当作有效读数
 */
int PCF8575_digitalRead(const uint8_t pin) {
    if (!PCF8575_pin_is_online(pin)) return LOW;
    const uint8_t device = VIRTUAL_TO_PCF8575_DEVICE(pin);
    const auto mask = static_cast<uint16_t>(1U << VIRTUAL_TO_PCF8575_BIT(pin));
    if (pcf8575_pin_modes[device] & mask)
        return pcf8575_pin_states[device] & mask ? HIGH : LOW;
    if (!pcf8575_write_device(device, pcf8575_pin_states[device] | mask)) return LOW;
    const uint16_t inputs = pcf8575[device].read16();
    if (pcf8575[device].lastError() != PCF8575_OK) {
        pcf8575_mark_offline(device);
        return LOW;
    }
    if (!pcf8575_write_device(device, pcf8575_pin_states[device])) return LOW;
    return inputs & mask ? HIGH : LOW;
}

/**
 * 用软件计数模拟 PCF8575 输出
 * @param pin 虚拟引脚号
 * @param value PWM 占空比，0～255
 * 用连续的 0～63 缓存索引为每个有效引脚独立计时，跳过虚拟编号空隙
 * 0 和 255 立即输出低或高，其余值由调用频率决定精度
 */
void PCF8575_analogWrite(const uint8_t pin, const uint8_t value) {
    if (!IS_PCF8575_VIRTUAL_PIN(pin)) return;
    static uint32_t last_pwm_time[PCF8575_VIRTUAL_PIN_COUNT] = {};
    static uint8_t pwm_counter[PCF8575_VIRTUAL_PIN_COUNT] = {};
    const uint8_t index = VIRTUAL_TO_PCF8575_INDEX(pin);
    if (value == 0 || value == 255) {
        PCF8575_digitalWrite(pin, value == 255 ? HIGH : LOW);
        return;
    }
    if (const uint32_t now = millis(); now - last_pwm_time[index] > 1) {
        last_pwm_time[index] = now;
        ++pwm_counter[index];
        PCF8575_digitalWrite(pin, value > pwm_counter[index] ? HIGH : LOW);
    }
}

// Arduino标准函数重写，用于透明替换

/**
 * 判断引脚是否为当前平台的真实 GPIO
 * @param pin 引脚号
 * @return true 表示合法 GPIO
 * 过滤 PIN_NONE、扩展编号及其空隙和平台不存在的编号，避免访问非法 GPIO
 */
static bool is_real_gpio(const uint8_t pin) {
    if (pin >= PCF8575_VIRTUAL_PIN_BASE) return false;
#if defined(ARDUINO_ARCH_ESP32)
    return digitalPinIsValid(pin);
#elif defined(NUM_DIGITAL_PINS)
    return pin < NUM_DIGITAL_PINS;
#else
    return true;
#endif
}

/**
 * 统一设置物理或扩展引脚模式
 * @param pin 引脚号或虚拟引脚号
 * @param mode 输入输出模式
 * 虚拟引脚转交 PCF8575_pinMode()，真实 GPIO 调用 Arduino pinMode()
 */
void pinMode_PCF8575(const uint8_t pin, const uint8_t mode) {
    if (IS_PCF8575_VIRTUAL_PIN(pin)) {
        PCF8575_pinMode(pin, mode);
    } else if (is_real_gpio(pin)) {
        pinMode(pin, mode);
    }
}

/**
 * 统一写入物理或扩展引脚电平
 * @param pin 引脚号或虚拟引脚号
 * @param value HIGH 或 LOW
 * 虚拟引脚走 PCF8575，真实 GPIO 走 Arduino digitalWrite()
 */
void digitalWrite_PCF8575(const uint8_t pin, const uint8_t value) {
    if (IS_PCF8575_VIRTUAL_PIN(pin)) {
        PCF8575_digitalWrite(pin, value);
    } else if (is_real_gpio(pin)) {
        digitalWrite(pin, value);
    }
}

/**
 * 统一读取物理或扩展引脚电平
 * @param pin 引脚号或虚拟引脚号
 * @return HIGH、LOW；非法引脚返回 LOW
 * 虚拟引脚走 PCF8575，真实 GPIO 走 Arduino digitalRead()
 */
int digitalRead_PCF8575(const uint8_t pin) {
    if (IS_PCF8575_VIRTUAL_PIN(pin)) {
        return PCF8575_digitalRead(pin);
    }
    if (is_real_gpio(pin)) {
        return digitalRead(pin);
    }
    return LOW;
}

/**
 * 统一输出物理或扩展引脚 PWM
 * @param pin 引脚号或虚拟引脚号
 * @param value PWM 值 0-255
 * 虚拟引脚使用软件 PWM，真实 GPIO 调用 Arduino analogWrite()
 */
void analogWrite_PCF8575(const uint8_t pin, const uint8_t value) {
    if (IS_PCF8575_VIRTUAL_PIN(pin)) {
        PCF8575_analogWrite(pin, value);
    } else if (is_real_gpio(pin)) {
        analogWrite(pin, value);
    }
}
