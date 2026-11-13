#pragma once

#include "platform.h"
#include "settings_storage.h"
#include <initializer_list>

class ProjectPresetPins {
public:
    using Channel = int[3];

    /**
     * 保存逐通道接线，并按列表条目数记录通道数量
     * @param channels 按进料、退料、微动排列的通道列表，空列表表示未接线
     * 使用固定存储复制列表；超出容量时保留原始数量，交由预设校验拒绝
     */
    constexpr ProjectPresetPins(std::initializer_list<Channel> channels = {}) : count_(channels.size()) {
        size_t ch = 0;
        for (const auto &channel: channels) {
            if (ch == IO_CONFIG_MAX_CHANNELS) break;
            for (int pin = 0; pin < 3; ++pin) values_[ch][pin] = channel[pin];
            ++ch;
        }
    }

    /**
     * 合并平台默认电机和微动数组，并从数组长度推导通道数量
     * @param motors 各通道的进料和退料引脚
     * @param switches 对应通道的微动引脚
     */
    template<size_t N>
    constexpr ProjectPresetPins(const int (&motors)[N][2], const int (&switches)[N]) : count_(N) {
        static_assert(N <= IO_CONFIG_MAX_CHANNELS, "项目通道数量超过存储容量");
        for (size_t ch = 0; ch < N; ++ch) {
            values_[ch][0] = motors[ch][0];
            values_[ch][1] = motors[ch][1];
            values_[ch][2] = switches[ch];
        }
    }

    /**
     * 获取显式列出的通道数量
     * @return 接线条目数，包含全部为 PIN_NONE 的占位通道
     */
    constexpr size_t size() const { return count_; }

    /**
     * 获取指定通道的接线
     * @param ch 存储容量内的通道下标，调用方应先检查是否已列出
     * @return 按进料、退料、微动排列的三个引脚
     */
    constexpr const int *operator[](size_t ch) const { return values_[ch]; }

private:
    size_t count_;
    Channel values_[IO_CONFIG_MAX_CHANNELS] = {};
};

struct ProjectPreset {
    const char *id;
    const char *name;
    const char *target;
    int ams;
    IoBusConfig bus;
    ProjectPresetPins pins; // 进料、退料、微动；通道数由条目数推导，未列出的通道统一清空
    int switch_pin = PIN_SWITCH; // 五通总微动默认引脚，未指定时采用平台默认值
    int hall[IO_CONFIG_MAX_CHANNELS] = {
        // 各通道霍尔默认不接；项目可按实际 ADC 接线单独指定
        PIN_NONE, PIN_NONE, PIN_NONE, PIN_NONE, PIN_NONE, PIN_NONE, PIN_NONE, PIN_NONE,
        PIN_NONE, PIN_NONE, PIN_NONE, PIN_NONE, PIN_NONE, PIN_NONE, PIN_NONE, PIN_NONE
    };
};

// 项目接线的唯一数据源：新增项目只需追加记录，ID 保持唯一且稳定
// pins 每项对应一个通道，最多 ams * 4 项；跳过中间通道时填 {PIN_NONE, PIN_NONE, PIN_NONE}
// 第一项是 C3 默认项目；PCF8575 每片使用 0～7、10～17 偏移，起始编号为 50、70、90、110
static constexpr ProjectPreset PROJECT_PRESETS[] = {
    {
        .id = "esp32c3-4switch", .name = "esp32c3(4色4微动)", .target = "esp32c3", .ams = 1,
        .bus = {.rx = 18, .tx = 19, .de = PIN_NONE, .mode = RS485_MODE_RXTX, .sda = PIN_NONE, .scl = PIN_NONE},
        .pins = {{0, 1, 2}, {12, 13, 3}, {10, 6, 4}, {7, 8, 5}}
    },
    {
        .id = "esp32c3-tuanzi", .name = "esp32c3(团子 pcb 8色)", .target = "esp32c3", .ams = 2,
        .bus = {.rx = 10, .tx = 7, .de = PIN_NONE, .mode = RS485_MODE_RXTX, .sda = 4, .scl = 5},
        .pins = {
            {55, 54, 50}, {56, 57, 52}, {60, 61, 67}, {63, 62, 65},
            {75, 74, 70}, {76, 77, 72}, {80, 81, 87}, {83, 82, 85}
        }
    },
    {
        .id = "top-ams", .name = "top-ams(pcb 8色 mqtt局域网方案)", .target = "esp32c3", .ams = 2,
        .bus = {
            .rx = PIN_NONE, .tx = PIN_NONE, .de = PIN_NONE, .mode = RS485_MODE_RXTX, .sda = PIN_NONE, .scl = PIN_NONE
        },
        .pins = {
            {2, 3, PIN_NONE}, {10, 6, PIN_NONE}, {5, 4, PIN_NONE}, {8, 9, PIN_NONE},
            {0, 1, PIN_NONE}, {20, 21, PIN_NONE}, {12, 13, PIN_NONE}, {18, 19, PIN_NONE}
        }
    },
};

void project_preset_io(const ProjectPreset &preset, IoConfig &config);

bool project_preset_validate(const ProjectPreset &preset, String *error = nullptr);

const ProjectPreset *project_preset_find(const char *id);

const char *project_preset_match(const SettingsData &settings);

void project_preset_apply(const ProjectPreset &preset, SettingsData &settings);
