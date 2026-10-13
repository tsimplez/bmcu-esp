#include "settings_storage.h"
#include "platform.h"
#include "logbus.h"

// ============================
// 运行期设置变量（初值为默认值，启动时由 settings_storage_init() 覆盖）
// ============================
int PULLBACK_TIMEOUT[16] = {7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7};
int CHANNEL_MODE[16] = {1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1};
int AMS_COUNT = DEFAULT_AMS_COUNT;
int MANUAL_PULSE_MS = DEFAULT_MANUAL_PULSE_MS;
int AUTO_LOAD_MS = DEFAULT_AUTO_LOAD_MS;
int MOTOR_LIMIT = 1;
int RS485_MODE = DEFAULT_RS485_MODE;
int BUS_LOG_ENABLE = DEFAULT_BUS_LOG;
char CUSTOM_SERIAL[CUSTOM_SERIAL_LENGTH + 1] = {};
char OTA_URL[OTA_URL_MAX] = DEFAULT_OTA_URL;
int OTA_NOTIFY = DEFAULT_OTA_NOTIFY;
IoConfig IO_CONFIG = {};
int BUS_RX = PIN_BUS_RX, BUS_TX = PIN_BUS_TX, BUS_DE = PIN_BUS_DE;

static void bus_defaults() {
    BUS_RX = PIN_BUS_RX; BUS_TX = PIN_BUS_TX; BUS_DE = PIN_BUS_DE;
}

int PCF_SDA = PIN_I2C_SDA;
int PCF_SCL = PIN_I2C_SCL;
// Web 里的 PCF8575 开关：默认关闭，只用主控直连 GPIO；打开后通道可改用扩展脚。
int PCF_ENABLE = 0;

static void pcf_defaults() {
    PCF_SDA = PIN_I2C_SDA;
    PCF_SCL = PIN_I2C_SCL;
}

// 当前设置记录使用 CRC32，计算时把 checksum 字段视为 0，避免校验值自引用。
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

bool settings_custom_serial_valid(const char *serial) {
    if (!serial) return false;
    size_t length = 0;
    while (length <= CUSTOM_SERIAL_LENGTH && serial[length]) length++;
    if (length == 0) return true;
    if (length != CUSTOM_SERIAL_LENGTH || serial[CUSTOM_SERIAL_LENGTH] != 0) return false;
    for (size_t i = 0; i < CUSTOM_SERIAL_LENGTH; i++) {
        const char c = serial[i];
        if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z'))) return false;
    }
    return true;
}

// 从Flash读取设置数据
static void read_settings_from_flash(SettingsData* data) {
    hal_storage_read(HAL_REGION_SETTINGS, data, sizeof(SettingsData));
}

// 写入设置到Flash
static bool write_settings_to_flash(const SettingsData* data) {
    return hal_storage_write(HAL_REGION_SETTINGS, data, sizeof(SettingsData));
}

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

    memcpy(CUSTOM_SERIAL, data.custom_serial, sizeof(CUSTOM_SERIAL));
    CUSTOM_SERIAL[CUSTOM_SERIAL_LENGTH] = 0;
    if (!settings_custom_serial_valid(CUSTOM_SERIAL)) CUSTOM_SERIAL[0] = 0;

    OTA_NOTIFY = data.ota_notify == 0 || data.ota_notify == 1 ? data.ota_notify : DEFAULT_OTA_NOTIFY;
    memcpy(OTA_URL, data.ota_url, OTA_URL_MAX);
    OTA_URL[OTA_URL_MAX - 1] = 0;  // 强行截断，防止 NVS 里出现没有结束符的脏数据
    MOTOR_LIMIT = data.motor_limit >= 1 && data.motor_limit <= IO_CONFIG_MAX_CHANNELS
                      ? min(static_cast<int>(data.motor_limit), active_channel_count()) : 1;

    // 先加载整组候选接线，再交叉校验，避免用旧总线引脚误判新配置。
    IoConfig loaded = data.io_config;
    BUS_RX = data.bus_rx; BUS_TX = data.bus_tx; BUS_DE = data.bus_de;
    // PCF8575 开关要先于接线校验生效：关闭时虚拟引脚不可用、I2C 引脚可作普通 IO。
    PCF_ENABLE = data.pcf_enable == 0 || data.pcf_enable == 1 ? data.pcf_enable : 0;
    pcf_defaults();
    PCF_SDA = data.pcf_sda; PCF_SCL = data.pcf_scl;
    String io_error;
    bool wiring_valid = io_config_bus_validate(BUS_RX, BUS_TX, BUS_DE, RS485_MODE,
        PCF_SDA, PCF_SCL, loaded, active_channel_count(), &io_error);
    if (PCF_ENABLE)
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

void settings_storage_init() {
    SettingsData data{};

    read_settings_from_flash(&data);

    // 还在开发中，不兼容旧记录：魔数或 CRC32 对不上就整体恢复默认值后重写。
    if (data.magic == SETTINGS_MAGIC_NUMBER && calculate_crc32(&data) == data.checksum) {
        apply_settings_values(data);
        return;
    }

    LOG_MAIN("[settings] 没有可用的设置记录，已恢复默认值并重新写入");
    settings_reset_to_defaults();
    if (!settings_save_to_flash())
        LOG_MAIN("[settings] 默认设置写入失败，将在下次启动重试");
}

void settings_load_from_flash() {
    // 重新加载（与init相同逻辑）
    settings_storage_init();
}

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
    memcpy(data.custom_serial, CUSTOM_SERIAL, CUSTOM_SERIAL_LENGTH);
    memset(data.ota_url, 0, sizeof(data.ota_url));
    memcpy(data.ota_url, OTA_URL, OTA_URL_MAX - 1);
    data.magic = SETTINGS_MAGIC_NUMBER;
    data.motor_limit = MOTOR_LIMIT;
    data.io_config = IO_CONFIG;
    data.pcf_sda = PCF_SDA;
    data.pcf_scl = PCF_SCL;
    data.bus_rx = BUS_RX; data.bus_tx = BUS_TX; data.bus_de = BUS_DE;
    data.pcf_enable = PCF_ENABLE;
    data.checksum = calculate_crc32(&data);
    return data;
}

void settings_restore_snapshot(const SettingsData &data) {
    apply_settings_values(data);
}

bool settings_save_to_flash() {
    const SettingsData data = settings_snapshot();
    if (!write_settings_to_flash(&data)) return false;
    SettingsData saved{};
    read_settings_from_flash(&saved);
    return saved.magic == SETTINGS_MAGIC_NUMBER &&
           calculate_crc32(&saved) == saved.checksum &&
           memcmp(&data, &saved, sizeof(data)) == 0;
}

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
    memset(OTA_URL, 0, sizeof(OTA_URL));
    OTA_NOTIFY = DEFAULT_OTA_NOTIFY;
    PCF_ENABLE = 0;
    io_config_set_defaults(IO_CONFIG);
    bus_defaults();
    pcf_defaults();
}

bool settings_is_valid() {
    SettingsData data{};
    read_settings_from_flash(&data);

    return data.magic == SETTINGS_MAGIC_NUMBER && calculate_crc32(&data) == data.checksum;
}

static const char *channel_mode_name(int mode) {
    switch (mode) {
        case CHANNEL_MODE_OFFLINE: return "离线";
        case CHANNEL_MODE_ONLINE:  return "在线";
        case CHANNEL_MODE_AUTO:    return "自动";
        default:                   return "未知";
    }
}

// 把当前生效的设置打印到主板日志：启动时打印一次，之后每次保存设置再打印一次
void settings_log_all() {
    LOG_MAIN("[settings] RS485 收发模式=%d，BambuBus 原始报文日志=%s",
             RS485_MODE, BUS_LOG_ENABLE ? "已启用" : "已关闭");
    LOG_MAIN("[settings] BambuBus RX=IO%d，TX=IO%d，DE=IO%d（%s）", BUS_RX, BUS_TX, BUS_DE,
             RS485_MODE == RS485_MODE_DE ? "占用" : "不占用");
    LOG_MAIN("[settings] BambuBus 序列号=%s",
             CUSTOM_SERIAL[0] ? CUSTOM_SERIAL : "自动生成");
    LOG_MAIN("[settings] AMS 数量=%d，手动进退料点动时长=%dms", AMS_COUNT, MANUAL_PULSE_MS);
    LOG_MAIN("[settings] 同时运行电机上限=%d", MOTOR_LIMIT);
    if (PCF_ENABLE)
        LOG_MAIN("[settings] PCF8575 已启用：SDA=IO%d，SCL=IO%d", PCF_SDA, PCF_SCL);
    else
        LOG_MAIN("[settings] PCF8575 已关闭，通道使用直连 GPIO");
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

// 获取指定电机的退料超时时间
int get_motor_pullback_timeout(int motor_index) {
    if (motor_index >= 0 && motor_index < 16) {
        return PULLBACK_TIMEOUT[motor_index];
    }
    return DEFAULT_PULLBACK_TIMEOUT;
}
