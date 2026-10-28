#include "settings_storage.h"
#include "platform.h"
#include "log_bus.h"
#include "i2c_bus.h"

// ============================
// 运行期设置变量（初值为默认值，启动时由 settings_storage_init() 覆盖）
// ============================

// 各通道退料动作超时时间，单位秒；打印机请求退料时用作电机限时停止保护
int PULLBACK_TIMEOUT[16] = {7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7};
// 各通道工作模式：0=离线，1=在线，2=自动（按微动开关触发辅助进料）
int CHANNEL_MODE[16] = {1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1};
// 启用的 AMS 数量（1-4），每个 AMS 固定对应 4 个耗材通道
int AMS_COUNT = DEFAULT_AMS_COUNT;
// Web 手动进退料按钮的默认点动时长，单位毫秒
int MANUAL_PULSE_MS = DEFAULT_MANUAL_PULSE_MS;
// 微动由无料变有料后的辅助进料时长，单位毫秒；0 表示关闭
int AUTO_LOAD_MS = DEFAULT_AUTO_LOAD_MS;
// 允许同时运行的电机数量上限，运行时由设置加载值限制到 active_channel_count()
int MOTOR_LIMIT = 1;
// BambuBus 的 RS485 收发模式：RS485_MODE_RXTX 或 RS485_MODE_DE
int RS485_MODE = DEFAULT_RS485_MODE;
// 是否把 BambuBus 原始收发报文写入总线日志（0=关，1=开）
int BUS_LOG_ENABLE = DEFAULT_BUS_LOG;
// BambuBus 自定义序列号；空串表示按芯片 ID 自动生成
char CUSTOM_SERIAL[CUSTOM_SERIAL_LENGTH + 1] = {};
// 返回给打印机的 AMS 版本，网页按 A.B.C.D 显示，总线载荷按 D.C.B.A 写入
int AMS_VERSION[4] = {DEFAULT_AMS_VERSION_MAJOR, DEFAULT_AMS_VERSION_MINOR,
                      DEFAULT_AMS_VERSION_PATCH, DEFAULT_AMS_VERSION_BUILD};
// OTA 固件发布目录，设备会读取该目录下的 manifest.json
char OTA_URL[OTA_URL_MAX] = DEFAULT_OTA_URL;
// 打开网页时是否自动检查 OTA 更新（0=关，1=开）
int OTA_NOTIFY = DEFAULT_OTA_NOTIFY;
// 各通道电机引脚和微动开关引脚的当前映射
IoConfig IO_CONFIG = {};
// BambuBus 串口 RX/TX 和 RS485 DE 引脚；启动时从设置加载
int BUS_RX = PIN_BUS_RX, BUS_TX = PIN_BUS_TX, BUS_DE = PIN_BUS_DE;

/**
 * 恢复 BambuBus 引脚默认值
 * 把 RX、TX、DE 三个引脚恢复为当前平台的板级默认映射
 */
static void bus_defaults() {
    BUS_RX = PIN_BUS_RX; BUS_TX = PIN_BUS_TX; BUS_DE = PIN_BUS_DE;
}

// I2C 总线 SDA 引脚；默认未配置，需在网页中同时选择 SDA/SCL 后重启启用
int PCF_SDA = PIN_NONE;
// I2C 总线 SCL 引脚；默认未配置，需在网页中同时选择 SDA/SCL 后重启启用
int PCF_SCL = PIN_NONE;

/**
 * 恢复 I2C 引脚默认值
 * 默认不占用任何引脚；用户需在网页中同时选择 SDA、SCL 后才启用总线
 */
static void pcf_defaults() {
    PCF_SDA = PIN_NONE;
    PCF_SCL = PIN_NONE;
}

// 当前设置记录使用 CRC32，计算时把 checksum 字段视为 0，避免校验值自引用
/**
 * 计算设置记录 CRC32
 * @param data 设置数据结构
 * @return 计算得到的 32 位 CRC
 * 计算前把 checksum 字段清零，避免校验值自引用
 */
static uint32_t calculate_crc32(const SettingsData* data) {
    SettingsData copy = *data;
    copy.checksum = 0;
    const auto *bytes = reinterpret_cast<const uint8_t *>(&copy);
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < sizeof(copy); i++) {
        crc ^= bytes[i];
        for (int bit = 0; bit < 8; bit++)
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

/**
 * 校验自定义序列号格式
 * @param serial 待校验的字符串
 * @return true 表示格式合法
 * 允许空串，或恰好 12 位大写十六进制字符（0-9、A-F）的序列号后缀
 */
bool settings_custom_serial_valid(const char *serial) {
    if (!serial) return false;
    size_t length = 0;
    while (length <= CUSTOM_SERIAL_LENGTH && serial[length]) length++;
    if (length == 0) return true;
    if (length != CUSTOM_SERIAL_LENGTH) return false;
    for (size_t i = 0; i < length; i++) {
        const char c = serial[i];
        if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'F'))) return false;
    }
    return true;
}

/**
 * 解析 A.B.C.D 格式的 AMS 版本
 * @param text 待解析文本，例如 "1.0.6.87"
 * @param out 输出四个版本分量
 * @return true 表示格式正确且每段均在 0..255
 * 必须正好四段十进制数字；仅在完整解析成功后写入 out
 */
bool settings_parse_ams_version(const char *text, int out[4]) {
    if (!text || !out) return false;
    int parsed[4] = {};
    int segment = 0;
    int value = 0;
    bool has_digit = false;
    for (const char *p = text;; ++p) {
        const char c = *p;
        if (c >= '0' && c <= '9') {
            has_digit = true;
            value = value * 10 + (c - '0');
            if (value > 255) return false;
            continue;
        }
        if (c != '.' && c != '\0') return false;
        if (!has_digit || segment >= 4) return false;
        parsed[segment++] = value;
        if (c == '\0') break;
        if (segment >= 4) return false;
        value = 0;
        has_digit = false;
    }
    if (segment != 4) return false;
    memcpy(out, parsed, sizeof(parsed));
    return true;
}

// 从Flash读取设置数据
/**
 * 从持久化存储读设置记录
 * @param data 输出结构
 * 调用平台存储接口读取整块 SettingsData
 */
static void read_settings_from_flash(SettingsData* data) {
    hal_storage_read(HAL_REGION_SETTINGS, data, sizeof(SettingsData));
}

// 写入设置到Flash
/**
 * 把设置记录写入持久化存储
 * @param data 待写入结构
 * @return 是否写入成功
 * 调用平台存储接口写入整块 SettingsData
 */
static bool write_settings_to_flash(const SettingsData* data) {
    return hal_storage_write(HAL_REGION_SETTINGS, data, sizeof(SettingsData));
}

/**
 * 校验并应用设置到运行期变量
 * @param data 从存储或快照得到的设置
 * 逐项检查范围后更新全局配置；接线配置无效时恢复平台默认映射并记录日志
 */
static void apply_settings_values(const SettingsData& data) {
    for (int i = 0; i < 16; i++) {
        PULLBACK_TIMEOUT[i] = data.pullback_timeout[i] >= 3 && data.pullback_timeout[i] <= 30
                                  ? data.pullback_timeout[i] : DEFAULT_PULLBACK_TIMEOUT;
        CHANNEL_MODE[i] = data.channel_mode[i] >= 0 && data.channel_mode[i] <= 2
                              ? data.channel_mode[i] : CHANNEL_MODE_AUTO;
    }

    AMS_COUNT = data.ams_count >= 1 && data.ams_count <= IO_CONFIG_MAX_CHANNELS / 4
                    ? data.ams_count : DEFAULT_AMS_COUNT;
    MANUAL_PULSE_MS = data.manual_pulse_ms > 0 && data.manual_pulse_ms < MAX_PULSE_MS
                          ? data.manual_pulse_ms : DEFAULT_MANUAL_PULSE_MS;
    AUTO_LOAD_MS = data.auto_load_ms >= 0 && data.auto_load_ms < MAX_PULSE_MS
                       ? data.auto_load_ms : DEFAULT_AUTO_LOAD_MS;
    RS485_MODE = data.rs485_mode == RS485_MODE_RXTX || data.rs485_mode == RS485_MODE_DE
                     ? data.rs485_mode : DEFAULT_RS485_MODE;
    BUS_LOG_ENABLE = data.bus_log_enable == 0 || data.bus_log_enable == 1
                         ? data.bus_log_enable : DEFAULT_BUS_LOG;

    memcpy(CUSTOM_SERIAL, data.custom_serial, sizeof(data.custom_serial));
    CUSTOM_SERIAL[CUSTOM_SERIAL_LENGTH] = 0;
    if (!settings_custom_serial_valid(CUSTOM_SERIAL)) CUSTOM_SERIAL[0] = 0;

    bool version_valid = true;
    for (int i = 0; i < 4; ++i)
        if (data.ams_version[i] < 0 || data.ams_version[i] > 255) version_valid = false;
    if (version_valid) {
        memcpy(AMS_VERSION, data.ams_version, sizeof(AMS_VERSION));
    } else {
        AMS_VERSION[0] = DEFAULT_AMS_VERSION_MAJOR;
        AMS_VERSION[1] = DEFAULT_AMS_VERSION_MINOR;
        AMS_VERSION[2] = DEFAULT_AMS_VERSION_PATCH;
        AMS_VERSION[3] = DEFAULT_AMS_VERSION_BUILD;
    }

    OTA_NOTIFY = data.ota_notify == 0 || data.ota_notify == 1 ? data.ota_notify : DEFAULT_OTA_NOTIFY;
    memcpy(OTA_URL, data.ota_url, OTA_URL_MAX);
    OTA_URL[OTA_URL_MAX - 1] = 0;  // 强行截断，防止 NVS 里出现没有结束符的脏数据
    MOTOR_LIMIT = data.motor_limit >= 1 && data.motor_limit <= IO_CONFIG_MAX_CHANNELS
                      ? min(static_cast<int>(data.motor_limit), active_channel_count()) : 1;

    // 先加载整组候选接线，再交叉校验，避免用旧总线引脚误判新配置
    const IoConfig loaded = data.io_config;
    BUS_RX = data.bus_rx; BUS_TX = data.bus_tx; BUS_DE = data.bus_de;
    // I2C 总线由 SDA/SCL 是否同时配置决定，旧版 pcf_enable 字段只用于兼容旧记录
    pcf_defaults();
    PCF_SDA = data.pcf_sda; PCF_SCL = data.pcf_scl;
    // 旧版记录在关闭 PCF8575 时仍保存板级默认 4/5，迁移为“未配置”，
    // 避免升级后被自动启用并和原有微动引脚冲突
    if (data.pcf_enable == 0 && PCF_SDA == PIN_I2C_SDA && PCF_SCL == PIN_I2C_SCL) {
        PCF_SDA = PIN_NONE;
        PCF_SCL = PIN_NONE;
    }
    // 只配置单个引脚属于非法状态，按未配置处理
    if ((PCF_SDA == PIN_NONE) != (PCF_SCL == PIN_NONE)) {
        PCF_SDA = PIN_NONE;
        PCF_SCL = PIN_NONE;
    }
    // 两脚相同、越界或旧版脏数据都按“总线未配置”处理，避免网页锁死在无效组合
    if (!i2c_bus_configured()) {
        PCF_SDA = PIN_NONE;
        PCF_SCL = PIN_NONE;
    }
    String io_error;
    bool wiring_valid = io_config_bus_validate(BUS_RX, BUS_TX, BUS_DE, RS485_MODE,
        PCF_SDA, PCF_SCL, loaded, active_channel_count(), &io_error);
    if (i2c_bus_configured())
        wiring_valid = wiring_valid && io_config_i2c_validate(PCF_SDA, PCF_SCL, loaded, active_channel_count(), &io_error);
    wiring_valid = wiring_valid && io_config_validate(loaded, active_channel_count(), &io_error);
    if (wiring_valid) {
        IO_CONFIG = loaded;
    } else {
        AMS_COUNT = DEFAULT_AMS_COUNT;
        MOTOR_LIMIT = min(MOTOR_LIMIT, active_channel_count());
        RS485_MODE = DEFAULT_RS485_MODE;
        bus_defaults();
        pcf_defaults();
        io_config_set_defaults(IO_CONFIG);
        LOG_MAIN("[settings] 接线配置无效，已恢复平台默认映射：%s", io_error.c_str());
    }
}

/**
 * 初始化设置存储
 * 读取设置并校验魔数与 CRC32；上一版记录仅迁移旧默认 AMS 版本，保留自定义参数
 * 迁移仍执行写入及回读校验，失败恢复旧运行值；无效或空片时恢复默认值并重新写入
 */
void settings_storage_init() {
    SettingsData data{};

    read_settings_from_flash(&data);

    if ((data.magic == SETTINGS_MAGIC_NUMBER || data.magic == SETTINGS_MAGIC_PREVIOUS) &&
        calculate_crc32(&data) == data.checksum) {
        apply_settings_values(data);
        if (data.magic == SETTINGS_MAGIC_PREVIOUS) {
            if (data.ams_version[0] == 90 && data.ams_version[1] == 0 &&
                data.ams_version[2] == 0 && data.ams_version[3] == 0) {
                AMS_VERSION[0] = DEFAULT_AMS_VERSION_MAJOR;
                AMS_VERSION[1] = DEFAULT_AMS_VERSION_MINOR;
                AMS_VERSION[2] = DEFAULT_AMS_VERSION_PATCH;
                AMS_VERSION[3] = DEFAULT_AMS_VERSION_BUILD;
            }
            if (settings_save_to_flash()) {
                LOG_MAIN("[settings] 旧设置已迁移，AMS 版本=%d.%d.%d.%d", AMS_VERSION[0],
                         AMS_VERSION[1], AMS_VERSION[2], AMS_VERSION[3]);
            } else {
                apply_settings_values(data);
                LOG_MAIN("[settings] 设置迁移写入或回读失败，已恢复旧运行值");
            }
        }
        return;
    }

    LOG_MAIN("[settings] 没有可用的设置记录，已恢复默认值并重新写入");
    settings_reset_to_defaults();
    if (!settings_save_to_flash())
        LOG_MAIN("[settings] 默认设置写入失败，将在下次启动重试");
}

/**
 * 重新加载设置
 * 复用 settings_storage_init() 的读取与校验流程
 */
void settings_load_from_flash() {
    // 重新加载（与init相同逻辑）
    settings_storage_init();
}

/**
 * 生成当前设置快照
 * @return 包含所有运行期设置及 CRC32 的 SettingsData
 * 收集当前生效参数和接线配置，计算 checksum 后返回副本
 */
SettingsData settings_snapshot() {
    SettingsData data = {};

    for (int i = 0; i < 16; i++) {
        data.pullback_timeout[i] = PULLBACK_TIMEOUT[i];
        data.channel_mode[i] = CHANNEL_MODE[i];
    }
    data.ams_count = AMS_COUNT;
    data.manual_pulse_ms = MANUAL_PULSE_MS;
    data.auto_load_ms = AUTO_LOAD_MS;
    data.rs485_mode = RS485_MODE;
    data.bus_log_enable = BUS_LOG_ENABLE;
    data.ota_notify = OTA_NOTIFY;
    memset(data.custom_serial, 0, sizeof(data.custom_serial));
    memcpy(data.custom_serial, CUSTOM_SERIAL, sizeof(data.custom_serial));
    memset(data.ota_url, 0, sizeof(data.ota_url));
    memcpy(data.ota_url, OTA_URL, OTA_URL_MAX - 1);
    data.magic = SETTINGS_MAGIC_NUMBER;
    data.motor_limit = MOTOR_LIMIT;
    data.io_config = IO_CONFIG;
    data.pcf_sda = PCF_SDA;
    data.pcf_scl = PCF_SCL;
    data.bus_rx = BUS_RX; data.bus_tx = BUS_TX; data.bus_de = BUS_DE;
    data.pcf_enable = i2c_bus_configured() ? 1 : 0;
    memcpy(data.ams_version, AMS_VERSION, sizeof(AMS_VERSION));
    data.checksum = calculate_crc32(&data);
    return data;
}

/**
 * 从设置快照恢复运行期变量
 * @param data 要恢复的设置快照
 * 只应用内存参数，不写入持久化存储；用于保存失败时回滚
 */
void settings_restore_snapshot(const SettingsData &data) {
    apply_settings_values(data);
}

/**
 * 保存当前设置并校验
 * @return true 表示写入并回读一致
 * 生成快照、写入存储，再回读比较魔数和 CRC32
 */
bool settings_save_to_flash() {
    const SettingsData data = settings_snapshot();
    if (!write_settings_to_flash(&data)) return false;
    SettingsData saved{};
    read_settings_from_flash(&saved);
    return saved.magic == SETTINGS_MAGIC_NUMBER &&
           calculate_crc32(&saved) == saved.checksum &&
           memcmp(&data, &saved, sizeof(data)) == 0;
}

/**
 * 恢复所有设置默认值
 * 重置参数、引脚映射和 I2C 总线；OTA 恢复默认发布目录，不直接写存储
 */
void settings_reset_to_defaults() {
    for (int i = 0; i < 16; i++) {
        PULLBACK_TIMEOUT[i] = DEFAULT_PULLBACK_TIMEOUT;
        CHANNEL_MODE[i] = CHANNEL_MODE_AUTO;
    }
    AMS_COUNT = DEFAULT_AMS_COUNT;
    MANUAL_PULSE_MS = DEFAULT_MANUAL_PULSE_MS;
    AUTO_LOAD_MS = DEFAULT_AUTO_LOAD_MS;
    MOTOR_LIMIT = 1;
    RS485_MODE = DEFAULT_RS485_MODE;
    BUS_LOG_ENABLE = DEFAULT_BUS_LOG;
    memset(CUSTOM_SERIAL, 0, sizeof(CUSTOM_SERIAL));
    AMS_VERSION[0] = DEFAULT_AMS_VERSION_MAJOR;
    AMS_VERSION[1] = DEFAULT_AMS_VERSION_MINOR;
    AMS_VERSION[2] = DEFAULT_AMS_VERSION_PATCH;
    AMS_VERSION[3] = DEFAULT_AMS_VERSION_BUILD;
    memset(OTA_URL, 0, sizeof(OTA_URL));
    memcpy(OTA_URL, DEFAULT_OTA_URL, sizeof(DEFAULT_OTA_URL));
    OTA_NOTIFY = DEFAULT_OTA_NOTIFY;
    io_config_set_defaults(IO_CONFIG);
    bus_defaults();
    pcf_defaults();
}

/**
 * 检查存储中的设置是否有效
 * @return true 表示魔数和 CRC32 均有效
 * 当前记录和布局相同的上一版记录均校验完整 CRC32，上一版在加载时迁移
 */
bool settings_is_valid() {
    SettingsData data{};
    read_settings_from_flash(&data);
    return (data.magic == SETTINGS_MAGIC_NUMBER || data.magic == SETTINGS_MAGIC_PREVIOUS) &&
           calculate_crc32(&data) == data.checksum;
}

/**
 * 获取通道模式中文名称
 * @param mode 通道模式枚举
 * @return 离线/在线/自动/未知
 * 用于日志输出
 */
static const char *channel_mode_name(const int mode) {
    switch (mode) {
        case CHANNEL_MODE_OFFLINE: return "离线";
        case CHANNEL_MODE_ONLINE:  return "在线";
        case CHANNEL_MODE_AUTO:    return "自动";
        default:                   return "未知";
    }
}

// 把当前生效的设置打印到主板日志：启动时打印一次，之后每次保存设置再打印一次
/**
 * 打印当前全部设置
 * 把总线、序列号、通道、引脚、OTA 等参数分行写入主板日志，便于启动排查
 */
void settings_log_all() {
    LOG_MAIN("[settings] RS485 收发模式=%d，BambuBus 原始报文日志=%s",
             RS485_MODE, BUS_LOG_ENABLE ? "已启用" : "已关闭");
    LOG_MAIN("[settings] BambuBus RX=IO%d，TX=IO%d，DE=IO%d（%s）", BUS_RX, BUS_TX, BUS_DE,
             RS485_MODE == RS485_MODE_DE ? "占用" : "不占用");
    LOG_MAIN("[settings] BambuBus 序列号后缀=%s，AMS 版本=%d.%d.%d.%d",
             CUSTOM_SERIAL[0] ? CUSTOM_SERIAL : "设备 ID 自动生成",
             AMS_VERSION[0], AMS_VERSION[1], AMS_VERSION[2], AMS_VERSION[3]);
    LOG_MAIN("[settings] AMS 数量=%d，手动进退料点动时长=%dms", AMS_COUNT, MANUAL_PULSE_MS);
    LOG_MAIN("[settings] 同时运行电机上限=%d", MOTOR_LIMIT);
    if (i2c_bus_configured())
        LOG_MAIN("[settings] I2C 总线已配置：SDA=IO%d，SCL=IO%d（PCF8575/温湿度模块共用）", PCF_SDA, PCF_SCL);
    else
        LOG_MAIN("[settings] I2C 总线未配置，PCF8575/温湿度模块不启用");
    for (int i = 0; i < active_channel_count(); i++) {
        LOG_MAIN("[settings] 通道 %d IO=进料%s/退料%s/微动%s", i + 1,
                 io_config_pin_label(IO_CONFIG.motor[i][0]).c_str(),
                 io_config_pin_label(IO_CONFIG.motor[i][1]).c_str(),
                 io_config_pin_label(IO_CONFIG.microswitch[i]).c_str());
    }
    if (AUTO_LOAD_MS)
        LOG_MAIN("[settings] 微动辅助进料时长=%dms（自动模式：微动由无料变有料时触发）", AUTO_LOAD_MS);
    else
        LOG_MAIN("[settings] 微动辅助进料=已关闭");

    char line[220];
    size_t n = 0;
    line[0] = 0;
    for (int i = 0; i < active_channel_count(); i++) {
        n += static_cast<size_t>(snprintf(line + n, sizeof(line) - n, "%s%d:%s/退料%ds",
                                          i ? "，" : "", i + 1, channel_mode_name(CHANNEL_MODE[i]),
                                          PULLBACK_TIMEOUT[i]));
        if (n >= sizeof(line)) n = sizeof(line) - 1;
        // 每 4 个通道一行（正好一个 AMS），日志看起来更整齐
        if ((i % 4 == 3) || i == active_channel_count() - 1) {
            LOG_MAIN("[settings] 通道 %s", line);
            n = 0;
            line[0] = 0;
        }
    }

    LOG_MAIN("[settings] OTA 更新源=%s，进页面提示更新=%s",
             OTA_URL[0] ? OTA_URL : "(未设置)", OTA_NOTIFY ? "开" : "关");
}

/**
 * 读取通道退料超时
 * @param motor_index 通道编号 0-15
 * @return 退料超时秒数
 * 编号越界时返回默认值
 */
int get_motor_pullback_timeout(const int motor_index) {
    if (motor_index >= 0 && motor_index < 16) {
        return PULLBACK_TIMEOUT[motor_index];
    }
    return DEFAULT_PULLBACK_TIMEOUT;
}
