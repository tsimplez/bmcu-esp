#include "project_presets.h"
#include <cstring>
#include <cstddef>

/**
 * 展开项目的完整通道接线
 * @param preset 项目预设
 * @param config 输出的通道配置
 * 按 pins 条目数展开，清空全部未列出的通道，并替换五通总微动和霍尔输入，恢复独立电机模式及空的离合接线
 */
void project_preset_io(const ProjectPreset &preset, IoConfig &config) {
    io_config_set_clutch_defaults(config);
    config.switch_pin = preset.switch_pin;
    for (size_t ch = 0; ch < IO_CONFIG_MAX_CHANNELS; ++ch) {
        config.motor[ch][0] = ch < preset.pins.size() ? preset.pins[ch][0] : PIN_NONE;
        config.motor[ch][1] = ch < preset.pins.size() ? preset.pins[ch][1] : PIN_NONE;
        config.microswitch[ch] = ch < preset.pins.size() ? preset.pins[ch][2] : PIN_NONE;
        config.hall[ch] = ch < preset.pins.size() ? preset.hall[ch] : PIN_NONE;
    }
}

/**
 * 校验项目与当前平台及整套接线是否兼容
 * @param preset 待检查的预设
 * @param error 可选的错误说明
 * @return true 表示可以应用
 * pins 条目数不得超过 AMS 通道数；使用候选总线检查全部冲突，不修改当前生效配置
 */
bool project_preset_validate(const ProjectPreset &preset, String *error) {
    if (strcmp(preset.target, hal_project_target()) != 0) {
        if (error) *error = "项目不适用于当前芯片";
        return false;
    }
    if (preset.ams < 1 || preset.ams > IO_CONFIG_MAX_CHANNELS / 4 ||
        preset.pins.size() > static_cast<size_t>(preset.ams * 4)) {
        if (error) *error = "项目的 AMS 数量或通道配置无效";
        return false;
    }
    const auto &bus = preset.bus;
    if ((bus.sda == PIN_NONE) != (bus.scl == PIN_NONE)) {
        if (error) *error = "项目的 SDA/SCL 必须同时配置或同时留空";
        return false;
    }
    IoConfig io;
    project_preset_io(preset, io);
    return io_config_bus_validate(bus.rx, bus.tx, bus.de, bus.mode, bus.sda, bus.scl,
                                  io, preset.ams * 4, error) &&
        (bus.sda == PIN_NONE || io_config_i2c_validate(bus.sda, bus.scl, io, preset.ams * 4, error, &bus)) &&
        io_config_validate(io, preset.ams * 4, error, &bus);
}

/**
 * 按稳定 ID 查找项目
 * @param id 请求中的项目 ID
 * @return 对应预设；不存在时返回 nullptr
 */
const ProjectPreset *project_preset_find(const char *id) {
    if (id) for (const auto &preset : PROJECT_PRESETS)
        if (strcmp(id, preset.id) == 0) return &preset;
    return nullptr;
}

/**
 * 把预设接线写入候选设置
 * @param preset 已通过校验的项目
 * @param settings 由当前快照创建的候选设置
 * 替换总线及通道接线，恢复独立电机模式与离合默认参数，保留其他时长参数并约束并发上限；CRC 由存储接口重新计算
 */
void project_preset_apply(const ProjectPreset &preset, SettingsData &settings) {
    settings.ams_count = preset.ams;
    settings.rs485_mode = preset.bus.mode;
    settings.bus_rx = preset.bus.rx;
    settings.bus_tx = preset.bus.tx;
    settings.bus_de = preset.bus.de;
    settings.pcf_sda = preset.bus.sda;
    settings.pcf_scl = preset.bus.scl;
    settings.motor_limit = min(static_cast<int>(settings.motor_limit), preset.ams * 4);
    project_preset_io(preset, settings.io_config);
}

/**
 * 根据实际接线识别当前项目
 * @param settings 当前设置快照
 * @return 匹配的项目 ID，空字符串表示自定义
 * 比较独立电机模式的总微动及所有通道接线，含停用通道；忽略未启用的离合配置和模式 1 下的 DE 引脚
 */
const char *project_preset_match(const SettingsData &settings) {
    if (settings.io_config.clutch_enabled) return "";
    for (const auto &preset : PROJECT_PRESETS) {
        if (strcmp(preset.target, hal_project_target()) != 0 || settings.ams_count != preset.ams ||
            settings.rs485_mode != preset.bus.mode || settings.bus_rx != preset.bus.rx ||
            settings.bus_tx != preset.bus.tx || settings.pcf_sda != preset.bus.sda ||
            settings.pcf_scl != preset.bus.scl ||
            (preset.bus.mode == RS485_MODE_DE && settings.bus_de != preset.bus.de)) continue;
        IoConfig io;
        project_preset_io(preset, io);
        if (memcmp(&io, &settings.io_config, offsetof(IoConfig, clutch_enabled)) == 0) return preset.id;
    }
    return "";
}
