#pragma once

#include <Arduino.h>

constexpr uint32_t HALL_SAMPLE_INTERVAL_MS = 50;
constexpr uint32_t HALL_HISTORY_INTERVAL_MS = 200;
constexpr uint16_t HALL_HISTORY_CAPACITY = 300;
constexpr uint16_t HALL_INVALID_VALUE = UINT16_MAX;

struct HallHistoryPoint {
    uint16_t average;
    uint16_t minimum;
    uint16_t maximum;
};

struct HallHistoryInfo {
    uint32_t epoch;
    uint32_t cursor;
    uint32_t now_ms;
    uint32_t end_ms;
    uint16_t count;
    uint8_t channels;
};

struct HallChannelStatus {
    int pin;
    bool configured;
    bool valid;
    bool statistics_valid;
    uint16_t raw;
    uint16_t minimum;
    uint16_t maximum;
    uint16_t average;
    uint16_t average_1s;
    uint16_t average_5s;
    uint32_t age_ms;
};

struct HallDetectionStatus {
    int channel;                    // 唯一检测通道；-1 表示没有检测目标
    const char *state;              // inactive / waiting / monitoring / timeout / sensor_error
    uint32_t idle_ms;
    uint32_t timeout_ms;
};

struct HallDetectionEvent {
    uint32_t sequence;              // 本次启动内的事件序号；0 表示尚无事件
    int channel;
    const char *type;               // timeout / recovered / sensor_error / sensor_restored
};

HallDetectionStatus hall_sensor_detection_status();

HallDetectionEvent hall_sensor_last_event();

void hall_sensor_init();

void hall_sensor_sync_config();

void hall_sensor_run();

HallHistoryInfo hall_sensor_history_info();

HallChannelStatus hall_sensor_status(int channel);

bool hall_sensor_history_point(int channel, uint32_t cursor, HallHistoryPoint &point);
