#pragma once
#include <Arduino.h>
#include "io_config.h"

// ============================
// 设置存储管理器 - 存储后端见 platform.cpp（RP2040: 片内Flash / ESP32: NVS）
// ============================

// BambuBus 序列号中可自定义的后缀长度；完整序列号为 0EA + AMS 编号 + 12 位后缀
#define CUSTOM_SERIAL_LENGTH 12
constexpr int AMS_IDENTITY_COUNT = IO_CONFIG_MAX_CHANNELS / 4;
constexpr int AMS_SERIAL_LENGTH = 16;

// 通道状态模式定义
typedef enum {
    CHANNEL_MODE_OFFLINE = 0,    // 离线
    CHANNEL_MODE_ONLINE = 1,     // 在线
    CHANNEL_MODE_AUTO = 2         // 自动（根据微动开关）
} ChannelMode;

// 运行期设置变量
extern int PULLBACK_TIMEOUT[16];   // 16个电机的退料时间（秒）
extern int CHANNEL_MODE[16];       // 16个通道的状态模式
extern int AMS_COUNT;              // 唯一的数量设置（1-4），每个 AMS 对应 4 个通道
extern int MANUAL_PULSE_MS;        // 手动进退料点动时长（毫秒）
extern int AUTO_LOAD_MS;           // 微动辅助进料时长（毫秒，0=关闭）
extern int FEED_STOP_DELAY_MS;     // 打印机进料结束后的延迟停车时间（毫秒，0=立即停车）
extern int MOTOR_LIMIT;            // 同时运行的电机数（1..active_channel_count()）
extern int RS485_MODE;             // RS485 收发模式（见下方 RS485_MODE_*）
extern int BUS_LOG_ENABLE;         // BambuBus 原始报文日志开关（0=关 1=开）
extern char CUSTOM_SERIAL[AMS_IDENTITY_COUNT][CUSTOM_SERIAL_LENGTH + 1]; // 各 AMS 自定义后缀；空时按芯片 ID 和编号生成
extern int AMS_VERSION[AMS_IDENTITY_COUNT][4]; // 各 AMS 返回给打印机的版本，依次为 A.B.C.D，每段 0..255
extern char OTA_URL[128];          // OTA 更新源目录（设备会去取 <目录>/manifest.json）
extern int OTA_NOTIFY;             // 进页面时自动检查更新并提示（0=关 1=开）
extern IoConfig IO_CONFIG;          // 电机进退料与微动开关引脚

// 设置数据结构
struct SettingsData {
    int32_t pullback_timeout[16];    // 16个电机的退料时间（秒）
    int32_t channel_mode[16];       // 16个通道的状态模式（0=离线, 1=在线, 2=自动）
    int32_t ams_count;             // AMS数量（1-4）
    int32_t manual_pulse_ms;        // 手动进退料点动时长（毫秒）
    int32_t auto_load_ms;           // 微动辅助进料时长（毫秒，0=关闭）
    int32_t feed_stop_delay_ms;     // 打印机进料结束后的延迟停车时间（毫秒，0=立即停车）
    uint32_t checksum;              // 校验和
    uint32_t magic;                 // 魔数
    int32_t rs485_mode;              // RS485 收发模式
    int32_t bus_log_enable;          // BambuBus 原始报文日志开关
    char ota_url[128];               // OTA 更新源目录
    int32_t ota_notify;              // 自动检查更新并提示
    int32_t motor_limit;             // 当前格式的同时运行电机上限
    char custom_serial[AMS_IDENTITY_COUNT][CUSTOM_SERIAL_LENGTH]; // 各 AMS 自定义后缀，空表示自动生成
    IoConfig io_config;              // 当前平台的电机/微动引脚配置
    int32_t pcf_sda;                 // I2C SDA 引脚（PCF8575/温湿度模块共用），PIN_NONE 表示未配置
    int32_t pcf_scl;                 // I2C SCL 引脚（PCF8575/温湿度模块共用），PIN_NONE 表示未配置
    int32_t bus_rx;                  // BambuBus 串口 RX 引脚
    int32_t bus_tx;                  // BambuBus 串口 TX 引脚
    int32_t bus_de;                  // RS485 DE 方向控制引脚；RXTX 模式下不使用
    int32_t ams_version[AMS_IDENTITY_COUNT][4]; // 各 AMS 版本（A.B.C.D，每段 0..255）
};

// 当前布局包含四份 AMS 身份、进料停止延迟及五通总微动引脚；旧布局均恢复整份默认设置
#define SETTINGS_MAGIC_NUMBER    0x5341542D

// 当前启用数量；数组容量使用 IO_CONFIG_MAX_CHANNELS
/**
 * 获取当前启用通道数
 * @return AMS 数量乘以 4
 * 所有运动逻辑和界面都以此作为有效通道上限
 */
inline int active_channel_count() { return AMS_COUNT * 4; }

// 默认值定义
#define DEFAULT_PULLBACK_TIMEOUT    7    // 退料超时默认值（秒）
constexpr int DEFAULT_AMS_COUNT = 1; // 首次启动、恢复默认和无效设置的回退值
#define DEFAULT_MANUAL_PULSE_MS     1000  // 手动点动默认时长（毫秒）
#define DEFAULT_AUTO_LOAD_MS        5000 // 微动辅助进料默认时长（毫秒）
#define DEFAULT_FEED_STOP_DELAY_MS  1000 // 进料停止延迟默认时长（毫秒）
#define DEFAULT_AMS_VERSION_MAJOR   1    // 默认 AMS 版本 A 段
#define DEFAULT_AMS_VERSION_MINOR   0    // 默认 AMS 版本 B 段
#define DEFAULT_AMS_VERSION_PATCH   6    // 默认 AMS 版本 C 段
#define DEFAULT_AMS_VERSION_BUILD   87   // 默认 AMS 版本 D 段

// 点动、辅助进料和进料停止延迟的时长上限（毫秒）：必须小于 30 秒
#define MAX_PULSE_MS                30000

#define RS485_MODE_RXTX             1   // 只用 RXD/TXD，不驱动 DE（模块自动收发方向）
#define RS485_MODE_DE               2   // RO/DI/RE/DE 四线，由配置的 BUS_DE 控制方向
#define DEFAULT_RS485_MODE          RS485_MODE_RXTX // RS485 默认收发模式
#define DEFAULT_BUS_LOG             1   // 默认开启 BambuBus 原始报文日志

// OTA 更新源
#define OTA_URL_MAX                 128 // OTA 更新源地址缓冲区长度，包含结尾 NUL
#define DEFAULT_OTA_URL             "https://tsimplez.github.io/bmcu-esp" // 默认 OTA 发布目录
#define DEFAULT_OTA_NOTIFY          1   // 默认进入页面时自动检查更新

// 初始化设置存储（在setup中调用）
void settings_storage_init();

// 从Flash加载设置到变量
void settings_load_from_flash();

// 保存失败时使用内存快照回退，避免重新读取可能不完整的 Flash
SettingsData settings_snapshot();
void settings_restore_snapshot(const SettingsData &data);

// 保存当前设置到Flash
bool settings_save_to_flash();

// 保存已校验的候选设置并重算 CRC、回读验证；不改变运行期配置
bool settings_save_snapshot(const SettingsData &candidate);

// 重置为默认值（不自动保存，需手动调用save）
void settings_reset_to_defaults();

// 检查设置是否有效（校验和验证）
bool settings_is_valid();

// 把当前生效的设置打印到主板日志（启动时、保存设置时各调一次）
void settings_log_all();

// 空字符串表示自动生成后缀；非空时必须为 12 位大写十六进制字符
bool settings_custom_serial_valid(const char *serial);

/**
 * 读取指定 AMS 实际发送的完整序列号
 * @param ams 协议 AMS 编号，范围 0..3，与当前启用数量无关
 * @param out 输出 16 位序列号和结尾 NUL，至少容纳 AMS_SERIAL_LENGTH + 1 字节
 * @return 编号及输出缓冲有效时返回 true，否则返回 false 并清空非空输出缓冲
 * 自动后缀取芯片 ID 的低 48 位十六进制值，并把末位替换为 AMS 编号；自定义后缀原样保留
 * 本函数只读取配置，不刷新注册状态
 */
bool settings_get_ams_serial(uint8_t ams, char out[AMS_SERIAL_LENGTH + 1]);

/**
 * 解析 A.B.C.D 格式的 AMS 版本
 * @param text 待解析文本，例如 "1.0.6.87"
 * @param out 输出四个版本分量
 * @return true 表示格式正确且每段均在 0..255
 * 仅在完整解析成功后写入 out，供网页保存前统一校验
 */
bool settings_parse_ams_version(const char *text, int out[4]);

// 获取指定电机的退料超时时间（新增辅助函数）
int get_motor_pullback_timeout(int motor_index);
