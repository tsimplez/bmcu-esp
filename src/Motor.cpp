#include "Motor.h"
#include "PCF8575_PinMap.h"
#include "platform.h"

#define DEFAULT_PWM_DUTY_CYCLE 255 // 默认 PWM 占空比（0-255）

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
 * 保存两个方向引脚并把它们配置为输出，占空比使用默认值
 */
Motor::Motor(const int pin1, const int pin2)
    : pin1(pin1), pin2(pin2), pwmDutyCycle(DEFAULT_PWM_DUTY_CYCLE) {
    pinMode_PCF8575(pin1, OUTPUT);
    pinMode_PCF8575(pin2, OUTPUT);
}

/**
 * 重新绑定电机方向引脚
 * @param newPin1 新的进料方向引脚
 * @param newPin2 新的退料方向引脚
 * 先停止电机并把旧引脚拉低，再配置新引脚为输出，避免切换接线时误转
 */
void Motor::setPins(const int newPin1, const int newPin2) {
    // 重新绑定前先把旧输出拉低，避免切换配置时留下高电平
    stop();
    pin1 = newPin1;
    pin2 = newPin2;
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
 * 将 pin2 拉低、pin1 拉高，并把状态更新为“前进”
 */
void Motor::forward() {
    digitalWrite_PCF8575(pin2, LOW);
    digitalWrite_PCF8575(pin1, HIGH);
    isStop = false;
    state = "前进";
}

/**
 * 驱动电机反转（退料）
 * 将 pin1 拉低、pin2 拉高，并把状态更新为“后退”
 */
void Motor::backward() {
    digitalWrite_PCF8575(pin1, LOW);
    digitalWrite_PCF8575(pin2, HIGH);
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
