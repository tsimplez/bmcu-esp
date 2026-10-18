#pragma once

#include "main.h"
#include "BambuBus.h"
#ifdef __cplusplus
extern "C"
{
#endif

    // #define BMCU_uart uart1
    // #define BMCU_uart_IRQ UART1_IRQ
    // #define BMCU_pin_tx 4
    // #define BMCU_pin_rx 5

#define BMCU_AS5600_SDA 4
#define BMCU_AS5600_SCL 5

// AS5600相关常量
#define AS5600_RAW_TO_DEGREES     360.0 / 4096
#define AS5600_DEGREES_TO_RAW     4096 / 360.0
#define AS5600_PI 3.14159265358979323846
#define AS5600_MM_PER_CNT         -(AS5600_PI * 7.5f) / 4096.0f

// AS5600磁场状态
enum AS5600_magnet_stu {
    AS5600_low     = 1,
    AS5600_high    = 2,
    AS5600_offline = -1,
    AS5600_normal  = 0
};

// AS5600模拟函数
extern void AS5600_init();
extern float AS5600_get_distance_E();
extern uint16_t AS5600_get_raw_angle(uint8_t channel);
extern AS5600_magnet_stu AS5600_get_magnet_stu(uint8_t channel);
extern bool AS5600_is_online(uint8_t channel);
extern float AS5600_get_speed(uint8_t channel);

// AS5600模拟变量
extern int16_t as5600_current_angle[4];
extern int16_t as5600_last_angle[4];
extern uint32_t as5600_last_time[4];
extern float as5600_speed[4];

    void set_color(u_int8_t r, u_int8_t g, u_int8_t b, u_int8_t a);
    void rgb_set_breath(int16_t time, uint16_t count);
    void selectOnePos(char num);
    void releaseAllPos();
    void set_LED_state(char led, char state);
    void debug_info(bool force);

    extern void BMCU_init();
    extern void BMCU_run();
    extern void BMCU_motion();
    void check_microswitch_status();

    enum MotorSource { MOTOR_IDLE, MOTOR_PRINTER, MOTOR_MANUAL, MOTOR_AUTOLOAD };
    struct MotorStatus {
        int direction;         // 当前方向：1=进料，-1=退料，0=停止
        MotorSource source;    // 当前动作来源：打印机、手动点动或微动辅助进料
        int32_t remaining_ms; // -1 表示结束时间由打印机决定
        bool blocked;         // 等待打印机结束当前动作，防止停止后重复启动
        int queue_position;    // 0=未排队，1=下一个执行
        int queued_direction;  // 排队任务的点动方向：1=进料，-1=退料，0=无排队任务
    };
    bool motorCmd(int motorNum, int cmd, MotorSource source = MOTOR_PRINTER, uint32_t duration_ms = 0);
    MotorStatus BMCU_motor_status(int motorNum);
    int BMCU_running_count();
    void BMCU_stop_motor(int motorNum);
    void BMCU_printer_idle(int motorNum);
    bool BMCU_printer_bus_fault();
    bool BMCU_can_maintain();
    bool BMCU_manual_pulse(int motorNum, int cmd, uint32_t duration_ms);
    // 该通道的进料/退料引脚是否都已配置（任一为「无」表示没接电机，不能启动）
    bool BMCU_channel_motor_ready(int motorNum);
    bool BMCU_manual_active();
    int BMCU_motor_count();
    int BMCU_max_ams_count();
    int BMCU_channel_filament_state(int motorNum);
    void BMCU_apply_channel_modes();
    void setMainMotorDutyCycle(uint8_t dutyCycle);
    void init_io_pins();
    void BMCU_apply_io_config();

#ifdef __cplusplus
}
#endif
