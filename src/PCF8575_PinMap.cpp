/**
 * PCF8575引脚映射实现文件
 * 
 * 实现PCF8575虚拟引脚功能，将PCF8575的0-15号引脚映射为Pico的30-45号虚拟引脚
 */
#include "PCF8575_PinMap.h"
#include "platform.h"
#include "i2c_bus.h"

// 使用在BMCU.cpp中已经定义的PCF8575对象
extern PCF8575 pcf8575;

// 引脚状态缓存
// 16 位输出锁存缓存：bit0..15 对应 PCF8575 的 P0..P15，1=高电平
static uint16_t pcf8575_pin_states = 0;
// 16 位引脚模式缓存：0=INPUT，1=OUTPUT，逐位对应 P0..P15
static uint16_t pcf8575_pin_modes = 0;
// 各物理引脚最近一次写入的 PWM 占空比；数组覆盖 P0..P17 的虚拟编号
static uint8_t pcf8575_pwm_values[18] = {};
// PCF8575 是否已完成初始化；true 时重复调用 PCF8575_init() 直接返回成功
static bool pcf8575_initialized = false;
// 上一次尝试初始化 PCF8575 的 millis() 时间，用于离线重试限频
static uint32_t pcf8575_last_attempt_ms = 0;

#if defined(ARDUINO_ARCH_RP2040)
// Arduino-Mbed 的全局 Wire 对象没有 setSDA/setSCL；按设置创建一个使用自定义
// 引脚的 I2C 实例，并把 PCF8575 切换到该实例。设置只在重启后生效。
// RP2040 专用的 I2C 实例；PCF8575_init() 首次调用时按 PCF_SDA/PCF_SCL 创建
static arduino::MbedI2C *pcf8575_wire = nullptr;
#endif

/**
 * 初始化 PCF8575 扩展芯片
 * @return true 表示初始化成功
 * 按配置创建 I2C 实例并启动 PCF8575，所有输出初始化为低电平；重复调用直接返回成功。
 */
bool PCF8575_init() {
    if (pcf8575_initialized) {
        return true;
    }
    if (!i2c_bus_configured()) {
        return false;
    }
    pcf8575_last_attempt_ms = millis();

#if defined(ARDUINO_ARCH_RP2040)
    if (!pcf8575_wire) {
        pcf8575_wire = new arduino::MbedI2C(PCF_SDA, PCF_SCL);
        pcf8575_wire->begin();
        pcf8575_wire->setClock(400000);
        pcf8575 = PCF8575(PCF8575_ADDR, pcf8575_wire);
    }
#endif

    if (!pcf8575.begin()) {
        pcf8575_initialized = false;
        return false;
    }
    
    // 初始化所有引脚为低电平
    pcf8575.write16(0);
    pcf8575_pin_states = 0;
    pcf8575_pin_modes = 0; // 默认所有引脚为INPUT
    
    pcf8575_initialized = true;
    return true;
}

/**
 * 查询 PCF8575 是否在线
 * @return true 表示芯片已完成初始化且可以读写
 * 未配置 I2C 或芯片未应答时返回 false，调用方应停用扩展虚拟引脚。
 */
bool PCF8575_is_online() {
    return pcf8575_initialized;
}

/**
 * 定期重试离线 PCF8575
 * 未在线的芯片每 5 秒重新探测一次，避免启动时接触不良导致永久禁用。
 */
void PCF8575_poll() {
    if (pcf8575_initialized || !i2c_bus_configured()) return;
    if (millis() - pcf8575_last_attempt_ms < 5000) return;
    PCF8575_init();
}

/**
 * 刷新 PCF8575 输出状态
 * 把缓存的 16 位输出状态一次写入扩展芯片。
 */
void PCF8575_update() {
    if (!pcf8575_initialized) {
        return;
    }
    
    // 更新输出引脚状态
    pcf8575.write16(pcf8575_pin_states);
    
    // 读取输入引脚状态（如果需要的话）
    // 注意：PCF8575是输出扩展器，输入功能有限
    // uint16_t input_states = pcf8575.read16();
}

/**
 * 设置 PCF8575 虚拟引脚模式
 * @param pin 虚拟引脚号
 * @param mode INPUT 或 OUTPUT
 * 只更新引脚模式缓存，不直接操作硬件。
 */
void PCF8575_pinMode(const uint8_t pin, const uint8_t mode) {
    if (!IS_PCF8575_VIRTUAL_PIN(pin)) {
        return;
    }

    const uint8_t pcf_pin = VIRTUAL_TO_PCF8575_PIN(pin);
    
    if (mode == OUTPUT) {
        pcf8575_pin_modes |= 1 << pcf_pin;
    } else {
        pcf8575_pin_modes &= ~(1 << pcf_pin);
    }
}

/**
 * 设置 PCF8575 虚拟引脚电平
 * @param pin 虚拟引脚号
 * @param value HIGH 或 LOW
 * 更新输出位缓存并立即写入芯片。
 */
void PCF8575_digitalWrite(const uint8_t pin, const uint8_t value) {
    if (!IS_PCF8575_VIRTUAL_PIN(pin)) {
        return;
    }

    const uint8_t pcf_pin = VIRTUAL_TO_PCF8575_PIN(pin);
    
    if (value == HIGH) {
        pcf8575_pin_states |= 1 << pcf_pin;
    } else {
        pcf8575_pin_states &= ~(1 << pcf_pin);
    }
    
    // 立即更新PCF8575状态
    if (pcf8575_initialized) {
        pcf8575.write16(pcf8575_pin_states);
    }
}

/**
 * 读取 PCF8575 虚拟引脚电平
 * @param pin 虚拟引脚号
 * @return HIGH 或 LOW
 * 输出模式返回缓存电平；输入模式先写高电平上拉，读取后再恢复原输出状态。
 */
int PCF8575_digitalRead(const uint8_t pin) {
    if (!IS_PCF8575_VIRTUAL_PIN(pin) || !pcf8575_initialized) {
        return LOW;
    }
    
    // PCF8575输入读取：需要先设置为高电平（上拉），然后读取实际输入状态
    const uint8_t pcf_pin = VIRTUAL_TO_PCF8575_PIN(pin);
    
    // 检查引脚模式
    const bool is_output = pcf8575_pin_modes & (1 << pcf_pin);
    
    if (is_output) {
        // 输出模式：返回当前设置的输出状态
        return pcf8575_pin_states & (1 << pcf_pin) ? HIGH : LOW;
    }
    // 输入模式：读取实际输入状态
    // PCF8575作为输入时需要先写入高电平（上拉）
    uint16_t current_states = pcf8575_pin_states;
    current_states |= (1 << pcf_pin);  // 设置对应位为高电平
    pcf8575.write16(current_states);   // 写入上拉
        
    // 读取实际输入状态
    uint16_t input_states = pcf8575.read16();
        
    // 恢复原始输出状态（不影响其他引脚）
    pcf8575.write16(pcf8575_pin_states);
        
    return (input_states & (1 << pcf_pin)) ? HIGH : LOW;
}

/**
 * 用软件 PWM 模拟 PCF8575 输出
 * @param pin 虚拟引脚号
 * @param value PWM 占空比，0-255
 * 用约 1kHz 的软件计数比较模拟 PWM，精度有限。
 */
void PCF8575_analogWrite(const uint8_t pin, const uint8_t value) {
    if (!IS_PCF8575_VIRTUAL_PIN(pin)) {
        return;
    }
    
    uint8_t pcf_pin = VIRTUAL_TO_PCF8575_PIN(pin);
    
    // 缓存PWM值
    pcf8575_pwm_values[pcf_pin] = value;
    
    // 简单的PWM模拟：根据PWM值设置引脚状态
    // 注意：这是软件模拟的PWM，精度有限
    // 软件 PWM 上一次推进计数的 millis() 时间
    static uint32_t last_pwm_time = 0;
    // 软件 PWM 的 0..255 循环计数器，与占空比比较决定高低电平
    static uint8_t pwm_counter = 0;

    const uint32_t current_time = millis();
    if (current_time - last_pwm_time > 1) { // 约1kHz PWM频率
        last_pwm_time = current_time;
        pwm_counter++;
        
        if (value > pwm_counter) {
            PCF8575_digitalWrite(pin, HIGH);
        } else {
            PCF8575_digitalWrite(pin, LOW);
        }
    }
}

// Arduino标准函数重写，用于透明替换

// 该引脚在当前平台上是否真实存在。
// motorPins[8..15]（GP80-95）是预留槽位、根本不是合法 GPIO，
// 直接透传给 core 的话，ESP32 的 pinMode() 会为每个编号打一条
// "Invalid pin selected"，空板启动就能刷屏几十行噪声。
/**
 * 判断引脚是否为当前平台的真实 GPIO
 * @param pin 引脚号
 * @return true 表示合法 GPIO
 * 过滤 PIN_NONE 和平台不存在的编号，避免向 Arduino core 传入非法引脚。
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
 * 虚拟引脚转交 PCF8575_pinMode()，真实 GPIO 调用 Arduino pinMode()。
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
 * 虚拟引脚走 PCF8575，真实 GPIO 走 Arduino digitalWrite()。
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
 * 虚拟引脚走 PCF8575，真实 GPIO 走 Arduino digitalRead()。
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
 * 虚拟引脚使用软件 PWM，真实 GPIO 调用 Arduino analogWrite()。
 */
void analogWrite_PCF8575(const uint8_t pin, const uint8_t value) {
    if (IS_PCF8575_VIRTUAL_PIN(pin)) {
        PCF8575_analogWrite(pin, value);
    } else if (is_real_gpio(pin)) {
        analogWrite(pin, value);
    }
}
