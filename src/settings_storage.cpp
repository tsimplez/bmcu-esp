#include "settings_storage.h"
#include "platform.h"
#include "log_bus.h"
#include "i2c_bus.h"
#include "project_presets.h"
#include "PCF8575_PinMap.h"
#include <cstring>

// ============================
// 运行期设置变量（初值为默认值，启动时由 settings_storage_init() 覆盖）
// ============================

// 各通道退料动作超时时间，单位秒；打印机请求退料时用作电机限时停止保护
int PULLBACK_TIMEOUT[16] = {7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7};
// 各通道工作模式：0=离线，1=在线，2=自动（按微动开关触发辅助进料）
int CHANNEL_MODE[16] = {1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1};
// 启用的 AMS 数量（1-4），每个 AMS 固定对应 4 个耗材通道
int AMS_COUNT = hal_default_project_preset().ams;
// Web 手动进退料按钮的默认点动时长，单位毫秒
int MANUAL_PULSE_MS = DEFAULT_MANUAL_PULSE_MS;
// 微动由无料变有料后的辅助进料时长，单位毫秒；0 表示关闭
int AUTO_LOAD_MS = DEFAULT_AUTO_LOAD_MS;
// 打印机进料结束后继续运行的时长，单位毫秒；0 表示立即停车
int FEED_STOP_DELAY_MS = DEFAULT_FEED_STOP_DELAY_MS;
// 五通总微动压下沿触发当前通道进料的独立时长设置
int TOTAL_SWITCH_ACTIVE_LEVEL = DEFAULT_TOTAL_SWITCH_ACTIVE_LEVEL;
int TOTAL_SWITCH_FEED_MS = DEFAULT_TOTAL_SWITCH_FEED_MS;
int TOTAL_SWITCH_PAUSE_TIMEOUT_S = DEFAULT_TOTAL_SWITCH_PAUSE_TIMEOUT_S;
char PROJECT_NAME[PROJECT_NAME_BUFFER_SIZE] = "团子 BMCU";
// 允许同时运行的电机数量上限，运行时由设置加载值限制到 active_channel_count()
int MOTOR_LIMIT = 1;
// BambuBus 的 RS485 收发模式：RS485_MODE_RXTX 或 RS485_MODE_DE
int RS485_MODE = hal_default_project_preset().bus.mode;
// 是否把 BambuBus 原始收发报文写入总线日志（0=关，1=开）
int BUS_LOG_ENABLE = DEFAULT_BUS_LOG;
// 各 AMS 自定义序列号后缀；空串表示按芯片 ID 和 AMS 编号自动生成
char CUSTOM_SERIAL[AMS_IDENTITY_COUNT][CUSTOM_SERIAL_LENGTH + 1] = {};
// 各 AMS 返回给打印机的版本，网页按 A.B.C.D 显示，总线载荷按 D.C.B.A 写入
int AMS_VERSION[AMS_IDENTITY_COUNT][4] = {
    {DEFAULT_AMS_VERSION_MAJOR, DEFAULT_AMS_VERSION_MINOR, DEFAULT_AMS_VERSION_PATCH, DEFAULT_AMS_VERSION_BUILD},
    {DEFAULT_AMS_VERSION_MAJOR, DEFAULT_AMS_VERSION_MINOR, DEFAULT_AMS_VERSION_PATCH, DEFAULT_AMS_VERSION_BUILD},
    {DEFAULT_AMS_VERSION_MAJOR, DEFAULT_AMS_VERSION_MINOR, DEFAULT_AMS_VERSION_PATCH, DEFAULT_AMS_VERSION_BUILD},
    {DEFAULT_AMS_VERSION_MAJOR, DEFAULT_AMS_VERSION_MINOR, DEFAULT_AMS_VERSION_PATCH, DEFAULT_AMS_VERSION_BUILD}
};
// OTA 固件发布目录，设备会读取该目录下的 manifest.json
char OTA_URL[OTA_URL_MAX] = DEFAULT_OTA_URL;
// 打开网页时是否自动检查 OTA 更新（0=关，1=开）
int OTA_NOTIFY = DEFAULT_OTA_NOTIFY;
// 各通道电机、微动开关和霍尔 ADC 引脚的当前映射
IoConfig IO_CONFIG = {};
// 启动时从设置加载；未校准前默认关闭霍尔停转检测
HallDetectionConfig HALL_DETECTION[IO_CONFIG_MAX_CHANNELS] = {};
// 局域网打印机凭据和换料参数；与其他设置一起保存，访问码不写日志
LanAmsConfig LAN_AMS_CONFIG = DEFAULT_LAN_AMS_CONFIG;
// BambuBus 串口 RX/TX 和 RS485 DE 引脚；启动时从设置加载
int BUS_RX = hal_default_project_preset().bus.rx;
int BUS_TX = hal_default_project_preset().bus.tx;
int BUS_DE = hal_default_project_preset().bus.de;

/**
 * 恢复 BambuBus 引脚默认值
 * 从平台默认项目恢复 RX、TX、DE，和网页预设共用接线数据
 */
static void bus_defaults() {
    const auto &bus = hal_default_project_preset().bus;
    BUS_RX = bus.rx; BUS_TX = bus.tx; BUS_DE = bus.de;
}

// I2C 总线 SDA 引脚；默认未配置，需在网页中同时选择 SDA/SCL 后重启启用
int PCF_SDA = hal_default_project_preset().bus.sda;
// I2C 总线 SCL 引脚；默认未配置，需在网页中同时选择 SDA/SCL 后重启启用
int PCF_SCL = hal_default_project_preset().bus.scl;

/**
 * 恢复 I2C 引脚默认值
 * 复用平台默认项目的 SDA/SCL；默认 C3 直连项目不启用 I2C
 */
static void pcf_defaults() {
    PCF_SDA = hal_default_project_preset().bus.sda;
    PCF_SCL = hal_default_project_preset().bus.scl;
}

namespace {
/**
 * 按记录布局计算设置 CRC32
 * @param bytes 原始记录
 * @param length 记录的完整字节数
 * @param checksum_offset checksum 字段偏移
 * @return checksum 字段视为零时的 CRC32
 */
uint32_t calculate_record_crc32(const uint8_t *bytes, const size_t length, const size_t checksum_offset) {
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < length; ++i) {
        crc ^= i >= checksum_offset && i < checksum_offset + 4 ? 0 : bytes[i];
        for (int bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

/**
 * 计算设置记录 CRC32
 * @param data 设置数据结构
 * @return 计算得到的 32 位 CRC
 * 使用当前布局并把 checksum 字段视为零，避免校验值自引用
 */
static uint32_t calculate_crc32(const SettingsData* data) {
    return calculate_record_crc32(reinterpret_cast<const uint8_t *>(data), sizeof(SettingsData), offsetof(SettingsData, checksum));
}

static_assert(offsetof(IoConfig, clutch_enabled) == 260, "历史 IO 基础布局发生变化");
static_assert(sizeof(HallDetectionConfig) == 8 && sizeof(LanAmsConfig) == 68, "历史传感器或局域网布局发生变化");
static_assert(sizeof(IoConfig) == 348, "当前历史 IO 布局发生变化");
static_assert(sizeof(SettingsData) == 1116, "当前设置布局大小发生变化");
struct LegacyFormat {
    uint32_t magic;
    size_t size;
    size_t checksum_offset;
    size_t serial_size;
    size_t io_size;
    uint8_t version_count;
    bool pcf_enable;
    bool continuous_pins;
    bool single_device;
    bool feed_stop_delay;
    bool hall_detection;
    bool lan_ams;
    uint8_t total_switch_parameter_size;
};

// 已发布标签及开发提交的真实布局；重复魔数按长度和原 CRC 区分，身份数量不决定 checksum 偏移
// 字段顺序：魔数、长度、checksum 偏移、序列号长度、IO 长度、版本数量、旧 PCF 开关、连续编号、仅首片、停止延迟、霍尔、局域网、五通参数字节数
constexpr LegacyFormat LEGACY_FORMATS[] = {
    {0x53415432, 980, 144, 48, 348, 4, false, false, false, true,  true,  true,  8}, // 五通电平及进料时长
    {0x53415433, 984, 144, 48, 348, 4, false, false, false, true,  true,  true, 12}  // 含五通持续压下超时，增加项目名前
};

/**
 * 安全读取历史记录中的 32 位字段
 * @param bytes 已通过长度校验的记录
 * @param offset 字段字节偏移
 * @return 字段值
 * 使用 memcpy 避免未对齐指针和别名访问
 */
uint32_t read_word(const uint8_t *bytes, const size_t offset) {
    uint32_t value;
    memcpy(&value, bytes + offset, sizeof(value));
    return value;
}

/**
 * 校验历史布局的完整长度、格式标识及原 CRC
 * @param bytes 原始记录
 * @param length 实际读取长度
 * @param format 历史布局描述
 * @param padded 是否来自没有记录长度的固定 Flash 扇区
 * @return 布局和校验均匹配时返回 true
 * Flash 允许全 0xFF 的尾部填充；NVS 必须精确匹配 blob 长度
 */
bool matches_format(const uint8_t *bytes, const size_t length, const LegacyFormat &format, const bool padded) {
    if (length < format.size || (!padded && length != format.size)) return false;
    for (size_t i = format.size; i < length; ++i) if (bytes[i] != 0xFF) return false;
    const size_t checksum = format.checksum_offset;
    return read_word(bytes, checksum + 4) == format.magic &&
        read_word(bytes, checksum) == calculate_record_crc32(bytes, format.size, checksum);
}

/**
 * 把旧连续扩展编号转换为当前面板编号
 * @param pin 历史引脚编号
 * @param single_device 历史布局是否只支持第一片扩展器
 * @return 同一芯片和寄存器位对应的新编号；其他编号保持原值
 */
int32_t migrate_pin(const int32_t pin, const bool single_device) {
    const int limit = single_device ? 66 : 114;
    if (pin < 50 || pin >= limit) return pin;
    const int index = pin - 50, bit = index % 16;
    return PCF8575_VIRTUAL_PIN_BASE + (index / 16) * PCF8575_VIRTUAL_PIN_STRIDE + bit + (bit >= 8 ? 2 : 0);
}

/**
 * 把已校验历史记录的有效字段复制到新布局
 * @param bytes 已校验的原始记录
 * @param format 对应历史布局
 * @param out 已填入当前默认值的候选设置
 * 仅覆盖旧布局已有字段；新字段使用候选默认值，不写入持久化存储
 */
void copy_legacy(const uint8_t *bytes, const LegacyFormat &format, SettingsData &out) {
    memcpy(out.pullback_timeout, bytes, 64);
    memcpy(out.channel_mode, bytes + 64, 64);
    memcpy(&out.ams_count, bytes + 128, 12);
    const size_t checksum = format.checksum_offset;
    if (format.feed_stop_delay) memcpy(&out.feed_stop_delay_ms, bytes + 140, 4);
    const size_t control = checksum + 8;
    memcpy(&out.rs485_mode, bytes + control, 8);
    memcpy(out.ota_url, bytes + control + 8, sizeof(out.ota_url));
    memcpy(&out.ota_notify, bytes + control + 136, 8);
    const size_t identities = control + 144;
    const size_t serial_bytes = format.version_count == AMS_IDENTITY_COUNT ? sizeof(out.custom_serial) : CUSTOM_SERIAL_LENGTH;
    memcpy(out.custom_serial, bytes + identities, serial_bytes);
    // 最早格式的序列号含结尾 NUL，占 13 字节；其后的 IO 仍按 4 字节对齐
    const size_t io = (identities + format.serial_size + 3u) & ~3u;
    // motor 和 microswitch 在全部旧布局中相同；新增输入不得继承当前默认项目的已有接线
    memcpy(&out.io_config, bytes + io, 192);
    out.io_config.switch_pin = PIN_NONE;
    for (int32_t &pin : out.io_config.hall) pin = PIN_NONE;
    if (format.io_size >= 196) memcpy(&out.io_config.switch_pin, bytes + io + 192, 4);
    if (format.io_size >= 260) memcpy(out.io_config.hall, bytes + io + 196, 64);
    if (format.io_size >= sizeof(IoConfig)) memcpy(&out.io_config, bytes + io, sizeof(IoConfig));
    else io_config_set_clutch_defaults(out.io_config);
    if (format.continuous_pins) {
        for (int ch = 0; ch < IO_CONFIG_MAX_CHANNELS; ++ch) {
            for (int direction = 0; direction < 2; ++direction)
                out.io_config.motor[ch][direction] = migrate_pin(out.io_config.motor[ch][direction], format.single_device);
            out.io_config.microswitch[ch] = migrate_pin(out.io_config.microswitch[ch], format.single_device);
        }
    }
    const size_t buses = io + format.io_size;
    memcpy(&out.pcf_sda, bytes + buses, 20);
    const size_t versions = buses + 20 + (format.pcf_enable ? 4 : 0);
    if (format.version_count == AMS_IDENTITY_COUNT) memcpy(out.ams_version, bytes + versions, sizeof(out.ams_version));
    else if (format.version_count == 1) for (auto & ams : out.ams_version)
        memcpy(ams, bytes + versions, 16); // 旧协议全部 AMS 使用同一份版本
    size_t extension = versions + (format.version_count == AMS_IDENTITY_COUNT ? sizeof(out.ams_version) :
                                   format.version_count == 1 ? 16u : 0u);
    if (format.hall_detection) {
        memcpy(out.hall_detection, bytes + extension, sizeof(out.hall_detection));
        extension += sizeof(out.hall_detection);
    }
    if (format.lan_ams) {
        memcpy(&out.lan_ams, bytes + extension, sizeof(out.lan_ams));
        extension += sizeof(out.lan_ams);
    }
    if (format.total_switch_parameter_size)
        memcpy(&out.total_switch_active_level, bytes + extension, format.total_switch_parameter_size);
    memcpy(out.project_name, DEFAULT_PROJECT_NAME, sizeof(DEFAULT_PROJECT_NAME));
    out.magic = SETTINGS_MAGIC_NUMBER;
    out.checksum = 0;
}

/**
 * 校验并迁移已发布及开发提交的历史设置记录
 * @param bytes 原始记录缓冲区
 * @param length 实际读取的字节数
 * @param out 使用当前默认值初始化的候选设置
 * @param padded 是否允许固定 Flash 扇区的全 0xFF 尾部填充
 * @return 找到原格式且完整 CRC 正确时返回 true，否则保留候选和原始记录
 * 先按原布局验证，再迁移字段；兼容同魔数不同长度、早期序列号对齐和各字段独立引入，不执行写入
 */
bool settings_migrate_record(const uint8_t *bytes, const size_t length, SettingsData &out, const bool padded = false) {
    if (!bytes) return false;
    for (const LegacyFormat &format : LEGACY_FORMATS) {
        if (!matches_format(bytes, length, format, padded)) continue;
        copy_legacy(bytes, format, out);
        return true;
    }
    return false;
}
} // namespace

/**
 * 校验固件保存的项目名
 * @param name UTF-8 项目名，允许空串以恢复默认名称
 * @return 长度和 UTF-8 编码均有效时返回 true
 */
bool settings_project_name_valid(const char *name) {
    if (!name) return false;
    size_t length = 0;
    while (length < PROJECT_NAME_BUFFER_SIZE && name[length]) length++;
    if (length >= PROJECT_NAME_BUFFER_SIZE) return false;
    for (size_t i = 0; i < length;) {
        const auto first = static_cast<uint8_t>(name[i]);
        if (first < 0x80) {
            if (first < 0x20 || first == 0x7F) return false;
            ++i;
            continue;
        }
        size_t continuation_count;
        uint32_t codepoint;
        if (first >= 0xC2 && first <= 0xDF) {
            continuation_count = 1;
            codepoint = first & 0x1Fu;
        } else if (first >= 0xE0 && first <= 0xEF) {
            continuation_count = 2;
            codepoint = first & 0x0Fu;
        } else if (first >= 0xF0 && first <= 0xF4) {
            continuation_count = 3;
            codepoint = first & 0x07u;
        } else {
            return false;
        }
        if (i + continuation_count >= length) return false;
        for (size_t j = 1; j <= continuation_count; ++j) {
            const auto next = static_cast<uint8_t>(name[i + j]);
            if ((next & 0xC0u) != 0x80u) return false;
            codepoint = (codepoint << 6) | (next & 0x3Fu);
        }
        if ((continuation_count == 1 && codepoint < 0x80u) ||
            (continuation_count == 2 && codepoint < 0x800u) ||
            (continuation_count == 3 && codepoint < 0x10000u) ||
            (codepoint >= 0xD800u && codepoint <= 0xDFFFu) || codepoint > 0x10FFFFu) return false;
        i += continuation_count + 1;
    }
    return true;
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
 * 读取指定 AMS 实际发送的完整序列号
 * @param ams 协议 AMS 编号，范围 0..3，与当前启用数量无关
 * @param out 输出 16 位序列号和结尾 NUL，至少容纳 AMS_SERIAL_LENGTH + 1 字节
 * @return 编号及输出缓冲有效时返回 true，否则返回 false 并清空非空输出缓冲
 * 自动后缀取芯片 ID 的低 48 位十六进制值，并把末位替换为 AMS 编号；自定义后缀原样保留
 * 本函数只读取配置，不刷新注册状态
 */
bool settings_get_ams_serial(uint8_t ams, char out[AMS_SERIAL_LENGTH + 1]) {
    if (!out) return false;
    out[0] = 0;
    if (ams >= AMS_IDENTITY_COUNT) return false;
    memcpy(out, "0EA0", 4);
    out[3] = static_cast<char>('0' + ams);
    if (CUSTOM_SERIAL[ams][0]) {
        memcpy(out + 4, CUSTOM_SERIAL[ams], CUSTOM_SERIAL_LENGTH);
    } else {
        static constexpr char hex[] = "0123456789ABCDEF";
        const uint64_t id = hal_get_device_id();
        for (int i = 0; i < CUSTOM_SERIAL_LENGTH; ++i)
            out[4 + i] = hex[(id >> ((CUSTOM_SERIAL_LENGTH - 1 - i) * 4)) & 0x0Fu];
        out[AMS_SERIAL_LENGTH - 1] = static_cast<char>('0' + ams);
    }
    out[AMS_SERIAL_LENGTH] = 0;
    return true;
}

/**
 * 解析 A.B.C.D 格式的 AMS 版本
 * @param text 待解析文本
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
 * 校验霍尔双阈值和无转动超时
 * @param config 待检查的通道检测配置
 * @return 下限小于上限、上限不超过 4095 且超时不超过一小时时返回 true
 */
bool settings_hall_detection_valid(const HallDetectionConfig &config) {
    return config.lower < config.upper && config.upper <= 4095 && config.timeout_s <= MAX_HALL_TIMEOUT_S;
}

/**
 * 校验局域网打印机凭据和换料参数
 * @param config 待检查的局域网配置
 * @param require_credentials true 时必须完整填写打印机凭据
 * @return true 表示字符串、IPv4 地址、访问码、序列号和参数范围均合法
 * 凭据允许全部为空但不允许部分为空；IPv4 必须为四段十进制数字，不接受前导零或多余字符
 */
bool settings_lan_config_valid(const LanAmsConfig &config, const bool require_credentials) {
    if (!memchr(config.printer_ip, '\0', sizeof(config.printer_ip)) ||
        !memchr(config.access_code, '\0', sizeof(config.access_code)) ||
        !memchr(config.serial, '\0', sizeof(config.serial)) ||
        config.feed_ms < 100 || config.feed_ms >= MAX_PULSE_MS ||
        config.nozzle_temp < 170 || config.nozzle_temp > 300) return false;
    if (!config.printer_ip[0] && !config.access_code[0] && !config.serial[0])
        return !require_credentials;
    if (!config.printer_ip[0] || strlen(config.access_code) != 8 || !config.serial[0]) return false;

    const char *p = config.printer_ip;
    for (int segment = 0; segment < 4; ++segment) {
        const char *start = p;
        unsigned value = 0;
        while (*p >= '0' && *p <= '9') {
            value = value * 10 + static_cast<unsigned>(*p++ - '0');
            if (value > 255 || p - start > 3) return false;
        }
        if (p == start || (p - start > 1 && *start == '0')) return false;
        if (segment == 3) {
            if (*p) return false;
        } else if (*p++ != '.') {
            return false;
        }
    }
    const char *values[] = {config.access_code, config.serial};
    for (const char *value : values) {
        for (; *value; ++value) {
            const char c = *value;
            if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')))
                return false;
        }
    }
    return true;
}

/**
 * 校验并应用设置到运行期变量
 * @param data 从存储或快照得到的设置
 * 逐项检查范围后更新全局配置；项目名、各 AMS 身份、霍尔检测、五通总微动及停止打印保护超时、局域网配置独立校验，保留未启用通道的配置
 * 通道模式不在有效范围时恢复为自动模式
 * I2C 仅由 SDA/SCL 决定；升级后不可用接线只清理相关字段，保留已有有效参数及原始存储记录
 */
static void apply_settings_values(const SettingsData& data) {
    for (int i = 0; i < 16; i++) {
        PULLBACK_TIMEOUT[i] = data.pullback_timeout[i] >= 3 && data.pullback_timeout[i] <= 30
                                  ? data.pullback_timeout[i] : DEFAULT_PULLBACK_TIMEOUT;
        CHANNEL_MODE[i] = data.channel_mode[i] >= 0 && data.channel_mode[i] <= 2
                              ? data.channel_mode[i] : static_cast<int32_t>(CHANNEL_MODE_AUTO);
        HALL_DETECTION[i] = settings_hall_detection_valid(data.hall_detection[i])
                                ? data.hall_detection[i] : DEFAULT_HALL_DETECTION;
    }

    AMS_COUNT = data.ams_count >= 1 && data.ams_count <= IO_CONFIG_MAX_CHANNELS / 4
                    ? data.ams_count : hal_default_project_preset().ams;
    MANUAL_PULSE_MS = data.manual_pulse_ms > 0 && data.manual_pulse_ms < MAX_PULSE_MS
                          ? data.manual_pulse_ms : DEFAULT_MANUAL_PULSE_MS;
    AUTO_LOAD_MS = data.auto_load_ms >= 0 && data.auto_load_ms < MAX_PULSE_MS
                       ? data.auto_load_ms : DEFAULT_AUTO_LOAD_MS;
    FEED_STOP_DELAY_MS = data.feed_stop_delay_ms >= 0 && data.feed_stop_delay_ms < MAX_PULSE_MS
                            ? data.feed_stop_delay_ms : DEFAULT_FEED_STOP_DELAY_MS;
    TOTAL_SWITCH_ACTIVE_LEVEL = data.total_switch_active_level == LOW || data.total_switch_active_level == HIGH
                                    ? data.total_switch_active_level : DEFAULT_TOTAL_SWITCH_ACTIVE_LEVEL;
    TOTAL_SWITCH_FEED_MS = data.total_switch_feed_ms > 0 && data.total_switch_feed_ms < MAX_PULSE_MS
                               ? data.total_switch_feed_ms : DEFAULT_TOTAL_SWITCH_FEED_MS;
    TOTAL_SWITCH_PAUSE_TIMEOUT_S = data.total_switch_pause_timeout_s >= MIN_TOTAL_SWITCH_PAUSE_TIMEOUT_S &&
                                   data.total_switch_pause_timeout_s <= MAX_TOTAL_SWITCH_PAUSE_TIMEOUT_S
                                       ? data.total_switch_pause_timeout_s : DEFAULT_TOTAL_SWITCH_PAUSE_TIMEOUT_S;
    RS485_MODE = data.rs485_mode == RS485_MODE_RXTX || data.rs485_mode == RS485_MODE_DE
                     ? data.rs485_mode : hal_default_project_preset().bus.mode;
    BUS_LOG_ENABLE = data.bus_log_enable == 0 || data.bus_log_enable == 1
                         ? data.bus_log_enable : DEFAULT_BUS_LOG;

    for (int ams = 0; ams < AMS_IDENTITY_COUNT; ++ams) {
        memcpy(CUSTOM_SERIAL[ams], data.custom_serial[ams], CUSTOM_SERIAL_LENGTH);
        CUSTOM_SERIAL[ams][CUSTOM_SERIAL_LENGTH] = 0;
        if (!settings_custom_serial_valid(CUSTOM_SERIAL[ams]))
            memset(CUSTOM_SERIAL[ams], 0, sizeof(CUSTOM_SERIAL[ams]));

        bool version_valid = true;
        for (int i = 0; i < 4; ++i)
            if (data.ams_version[ams][i] < 0 || data.ams_version[ams][i] > 255) version_valid = false;
        if (version_valid) {
            memcpy(AMS_VERSION[ams], data.ams_version[ams], sizeof(AMS_VERSION[ams]));
        } else {
            AMS_VERSION[ams][0] = DEFAULT_AMS_VERSION_MAJOR;
            AMS_VERSION[ams][1] = DEFAULT_AMS_VERSION_MINOR;
            AMS_VERSION[ams][2] = DEFAULT_AMS_VERSION_PATCH;
            AMS_VERSION[ams][3] = DEFAULT_AMS_VERSION_BUILD;
        }
    }

    OTA_NOTIFY = data.ota_notify == 0 || data.ota_notify == 1 ? data.ota_notify : DEFAULT_OTA_NOTIFY;
    memcpy(OTA_URL, data.ota_url, OTA_URL_MAX);
    OTA_URL[OTA_URL_MAX - 1] = 0;  // 强行截断，防止 NVS 里出现没有结束符的脏数据
    memset(PROJECT_NAME, 0, sizeof(PROJECT_NAME));
    if (data.project_name[0] && settings_project_name_valid(data.project_name))
        memcpy(PROJECT_NAME, data.project_name, strlen(data.project_name));
    else
        memcpy(PROJECT_NAME, DEFAULT_PROJECT_NAME, sizeof(DEFAULT_PROJECT_NAME));
    MOTOR_LIMIT = data.motor_limit >= 1 && data.motor_limit <= IO_CONFIG_MAX_CHANNELS
                      ? min(static_cast<int>(data.motor_limit), active_channel_count()) : 1;
    LAN_AMS_CONFIG = settings_lan_config_valid(data.lan_ams) ? data.lan_ams : DEFAULT_LAN_AMS_CONFIG;

    // 先加载整组候选接线，再交叉校验，避免用旧总线引脚误判新配置
    IoConfig loaded = data.io_config;
    if (!io_config_clutch_valid(loaded)) {
        if (loaded.clutch_enabled != 0 && loaded.clutch_enabled != 1) loaded.clutch_enabled = 0;
        if (loaded.clutch_active_level != LOW && loaded.clutch_active_level != HIGH) loaded.clutch_active_level = HIGH;
        if (loaded.clutch_engage_ms < 0 || loaded.clutch_engage_ms > MAX_CLUTCH_DELAY_MS)
            loaded.clutch_engage_ms = DEFAULT_CLUTCH_DELAY_MS;
        if (loaded.clutch_release_ms < 0 || loaded.clutch_release_ms > MAX_CLUTCH_DELAY_MS)
            loaded.clutch_release_ms = DEFAULT_CLUTCH_DELAY_MS;
    }
    BUS_RX = data.bus_rx; BUS_TX = data.bus_tx; BUS_DE = data.bus_de;
    // I2C 总线由 SDA/SCL 是否同时配置决定
    PCF_SDA = data.pcf_sda; PCF_SCL = data.pcf_scl;
    // 只配置单个引脚属于非法状态，按未配置处理
    if ((PCF_SDA == PIN_NONE) != (PCF_SCL == PIN_NONE)) {
        PCF_SDA = PIN_NONE;
        PCF_SCL = PIN_NONE;
    }
    // 两脚相同或负值按“总线未配置”处理，其余不可用引脚由下方接线校验拒绝
    if (!i2c_bus_configured()) {
        PCF_SDA = PIN_NONE;
        PCF_SCL = PIN_NONE;
    }
    IoConfig unused{};
    io_config_set_clutch_defaults(unused);
    unused.switch_pin = PIN_NONE;
    for (int ch = 0; ch < IO_CONFIG_MAX_CHANNELS; ++ch) {
        unused.motor[ch][0] = unused.motor[ch][1] = unused.microswitch[ch] = unused.hall[ch] = PIN_NONE;
    }
    String io_error;
    IoBusConfig bus{BUS_RX, BUS_TX, BUS_DE, RS485_MODE, PCF_SDA, PCF_SCL};
    if (i2c_bus_configured() && !io_config_i2c_validate(PCF_SDA, PCF_SCL, unused, active_channel_count(), &io_error, &bus)) {
        PCF_SDA = PCF_SCL = PIN_NONE;
        bus.sda = bus.scl = PIN_NONE;
        LOG_MAIN("[settings] I2C 接线无法继续使用，已清空：%s", io_error.c_str());
    }
    if (!io_config_bus_validate(BUS_RX, BUS_TX, BUS_DE, RS485_MODE, PCF_SDA, PCF_SCL, unused, active_channel_count(), &io_error)) {
#if defined(ARDUINO_ARCH_ESP32)
        BUS_RX = BUS_TX = BUS_DE = PIN_NONE;
#else
        bus_defaults();
        RS485_MODE = hal_default_project_preset().bus.mode;
#endif
        bus.rx = BUS_RX; bus.tx = BUS_TX; bus.de = BUS_DE; bus.mode = RS485_MODE;
        LOG_MAIN("[settings] BambuBus 接线无法继续使用，已调整总线字段：%s", io_error.c_str());
    }
    if (const int cleared = io_config_sanitize_pins(loaded, active_channel_count(), &bus)) LOG_MAIN("[settings] 已清空 %d 个不可用或重复引脚，其余设置保留", cleared);
    IO_CONFIG = loaded;
}

/**
 * 读取并校验当前或历史设置记录
 * @param data 已填入当前默认值的候选设置
 * @param empty 返回存储是否真正为空
 * @return 当前格式或已发布历史格式完整校验成功时返回 true
 * 历史记录先按原长度和 CRC 验证，固定 Flash 只允许全 0xFF 尾部；失败不写入存储
 */
static bool load_settings_record(SettingsData &data, bool &empty) {
    const uint32_t stored_size = hal_storage_size(HAL_REGION_SETTINGS);
    empty = stored_size == 0;
    if (empty || stored_size == UINT32_MAX) return false;
    bool padded = false;
#if defined(ARDUINO_ARCH_RP2040)
    padded = true;
#endif
    if (!padded && stored_size > sizeof(SettingsData)) return false;
    const size_t length = min(static_cast<size_t>(stored_size), sizeof(SettingsData));
    uint8_t bytes[sizeof(SettingsData)];
    hal_storage_read(HAL_REGION_SETTINGS, bytes, static_cast<uint32_t>(length));
    if (padded) {
        empty = true;
        for (size_t i = 0; i < length; ++i) if (bytes[i] != 0xFF) { empty = false; break; }
        if (empty) return false;
    }
    if (length == sizeof(SettingsData)) {
        SettingsData current{};
        memcpy(&current, bytes, sizeof(current));
        if (current.magic == SETTINGS_MAGIC_NUMBER && calculate_crc32(&current) == current.checksum) {
            data = current;
            return true;
        }
    }
    return settings_migrate_record(bytes, length, data, padded);
}

/**
 * 初始化设置存储并迁移已发布历史格式
 * 有效记录只加载到运行期，旧记录保留到用户主动保存；损坏、未知格式或读取失败保留原始记录
 * 只有真正空片时保存默认值，并保留写入后的回读检查
 */
void settings_storage_init() {
    settings_reset_to_defaults();
    SettingsData data = settings_snapshot();
    bool empty = false;
    if (load_settings_record(data, empty)) {
        apply_settings_values(data);
        return;
    }
    if (!empty) {
        LOG_MAIN("[settings] 设置记录无效或无法迁移，使用运行期默认值，原始记录已保留");
        return;
    }
    LOG_MAIN("[settings] 存储为空，保存首次启动的默认设置");
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
 * 收集当前生效参数、网页项目名、全部 AMS 身份、霍尔检测、五通总微动和停止打印保护超时、局域网及接线配置；I2C 只存两根引脚，计算 checksum 后返回副本
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
    data.feed_stop_delay_ms = FEED_STOP_DELAY_MS;
    data.total_switch_active_level = TOTAL_SWITCH_ACTIVE_LEVEL;
    data.total_switch_feed_ms = TOTAL_SWITCH_FEED_MS;
    data.total_switch_pause_timeout_s = TOTAL_SWITCH_PAUSE_TIMEOUT_S;
    data.rs485_mode = RS485_MODE;
    data.bus_log_enable = BUS_LOG_ENABLE;
    data.ota_notify = OTA_NOTIFY;
    memset(data.custom_serial, 0, sizeof(data.custom_serial));
    for (int ams = 0; ams < AMS_IDENTITY_COUNT; ++ams)
        memcpy(data.custom_serial[ams], CUSTOM_SERIAL[ams], CUSTOM_SERIAL_LENGTH);
    memset(data.ota_url, 0, sizeof(data.ota_url));
    memcpy(data.ota_url, OTA_URL, OTA_URL_MAX - 1);
    data.magic = SETTINGS_MAGIC_NUMBER;
    data.motor_limit = MOTOR_LIMIT;
    data.io_config = IO_CONFIG;
    data.pcf_sda = PCF_SDA;
    data.pcf_scl = PCF_SCL;
    data.bus_rx = BUS_RX; data.bus_tx = BUS_TX; data.bus_de = BUS_DE;
    memcpy(data.ams_version, AMS_VERSION, sizeof(AMS_VERSION));
    memcpy(data.hall_detection, HALL_DETECTION, sizeof(HALL_DETECTION));
    data.lan_ams = LAN_AMS_CONFIG;
    if (PROJECT_NAME[0] && settings_project_name_valid(PROJECT_NAME))
        memcpy(data.project_name, PROJECT_NAME, strlen(PROJECT_NAME));
    else
        memcpy(data.project_name, DEFAULT_PROJECT_NAME, sizeof(DEFAULT_PROJECT_NAME));
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
 * 生成当前快照，复用候选设置保存接口进行 CRC 和回读检查
 */
bool settings_save_to_flash() {
    return settings_save_snapshot(settings_snapshot());
}

/**
 * 保存候选设置并完整回读校验
 * @param candidate 已完成范围和接线校验的候选设置
 * @return true 表示写入成功且回读完全一致
 * 校验全部霍尔检测、五通总微动、项目名和局域网参数并重算魔数和 CRC，不修改运行期设置，供项目切换在重启后统一生效
 */
bool settings_save_snapshot(const SettingsData &candidate) {
    if (!io_config_clutch_valid(candidate.io_config)) return false;
    if (candidate.ams_count < 1 || candidate.ams_count > IO_CONFIG_MAX_CHANNELS / 4) return false;
    const IoBusConfig bus{candidate.bus_rx, candidate.bus_tx, candidate.bus_de, candidate.rs485_mode,
                          candidate.pcf_sda, candidate.pcf_scl};
    const int channels = candidate.ams_count * 4;
    if ((bus.sda == PIN_NONE) != (bus.scl == PIN_NONE) ||
        !io_config_bus_validate(bus.rx, bus.tx, bus.de, bus.mode, bus.sda, bus.scl, candidate.io_config, channels) ||
        (bus.sda != PIN_NONE && !io_config_i2c_validate(bus.sda, bus.scl, candidate.io_config, channels, nullptr, &bus)) ||
        !io_config_validate(candidate.io_config, channels, nullptr, &bus)) return false;
    if ((candidate.total_switch_active_level != LOW && candidate.total_switch_active_level != HIGH) ||
        candidate.total_switch_feed_ms <= 0 || candidate.total_switch_feed_ms >= MAX_PULSE_MS) return false;
    if (candidate.total_switch_pause_timeout_s < MIN_TOTAL_SWITCH_PAUSE_TIMEOUT_S ||
        candidate.total_switch_pause_timeout_s > MAX_TOTAL_SWITCH_PAUSE_TIMEOUT_S) return false;
    if (!settings_project_name_valid(candidate.project_name)) return false;
    if (!settings_lan_config_valid(candidate.lan_ams)) return false;
    for (const auto &config : candidate.hall_detection)
        if (!settings_hall_detection_valid(config)) return false;
    SettingsData data = candidate;
    if (!data.project_name[0]) {
        memset(data.project_name, 0, sizeof(data.project_name));
        memcpy(data.project_name, DEFAULT_PROJECT_NAME, sizeof(DEFAULT_PROJECT_NAME));
    } else {
        const size_t name_length = strlen(data.project_name);
        memset(data.project_name + name_length, 0, sizeof(data.project_name) - name_length);
    }
    data.magic = SETTINGS_MAGIC_NUMBER;
    data.checksum = calculate_crc32(&data);
    if (!write_settings_to_flash(&data)) return false;
    SettingsData saved{};
    read_settings_from_flash(&saved);
    return saved.magic == SETTINGS_MAGIC_NUMBER &&
           calculate_crc32(&saved) == saved.checksum &&
           memcmp(&data, &saved, sizeof(data)) == 0;
}

/**
 * 恢复所有设置默认值
 * 接线和 AMS 数量复用平台默认项目；霍尔检测使用默认值，全部 AMS 使用自动序列号和默认版本，OTA 恢复默认发布目录
 * 清空局域网配对凭据并恢复默认换料参数；项目名恢复团子 BMCU，五通总微动恢复 LOW 压下、200ms 进料和 20 秒停止打印保护超时
 * 本函数不直接写存储
 */
void settings_reset_to_defaults() {
    for (int i = 0; i < 16; i++) {
        PULLBACK_TIMEOUT[i] = DEFAULT_PULLBACK_TIMEOUT;
        CHANNEL_MODE[i] = CHANNEL_MODE_AUTO;
        HALL_DETECTION[i] = DEFAULT_HALL_DETECTION;
    }
    AMS_COUNT = hal_default_project_preset().ams;
    MANUAL_PULSE_MS = DEFAULT_MANUAL_PULSE_MS;
    AUTO_LOAD_MS = DEFAULT_AUTO_LOAD_MS;
    FEED_STOP_DELAY_MS = DEFAULT_FEED_STOP_DELAY_MS;
    TOTAL_SWITCH_ACTIVE_LEVEL = DEFAULT_TOTAL_SWITCH_ACTIVE_LEVEL;
    TOTAL_SWITCH_FEED_MS = DEFAULT_TOTAL_SWITCH_FEED_MS;
    TOTAL_SWITCH_PAUSE_TIMEOUT_S = DEFAULT_TOTAL_SWITCH_PAUSE_TIMEOUT_S;
    memset(PROJECT_NAME, 0, sizeof(PROJECT_NAME));
    memcpy(PROJECT_NAME, DEFAULT_PROJECT_NAME, sizeof(DEFAULT_PROJECT_NAME));
    MOTOR_LIMIT = 1;
    RS485_MODE = hal_default_project_preset().bus.mode;
    BUS_LOG_ENABLE = DEFAULT_BUS_LOG;
    LAN_AMS_CONFIG = DEFAULT_LAN_AMS_CONFIG;
    memset(CUSTOM_SERIAL, 0, sizeof(CUSTOM_SERIAL));
    for (auto & ams : AMS_VERSION) {
        ams[0] = DEFAULT_AMS_VERSION_MAJOR;
        ams[1] = DEFAULT_AMS_VERSION_MINOR;
        ams[2] = DEFAULT_AMS_VERSION_PATCH;
        ams[3] = DEFAULT_AMS_VERSION_BUILD;
    }
    memset(OTA_URL, 0, sizeof(OTA_URL));
    memcpy(OTA_URL, DEFAULT_OTA_URL, sizeof(DEFAULT_OTA_URL));
    OTA_NOTIFY = DEFAULT_OTA_NOTIFY;
    io_config_set_defaults(IO_CONFIG);
    bus_defaults();
    pcf_defaults();
}

/**
 * 检查存储中的设置是否有效
 * @return 当前或已发布历史格式的长度、魔数及 CRC32 有效时返回 true
 * 只校验并在内存中解析，不改变运行期配置或持久化记录
 */
bool settings_is_valid() {
    SettingsData data = settings_snapshot();
    bool empty = false;
    return load_settings_record(data, empty);
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

/**
 * 打印当前全部设置
 * 启动和保存设置时，把总线、各 AMS 身份、通道接线、霍尔检测、五通总微动、局域网和 OTA 等参数写入主板日志，不输出访问码
 */
void settings_log_all() {
    LOG_MAIN("[settings] 进料停止延迟=%dms，五通总微动=%s，压下电平=%s，压下进料=%dms，持续压下停止打印超时=%ds", FEED_STOP_DELAY_MS,
             io_config_pin_label(IO_CONFIG.switch_pin).c_str(),
             TOTAL_SWITCH_ACTIVE_LEVEL == LOW ? "LOW" : "HIGH", TOTAL_SWITCH_FEED_MS, TOTAL_SWITCH_PAUSE_TIMEOUT_S);
    LOG_MAIN("[settings] RS485 收发模式=%d，BambuBus 原始报文日志=%s",
             RS485_MODE, BUS_LOG_ENABLE ? "已启用" : "已关闭");
    LOG_MAIN("[settings] BambuBus RX=%s，TX=%s，DE=%s（%s）", io_config_pin_label(BUS_RX).c_str(),
             io_config_pin_label(BUS_TX).c_str(), io_config_pin_label(BUS_DE).c_str(),
             io_config_bus_enabled() && RS485_MODE == RS485_MODE_DE ? "占用" : "不占用");
    LOG_MAIN("[settings] 通信方式=%s，局域网凭据=%s，送料=%lums，换料温度=%u℃",
             io_config_bus_enabled() ? "BambuBus" : "局域网", LAN_AMS_CONFIG.printer_ip[0] ? "已配置" : "未配置",
             static_cast<unsigned long>(LAN_AMS_CONFIG.feed_ms), static_cast<unsigned>(LAN_AMS_CONFIG.nozzle_temp));
    for (uint8_t ams = 0; ams < AMS_IDENTITY_COUNT; ++ams) {
        char serial[AMS_SERIAL_LENGTH + 1];
        settings_get_ams_serial(ams, serial);
        LOG_MAIN("[settings] AMS %d（%s）序列号=%s（%s），版本=%d.%d.%d.%d", ams + 1,
                 ams < AMS_COUNT ? "已启用" : "未启用", serial,
                 CUSTOM_SERIAL[ams][0] ? "自定义" : "自动",
                 AMS_VERSION[ams][0], AMS_VERSION[ams][1], AMS_VERSION[ams][2], AMS_VERSION[ams][3]);
    }
    LOG_MAIN("[settings] AMS 数量=%d，手动进退料点动时长=%dms", AMS_COUNT, MANUAL_PULSE_MS);
    LOG_MAIN("[settings] 电磁离合=%s，同时运行电机上限=%d", IO_CONFIG.clutch_enabled ? "已启用" : "已关闭",
             IO_CONFIG.clutch_enabled ? 1 : MOTOR_LIMIT);
    if (IO_CONFIG.clutch_enabled)
        LOG_MAIN("[settings] 共享电机进料=%s，退料=%s，离合闭合=%s，吸合=%dms，释放=%dms",
                 io_config_pin_label(IO_CONFIG.shared_motor[0]).c_str(), io_config_pin_label(IO_CONFIG.shared_motor[1]).c_str(),
                 IO_CONFIG.clutch_active_level == HIGH ? "HIGH" : "LOW",
                 static_cast<int>(IO_CONFIG.clutch_engage_ms), static_cast<int>(IO_CONFIG.clutch_release_ms));
    if (i2c_bus_configured())
        LOG_MAIN("[settings] I2C 总线已配置：SDA=IO%d，SCL=IO%d（PCF8575/温湿度模块共用）", PCF_SDA, PCF_SCL);
    else
        LOG_MAIN("[settings] I2C 总线未配置，PCF8575/温湿度模块不启用");
    for (int i = 0; i < active_channel_count(); i++) {
        if (IO_CONFIG.clutch_enabled)
            LOG_MAIN("[settings] 通道 %d IO=离合%s/微动%s/霍尔%s", i + 1,
                     io_config_pin_label(IO_CONFIG.clutch[i]).c_str(),
                     io_config_pin_label(IO_CONFIG.microswitch[i]).c_str(),
                     io_config_pin_label(IO_CONFIG.hall[i]).c_str());
        else LOG_MAIN("[settings] 通道 %d IO=进料%s/退料%s/微动%s/霍尔%s", i + 1,
                 io_config_pin_label(IO_CONFIG.motor[i][0]).c_str(),
                 io_config_pin_label(IO_CONFIG.motor[i][1]).c_str(),
                 io_config_pin_label(IO_CONFIG.microswitch[i]).c_str(),
                 io_config_pin_label(IO_CONFIG.hall[i]).c_str());
        LOG_MAIN("[settings] 通道 %d 霍尔下限=%u，上限=%u，超时=%lu 秒（0=关闭）", i + 1,
                 static_cast<unsigned>(HALL_DETECTION[i].lower), static_cast<unsigned>(HALL_DETECTION[i].upper),
                 static_cast<unsigned long>(HALL_DETECTION[i].timeout_s));
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
