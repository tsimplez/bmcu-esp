#include "Motor.h"
#include "PCF8575_PinMap.h"
#include "platform.h"

#define DEFAULT_PWM_DUTY_CYCLE 255 // 默认 PWM 占空比（0-255）

/**
 * 检查两个电机方向引脚的扩展芯片状态
 * @param forwardPin 进料引脚
 * @param backwardPin 退料引脚
 * @return 两方向均已配置且所属扩展芯片在线时为 true
 * 查询缓存状态，在关闭相反方向及启动后及时拒绝本轮写入产生的传输故障
 */
static bool motor_pins_ready(const int forwardPin, const int backwardPin) {
    if (forwardPin == PIN_NONE || backwardPin == PIN_NONE) return false;
    return (!IS_PCF8575_VIRTUAL_PIN(forwardPin) || PCF8575_pin_is_online(forwardPin)) &&
           (!IS_PCF8575_VIRTUAL_PIN(backwardPin) || PCF8575_pin_is_online(backwardPin));
}

/**
 * 默认电机构造函数
 * 创建一个尚未绑定引脚的电机对象，后续可通过 setPins() 绑定
 */
Motor::Motor()
    : pin1(PIN_NONE), pin2(PIN_NONE), pwmDutyCycle(DEFAULT_PWM_DUTY_CYCLE) {
}

/**
 * 带引脚参数的电机构造函数
 * @param pin1 进料方向控制引脚
 * @param pin2 退料方向控制引脚
 * 保存两个方向引脚，登记扩展器故障与恢复时的关闭电平，先预置关闭电平再配置为输出，占空比使用默认值
 */
Motor::Motor(const int pin1, const int pin2)
    : pin1(pin1), pin2(pin2), pwmDutyCycle(DEFAULT_PWM_DUTY_CYCLE) {
    PCF8575_set_idle_level(pin1, LOW);
    PCF8575_set_idle_level(pin2, LOW);
    digitalWrite_PCF8575(pin1, LOW);
    digitalWrite_PCF8575(pin2, LOW);
    pinMode_PCF8575(pin1, OUTPUT);
    pinMode_PCF8575(pin2, OUTPUT);
    digitalWrite_PCF8575(pin1, LOW);
    digitalWrite_PCF8575(pin2, LOW);
}

/**
 * 重新绑定电机方向引脚
 * @param newPin1 新的进料方向引脚
 * @param newPin2 新的退料方向引脚
 * 先停止电机并把旧引脚拉低，再登记新引脚的故障安全电平、预置关闭电平后配置为输出，避免切换接线时误转
 */
void Motor::setPins(const int newPin1, const int newPin2) {
    // 重新绑定前先把旧输出拉低，避免切换配置时留下高电平
    stop();
    pin1 = newPin1;
    pin2 = newPin2;
    PCF8575_set_idle_level(pin1, LOW);
    PCF8575_set_idle_level(pin2, LOW);
    digitalWrite_PCF8575(pin1, LOW);
    digitalWrite_PCF8575(pin2, LOW);
    pinMode_PCF8575(pin1, OUTPUT);
    pinMode_PCF8575(pin2, OUTPUT);
    digitalWrite_PCF8575(pin1, LOW);
    digitalWrite_PCF8575(pin2, LOW);
    isStop = true;
    state = "停止";
}

/**
 * 设置电机占空比
 * @param dutyCycle 占空比，范围 0-255
 * 把占空比限制在 0-255 范围内并保存，供上层读取或后续扩展 PWM 使用
 */
void Motor::setDutyCycle(const uint8_t dutyCycle) {
    pwmDutyCycle = constrain(dutyCycle, 0, 255);
}

/**
 * 读取当前占空比
 * @return 当前保存的 0-255 占空比
 * 返回电机对象中记录的占空比
 */
uint8_t Motor::getDutyCycle() const {
    return pwmDutyCycle;
}

/**
 * 驱动电机正转（进料）
 * 将 pin2 拉低后检查扩展芯片，再把 pin1 拉高并复查；写入故障时关闭输出并保持停止，成功后状态更新为“前进”
 */
void Motor::forward() {
    digitalWrite_PCF8575(pin2, LOW);
    if (!motor_pins_ready(pin1, pin2)) {
        stop();
        return;
    }
    digitalWrite_PCF8575(pin1, HIGH);
    if (!motor_pins_ready(pin1, pin2)) {
        stop();
        return;
    }
    isStop = false;
    state = "前进";
}

/**
 * 驱动电机反转（退料）
 * 将 pin1 拉低后检查扩展芯片，再把 pin2 拉高并复查；写入故障时关闭输出并保持停止，成功后状态更新为“后退”
 */
void Motor::backward() {
    digitalWrite_PCF8575(pin1, LOW);
    if (!motor_pins_ready(pin1, pin2)) {
        stop();
        return;
    }
    digitalWrite_PCF8575(pin2, HIGH);
    if (!motor_pins_ready(pin1, pin2)) {
        stop();
        return;
    }
    isStop = false;
    state = "后退";
}

/**
 * 停止电机
 * 将两个方向引脚都拉低，并更新停止标志和状态文本
 */
void Motor::stop() {
    digitalWrite_PCF8575(pin1, LOW);
    digitalWrite_PCF8575(pin2, LOW);
    isStop = true;
    state = "停止";
}

/**
 * 查询电机是否停止
 * @return true 表示已停止，false 表示正在转动
 */
bool Motor::getStopState() const {
    return isStop;
}

/**
 * 获取电机状态文本
 * @return 当前状态字符串（前进/后退/停止）
 */
String Motor::getState() {
    return state;
}
