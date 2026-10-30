#pragma once

#include "platform.h"
#include "settings_storage.h"

struct ProjectPreset {
    const char *id;
    const char *name;
    const char *target;
    int ams;
    IoBusConfig bus;
    int wired_channels;
    int pins[IO_CONFIG_MAX_CHANNELS][3]; // 进料、退料、微动；未列出的通道统一清空
};

// 项目接线的唯一数据源：新增项目只需追加记录，ID 保持唯一且稳定。
// 第一项是 C3 默认项目；PCF8575 每片使用 0～7、10～17 偏移，起始编号为 50、70、90、110。
static constexpr ProjectPreset PROJECT_PRESETS[] = {
    {
        .id = "esp32c3-4switch", .name = "esp32c3(4色4微动)", .target = "esp32c3", .ams = 1,
        .bus = {.rx = 18, .tx = 19, .de = PIN_NONE, .mode = RS485_MODE_RXTX, .sda = PIN_NONE, .scl = PIN_NONE},
        .wired_channels = 4,
        .pins = {{0, 1, 2}, {12, 13, 3}, {10, 6, 4}, {7, 8, 5}}
    },
    {
        .id = "esp32c3-tuanzi", .name = "esp32c3(团子 pcb 8色)", .target = "esp32c3", .ams = 2,
        .bus = {.rx = 10, .tx = 7, .de = PIN_NONE, .mode = RS485_MODE_RXTX, .sda = 4, .scl = 5},
        .wired_channels = 8,
        .pins = {
            {54, 55, 52}, {56, 57, 50}, {62, 63, 67}, {60, 61, 65},
            {74, 75, 72}, {76, 77, 70}, {82, 83, 87}, {80, 81, 85}
        }
    },
};

void project_preset_io(const ProjectPreset &preset, IoConfig &config);

bool project_preset_validate(const ProjectPreset &preset, String *error = nullptr);

const ProjectPreset *project_preset_find(const char *id);

const char *project_preset_match(const SettingsData &settings);

void project_preset_apply(const ProjectPreset &preset, SettingsData &settings);
