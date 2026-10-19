#pragma once

#include <Arduino.h>

// 固定存储容量；实际启用通道数由 AMS_COUNT × 4 决定。
#define IO_CONFIG_MAX_CHANNELS 16

struct IoConfig {
    int32_t motor[IO_CONFIG_MAX_CHANNELS][2]; // [通道][0=进料引脚, 1=退料引脚]，PIN_NONE 表示未配置
    int32_t microswitch[IO_CONFIG_MAX_CHANNELS]; // 各通道微动开关引脚，PIN_NONE 表示无输入
};

// 生成当前平台的固件默认映射。
void io_config_set_defaults(IoConfig &config);

// 切换 I2C 总线状态时清空在新状态下不可用的引脚，返回清掉的引脚数。
int io_config_clear_invalid_pins(IoConfig &config, bool i2c_configured, int sda, int scl);

// 判断某个引脚是否可以出现在网页的选择列表中。
bool io_config_pin_available(int pin, bool allow_none = false);
bool io_config_i2c_validate(int sda, int scl, const IoConfig &config, int active_channels, String *error = nullptr);

// 返回网页/日志使用的可读名称（GPIOx 或 PCF8575 Px）。
String io_config_pin_label(int pin);

// 启动绑带脚等可用但需要用户注意的引脚。
bool io_config_pin_has_warning(int pin);

// 校验整组配置。error 可选，用于返回可读的拒绝原因。
bool io_config_validate(const IoConfig &config, int active_channels, String *error = nullptr);

// 生成网页需要的可选引脚 JSON 数组。
void io_config_append_pin_options(String &json, bool i2c_only = false);

// 总线接线只允许主控 GPIO；模式 1 不占用 DE。
bool io_config_bus_validate(int rx, int tx, int de, int mode, int sda, int scl,
                            const IoConfig &config, int active_channels, String *error = nullptr);
