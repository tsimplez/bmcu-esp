#pragma once
#include <Arduino.h>

// 默认 PWM 占空比（0-255），具体值在 Motor.cpp 中定义
extern const uint8_t DEFAULT_PWM_DUTY_CYCLE;

class Motor {
    int pin1;                    // 进料方向控制引脚（可为主控 GPIO 或 PCF8575 虚拟脚）
    int pin2;                    // 退料方向控制引脚（可为主控 GPIO 或 PCF8575 虚拟脚）
    uint8_t pwmDutyCycle = 0;    // 当前 PWM 占空比（0-255），0 表示输出关闭
    bool isStop = true;          // 当前是否处于停止状态
    String state = "停止";       // 供状态接口显示的中文运行状态

public:
    Motor();

    Motor(int pin1, int pin2);

    void setPins(int pin1, int pin2);

    void setDutyCycle(uint8_t dutyCycle);

    uint8_t getDutyCycle() const;

    void forward();

    void backward();

    void stop();

    bool getStopState() const;

    String getState();
};
