#include "Motor.h"
#include "PCF8575_PinMap.h"
#include "platform.h"

const uint8_t DEFAULT_PWM_DUTY_CYCLE = 255; // 默认 PWM 占空比（0-255）

Motor::Motor(int pin1, int pin2)
    : pin1(pin1), pin2(pin2), pwmDutyCycle(DEFAULT_PWM_DUTY_CYCLE) {
    pinMode_PCF8575(pin1, OUTPUT);
    pinMode_PCF8575(pin2, OUTPUT);
}

Motor::Motor()
    : pin1(PIN_NONE), pin2(PIN_NONE), pwmDutyCycle(DEFAULT_PWM_DUTY_CYCLE) {}

void Motor::setPins(int newPin1, int newPin2) {
    // 重新绑定前先把旧输出拉低，避免切换配置时留下高电平。
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

void Motor::setDutyCycle(uint8_t dutyCycle)
{
    pwmDutyCycle = constrain(dutyCycle, 0, 255);
}

uint8_t Motor::getDutyCycle() const
{
    return pwmDutyCycle;
}

void Motor::forward()
{
    digitalWrite_PCF8575(pin2, LOW);
    digitalWrite_PCF8575(pin1, HIGH);
    isStop = false;
    state = "前进";
}

void Motor::backforward()
{
    digitalWrite_PCF8575(pin1, LOW);
    digitalWrite_PCF8575(pin2, HIGH);
    isStop = false;
    state = "后退";
}

void Motor::stop()
{
    digitalWrite_PCF8575(pin1, LOW);
    digitalWrite_PCF8575(pin2, LOW);
    isStop = true;
    state = "停止";
}

bool Motor::getStopState()
{
    return isStop;
}

String Motor::getState()
{
    return state;
}
