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
// 第一项是 C3 默认项目；50～65 是 PCF8575 虚拟引脚编号。
static const ProjectPreset PROJECT_PRESETS[] = {
    {"esp32c3-4switch", "esp32c3(4色4微动)", "esp32c3", 1,
     {18, 19, PIN_NONE, RS485_MODE_RXTX, PIN_NONE, PIN_NONE}, 4,
     {{0, 1, 2}, {12, 13, 3}, {10, 6, 4}, {7, 8, 5}}},
    {"esp32c3-tuanzi", "esp32c3(团子 pcb 4色)", "esp32c3", 2,
     {10, 7, PIN_NONE, RS485_MODE_RXTX, 4, 5}, 4,
     {{50, 54, 60}, {51, 55, 61}, {52, 56, 62}, {53, 57, 63}}},
};

void project_preset_io(const ProjectPreset &preset, IoConfig &config);
bool project_preset_validate(const ProjectPreset &preset, String *error = nullptr);
const ProjectPreset *project_preset_find(const char *id);
const char *project_preset_match(const SettingsData &settings);
void project_preset_apply(const ProjectPreset &preset, SettingsData &settings);
