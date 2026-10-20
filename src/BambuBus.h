#pragma once

#include "main.h"
#ifdef __cplusplus
extern "C" {
#endif

#define BambuBus_uart uart0
#define BambuBus_uart_IRQ UART0_IRQ
#define BambuBus_pin_tx 0
#define BambuBus_pin_rx 1
#define BambuBus_pin_de 6

enum filament_status {
    offline, // 通道无耗材或未检测到
    online, // 检测到耗材且可正常使用
    NFC_waiting // 等待读取 NFC/耗材信息
};

enum filament_motion_state_set {
    need_pull_back, // 打印机要求退料
    need_send_out, // 打印机要求进料
    waiting, // 空闲等待
    act_send_mm, // 按指定行程进料
    act_pull_mm, // 按指定行程退料
    select_pos, // 旧版舵机选位命令（当前硬件保留兼容）
    release_all, // 释放所有通道动作
    cancel // 取消当前动作
};

enum package_type {
    BambuBus_package_filament_motion_short, // 短包进退料请求（Cxx）
    BambuBus_package_filament_motion_long, // 长包进退料请求（Dxx）
    BambuBus_package_online_detect, // AMS 在线检测/注册（F00）
    BambuBus_package_REQx6, // 0x06 握手请求
    BambuBus_package_NFC_detect, // NFC 耗材检测
    BambuBus_package_set_filament, // 短包设置耗材信息
    BambuBus_long_package_MC_online, // MC 在线长包
    BambuBus_longe_package_filament, // 读取耗材信息长包
    BambuBus_long_package_version, // 版本信息长包
    BambuBus_package_heartbeat, // 打印机心跳
    BambuBus_package_ETC, // 其他未分类报文
    BambuBus_package_ERROR, // CRC、长度或格式错误
    BambuBus_package_set_filament_type2, // 新增：0x05 0x218 长包设置耗材
    __BambuBus_package_packge_type_size // 枚举数量哨兵，不是实际报文类型
};

extern void BambuBus_init();

extern int BambuBus_run();

extern bool BambuBus_recent_activity(uint32_t quiet_ms);

// 0=尚未收到心跳，1=在线，2=超时离线；只统计 CRC 正确的打印机心跳。
extern int BambuBus_printer_status();

extern uint32_t BambuBus_rx_error_count();

extern uint32_t BambuBus_rx_dropped_count();

extern uint32_t BambuBus_last_valid_packet_age_ms();

extern bool BambuBus_read();

extern void BambuBus_set_need_to_save();

extern int get_now_filament_num();

extern int get_current_AMS_index();

extern void reset_filament_meters(int num);

extern void add_filament_meters(int num, float meters);

extern float get_filament_meters(int num);

extern void set_filament_online(int num, int count, bool if_online);

extern bool is_filament_online(int num);

extern uint8_t get_filament_color_A(int num);

extern uint8_t get_filament_color_R(int num);

extern uint8_t get_filament_color_G(int num);

extern uint8_t get_filament_color_B(int num);

extern int get_cmd_type();

extern unsigned char get_now_op_num();

extern void set_filament_motion(int num, filament_motion_state_set motion);

extern void send_uart(const unsigned char *data, uint16_t length);

filament_motion_state_set get_filament_motion(int num);

String get_now_filament_name(int num);

#ifdef BambuBus_use_forwarding_Serial
#define forwarding_Serial Serial4
extern void BambuBus_run_forward();
#endif

#ifdef __cplusplus
}
#endif
