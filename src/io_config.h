#pragma once

#include <Arduino.h>

// 固定存储容量；实际启用通道数由 AMS_COUNT × 4 决定
#define IO_CONFIG_MAX_CHANNELS 16

struct IoConfig {
    int32_t motor[IO_CONFIG_MAX_CHANNELS][2]; // [通道][0=进料引脚, 1=退料引脚]，PIN_NONE 表示未配置
    int32_t microswitch[IO_CONFIG_MAX_CHANNELS]; // 各通道微动开关引脚，PIN_NONE 表示无输入
    int32_t switch_pin; // 五通总微动输入，PIN_NONE 表示不使用
    int32_t hall[IO_CONFIG_MAX_CHANNELS]; // 各通道 49E 模拟霍尔引脚，PIN_NONE 表示未配置
    int32_t clutch_enabled; // 0=独立电机，1=共享电机与电磁离合
    int32_t shared_motor[2]; // 共享电机的进料、退料引脚
    int32_t clutch[IO_CONFIG_MAX_CHANNELS]; // 各通道离合控制引脚
    int32_t clutch_active_level; // 离合闭合电平（LOW/HIGH）
    int32_t clutch_engage_ms; // 吸合后等待稳定的时长（毫秒）
    int32_t clutch_release_ms; // 释放后等待稳定的时长（毫秒）
};

constexpr int DEFAULT_CLUTCH_DELAY_MS = 100;
constexpr int MAX_CLUTCH_DELAY_MS = 2000;

void io_config_set_clutch_defaults(IoConfig &config);
bool io_config_clutch_valid(const IoConfig &config);
bool io_config_uses_pin(const IoConfig &config, int active_channels, int pin);

// 整套候选总线接线；校验项目预设时不借用或修改运行期设置
struct IoBusConfig {
    int rx, tx, de, mode, sda, scl;
};

int io_config_sanitize_pins(IoConfig &config, int active_channels, const IoBusConfig *bus);

// RX/TX 均已配置时启用有线总线；仅查询配置，不替代完整接线校验
bool io_config_bus_enabled(const IoBusConfig *bus = nullptr);

// 生成当前平台的固件默认映射
void io_config_set_defaults(IoConfig &config);

// 切换 I2C 总线状态时清空不可用的通道/总微动引脚，返回清掉的引脚数
int io_config_clear_invalid_pins(IoConfig &config, bool i2c_configured, int sda, int scl);

// 判断某个引脚是否可以出现在网页的选择列表中
bool io_config_pin_available(int pin, bool allow_none = false, const IoBusConfig *bus = nullptr);
bool io_config_hall_pin_available(int pin, bool allow_none = false, const IoBusConfig *bus = nullptr);
bool io_config_i2c_validate(int sda, int scl, const IoConfig &config, int active_channels, String *error = nullptr,
                            const IoBusConfig *bus = nullptr);

// 返回网页/日志使用的可读名称（IOx 或虚拟编号、扩展器序号及 Px）
String io_config_pin_label(int pin);

// 启动绑带脚等可用但需要用户注意的引脚
bool io_config_pin_has_warning(int pin);

// 校验整组配置error 可选，用于返回可读的拒绝原因
bool io_config_validate(const IoConfig &config, int active_channels, String *error = nullptr,
                        const IoBusConfig *bus = nullptr);

// 生成网页需要的可选引脚 JSON 数组
void io_config_append_pin_options(String &json, bool i2c_only = false);
void io_config_append_hall_pin_options(String &json);

// 有线总线只允许主控 GPIO；模式 1 不占用 DE，ESP32 双空 RX/TX 时使用局域网且不占用 DE
bool io_config_bus_validate(int rx, int tx, int de, int mode, int sda, int scl,
                            const IoConfig &config, int active_channels, String *error = nullptr);
