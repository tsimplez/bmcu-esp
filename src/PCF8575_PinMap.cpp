/**
 * PCF8575 引脚映射实现：四片独立缓存，共用已配置的 I2C 总线
 */
#include "PCF8575_PinMap.h"
#include "PCF8575.h"
#include "platform.h"
#include "i2c_bus.h"

// 对象和输出状态归映射层管理，地址固定为 0x20～0x23
static PCF8575 pcf8575[PCF8575_DEVICE_COUNT] = {
    PCF8575(PCF8575_ADDR), PCF8575(PCF8575_ADDR + 1),
    PCF8575(PCF8575_ADDR + 2), PCF8575(PCF8575_ADDR + 3)
};
static uint16_t pcf8575_pin_states[PCF8575_DEVICE_COUNT] = {};
static uint16_t pcf8575_pin_modes[PCF8575_DEVICE_COUNT] = {}; // 1=输出，0=输入
static bool pcf8575_initialized[PCF8575_DEVICE_COUNT] = {};
static uint32_t pcf8575_last_attempt_ms[PCF8575_DEVICE_COUNT] = {};

/**
 * 初始化单片 PCF8575
 * @param device 已校验的芯片索引 0～3
 * @return true 表示设备应答且全低电平写入成功
 * 恢复时丢弃离线期间缓存的输出，保留通道已配置的输入输出模式
 */
static bool pcf8575_init_device(const uint8_t device) {
    if (pcf8575_initialized[device]) return true;
    pcf8575_last_attempt_ms[device] = millis();
    pcf8575_pin_states[device] = 0;
    pcf8575_initialized[device] = pcf8575[device].begin(0) &&
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
 * @return true 表示总线已初始化且该芯片初始化成功
 * 非法索引返回 false，不访问其他芯片
 */
bool PCF8575_is_online(const uint8_t device) {
    return device < PCF8575_DEVICE_COUNT && i2c_bus_ready() && pcf8575_initialized[device];
}

/**
 * 查询虚拟引脚所属芯片是否在线
 * @param pin 虚拟引脚号
 * @return true 表示引脚有效且所属芯片已初始化
 * 真实 GPIO 和 PIN_NONE 不属于扩展芯片，返回 false
 */
bool PCF8575_pin_is_online(const uint8_t pin) {
    return IS_PCF8575_VIRTUAL_PIN(pin) && PCF8575_is_online(VIRTUAL_TO_PCF8575_DEVICE(pin));
}

/**
 * 定期重试离线 PCF8575
 * 每片独立限频为 5 秒；其他芯片已经在线时仍继续尝试缺失地址
 */
void PCF8575_poll() {
    if (!i2c_bus_ready()) return;
    const uint32_t now = millis();
    for (uint8_t device = 0; device < PCF8575_DEVICE_COUNT; ++device) {
        if (!pcf8575_initialized[device] && now - pcf8575_last_attempt_ms[device] >= 5000)
            pcf8575_init_device(device);
    }
}

/**
 * 刷新所有在线 PCF8575 的输出状态
 * 每片只写入自身的 16 位缓存，离线芯片不访问总线
 */
void PCF8575_update() {
    for (uint8_t device = 0; device < PCF8575_DEVICE_COUNT; ++device) {
        if (PCF8575_is_online(device)) pcf8575[device].write16(pcf8575_pin_states[device]);
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
    const uint16_t mask = uint16_t(1U << VIRTUAL_TO_PCF8575_BIT(pin));
    if (mode == OUTPUT) pcf8575_pin_modes[device] |= mask;
    else pcf8575_pin_modes[device] &= ~mask;
}

/**
 * 设置 PCF8575 虚拟引脚电平
 * @param pin 虚拟引脚号
 * @param value HIGH 或 LOW
 * 只更新所属芯片的输出缓存并写入该芯片，P10～P17 使用位序 8～15
 */
void PCF8575_digitalWrite(const uint8_t pin, const uint8_t value) {
    if (!IS_PCF8575_VIRTUAL_PIN(pin)) return;
    const uint8_t device = VIRTUAL_TO_PCF8575_DEVICE(pin);
    const uint16_t mask = uint16_t(1U << VIRTUAL_TO_PCF8575_BIT(pin));
    if (value == HIGH) pcf8575_pin_states[device] |= mask;
    else pcf8575_pin_states[device] &= ~mask;
    if (PCF8575_is_online(device)) pcf8575[device].write16(pcf8575_pin_states[device]);
}

/**
 * 读取 PCF8575 虚拟引脚电平
 * @param pin 虚拟引脚号
 * @return HIGH 或 LOW；无效或离线返回 LOW，调用方须先检查在线状态
 * 输出返回自身缓存；输入先释放对应位，读取后恢复该芯片的原输出，不影响其他芯片
 */
int PCF8575_digitalRead(const uint8_t pin) {
    if (!PCF8575_pin_is_online(pin)) return LOW;
    const uint8_t device = VIRTUAL_TO_PCF8575_DEVICE(pin);
    const uint16_t mask = uint16_t(1U << VIRTUAL_TO_PCF8575_BIT(pin));
    if (pcf8575_pin_modes[device] & mask)
        return pcf8575_pin_states[device] & mask ? HIGH : LOW;
    pcf8575[device].write16(pcf8575_pin_states[device] | mask);
    const uint16_t inputs = pcf8575[device].read16();
    pcf8575[device].write16(pcf8575_pin_states[device]);
    return inputs & mask ? HIGH : LOW;
}

/**
 * 用软件计数模拟 PCF8575 输出
 * @param pin 虚拟引脚号
 * @param value PWM 占空比，0～255
 * 每个虚拟引脚独立计时；0 和 255 立即输出低或高，其余值由调用频率决定精度
 */
void PCF8575_analogWrite(const uint8_t pin, const uint8_t value) {
    if (!IS_PCF8575_VIRTUAL_PIN(pin)) return;
    static uint32_t last_pwm_time[PCF8575_VIRTUAL_PIN_COUNT] = {};
    static uint8_t pwm_counter[PCF8575_VIRTUAL_PIN_COUNT] = {};
    const uint8_t index = pin - PCF8575_VIRTUAL_PIN_BASE;
    if (value == 0 || value == 255) {
        PCF8575_digitalWrite(pin, value == 255 ? HIGH : LOW);
        return;
    }
    const uint32_t now = millis();
    if (now - last_pwm_time[index] > 1) {
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
 * 过滤 PIN_NONE 和平台不存在的编号，避免向 Arduino core 传入非法引脚
 */
static bool is_real_gpio(const uint8_t pin) {
    if (pin == 255) return false;
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
