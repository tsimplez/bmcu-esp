#include "project_presets.h"
#include <cstring>

/**
 * 展开项目的完整通道接线
 * @param preset 项目预设
 * @param config 输出的通道配置
 * 清空全部未定义通道，防止切换项目后残留旧接线
 */
void project_preset_io(const ProjectPreset &preset, IoConfig &config) {
    for (int ch = 0; ch < IO_CONFIG_MAX_CHANNELS; ++ch) {
        config.motor[ch][0] = ch < preset.wired_channels ? preset.pins[ch][0] : PIN_NONE;
        config.motor[ch][1] = ch < preset.wired_channels ? preset.pins[ch][1] : PIN_NONE;
        config.microswitch[ch] = ch < preset.wired_channels ? preset.pins[ch][2] : PIN_NONE;
    }
}

/**
 * 校验项目与当前平台及整套接线是否兼容
 * @param preset 待检查的预设
 * @param error 可选的错误说明
 * @return true 表示可以应用
 * 使用候选总线检查全部冲突，不修改当前生效配置
 */
bool project_preset_validate(const ProjectPreset &preset, String *error) {
    if (strcmp(preset.target, hal_project_target()) != 0) {
        if (error) *error = "项目不适用于当前芯片";
        return false;
    }
    if (preset.ams < 1 || preset.ams > IO_CONFIG_MAX_CHANNELS / 4 ||
        preset.wired_channels < 0 || preset.wired_channels > preset.ams * 4) {
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
 * 保留其他参数，仅约束并发上限；I2C 只存 SDA/SCL，CRC 由存储接口重新计算
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
 * 比较所有通道含停用通道，模式 1 下忽略不使用的 DE 引脚
 */
const char *project_preset_match(const SettingsData &settings) {
    for (const auto &preset : PROJECT_PRESETS) {
        if (strcmp(preset.target, hal_project_target()) != 0 || settings.ams_count != preset.ams ||
            settings.rs485_mode != preset.bus.mode || settings.bus_rx != preset.bus.rx ||
            settings.bus_tx != preset.bus.tx || settings.pcf_sda != preset.bus.sda ||
            settings.pcf_scl != preset.bus.scl ||
            (preset.bus.mode == RS485_MODE_DE && settings.bus_de != preset.bus.de)) continue;
        IoConfig io;
        project_preset_io(preset, io);
        if (memcmp(&io, &settings.io_config, sizeof(io)) == 0) return preset.id;
    }
    return "";
}
