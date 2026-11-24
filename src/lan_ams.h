#pragma once

#include <stdint.h>

struct LanAmsStatus {
    bool connected, paired, busy, fault;
    int current_channel, target_channel;
    uint32_t age_ms;
    const char *state;
    const char *error;
};

bool lan_ams_enabled();
void lan_ams_run();
bool lan_ams_busy();
bool lan_ams_motor_allowed(int channel);
int lan_ams_active_channel();
LanAmsStatus lan_ams_status();
bool lan_ams_request_channel(int channel);
/**
 * 提交局域网打印暂停命令
 * @return 当前打印通道有效且命令成功入队时返回 true
 */
bool lan_ams_request_pause();
void lan_ams_request_cancel();
void lan_ams_cancel();
