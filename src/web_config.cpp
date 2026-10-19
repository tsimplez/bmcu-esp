#include "web_config.h"

#if defined(ARDUINO_ARCH_ESP32)

#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <Update.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>

#include "logbus.h"
#include "platform.h"
#include "settings_storage.h"
#include "BMCU.h"
#include "version.h"
#include "i2c_bus.h"
#include "env_sensor.h"
#include "PCF8575_PinMap.h"

// ============================================================
// 常量
// ============================================================
#define AP_SSID_PREFIX  "BMCU"     // 配网热点名前缀
#define AP_PASS         "12345678" // 配网热点密码（至少 8 位）
#define WIFI_NS         "wifi"     // NVS 中 WiFi 凭据的命名空间
#define DEFAULT_MDNS_NAME "bmcu"   // 默认 mDNS 主机名
#define MDNS_NAME_MAX   32         // mDNS 主机名最大长度
#define STA_TIMEOUT_MS  15000      // 连接路由器超时（毫秒）
#define LOG_PULL_MAX    16384      // 单次拉取日志的最大字节数

// ESP32 内置 HTTP 服务器，监听 80 端口并提供状态、设置、日志和 OTA 接口
static WebServer   server(80);
// 配网热点模式下的 DNS 服务，把所有域名解析到设备自身以触发配置页
static DNSServer   dns;
// NVS 访问对象，保存 STA 的 SSID、密码和 mDNS 名称
static Preferences wprefs;

enum { ST_IDLE = 0, ST_CONNECTING };
// WiFi 连接状态：ST_IDLE=空闲/已连接，ST_CONNECTING=正在连接保存的路由器
static int      wifi_state = ST_IDLE;
// 本次 STA 连接的超时截止时间（millis），超过后启动配网热点
static uint32_t connect_deadline = 0;
// 当前是否运行在 AP 配网模式
static bool     ap_mode = false;
// 已保存的路由器 SSID（最多 63 个可见字符 + NUL）
static char     sta_ssid[64] = {};
// 已保存的路由器密码
static char     sta_pass[64] = {};
// 当前 mDNS 主机名（不含 .local 后缀）
static char     mdns_name[MDNS_NAME_MAX + 1] = DEFAULT_MDNS_NAME;
// 根据芯片 ID 生成的配网热点名，例如 BMCU_1A2B3C
static char     ap_ssid[16] = {};

// ============================================================
// 页面
// ============================================================
// 页面本体在 web/index.html，由 PlatformIO 的 board_build.embed_txtfiles 编进固件，
// 改完 html 直接 pio run -t upload 即可，不需要单独烧文件系统
extern const uint8_t web_index_html_start[] asm("_binary_web_index_html_start");
extern const uint8_t web_index_html_end[] asm("_binary_web_index_html_end");
#define PAGE_HTML ((const char *)web_index_html_start)

// embed_txtfiles 会在末尾补一个 '\0'，发给浏览器时要去掉，否则 HTML 尾巴上会多一个空字节
/**
 * 获取嵌入网页的长度
 * @return 不包含末尾 NUL 的 HTML 字节数
 * PlatformIO 嵌入文件时会在末尾补 0，发送给浏览器前需要去掉。
 */
static size_t page_html_len() {
    size_t n = (size_t)(web_index_html_end - web_index_html_start);
    if (n && web_index_html_start[n - 1] == 0) n--;
    return n;
}
// ============================================================
// 小工具
// ============================================================
/**
 * 安全复制字符串到固定缓冲区
 * @param dst 目标缓冲区
 * @param cap 目标容量
 * @param src 源字符串
 * 按容量截断并保证以 NUL 结尾，源指针为空时写入空串。
 */
static void copy_str(char *dst, size_t cap, const char *src) {
    if (cap == 0) return;
    size_t n = src ? strlen(src) : 0;
    if (n >= cap) n = cap - 1;
    if (n) memcpy(dst, src, n);
    dst[n] = 0;
}

/**
 * 转义 JSON 字符串内容
 * @param s 原始字符串
 * @return 转义后的字符串
 * 处理双引号和反斜杠，并把控制字符替换为空格，避免破坏 JSON。
 */
static String json_escape(const char *s) {
    String out;
    for (; s && *s; s++) {
        char c = *s;
        if (c == '"' || c == '\\') { out += '\\'; out += c; }
        else if ((uint8_t)c < 0x20) out += ' ';
        else out += c;
    }
    return out;
}

/**
 * 校验 mDNS 主机名格式
 * @param name 待校验的主机名
 * @return true 表示名称合法
 * 只允许 1-32 位字母、数字和连字符，且首尾不能是连字符。
 */
static bool mdns_name_valid(const char *name) {
    if (!name) return false;
    const size_t length = strlen(name);
    if (length == 0 || length > MDNS_NAME_MAX) return false;
    if (name[0] == '-' || name[length - 1] == '-') return false;
    for (size_t i = 0; i < length; i++) {
        const char c = name[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '-')) return false;
    }
    return true;
}

/**
 * 规范化 mDNS 主机名
 * @param raw 用户输入或存储中的原始名称
 * @param out 输出的规范名称
 * @return true 表示名称合法
 * 去除空白和可选的 .local 后缀并转为小写，再用 mdns_name_valid() 校验。
 */
static bool normalize_mdns_name(const String &raw, String &out) {
    String name = raw;
    name.trim();
    name.toLowerCase();
    if (name.endsWith(".local")) name.remove(name.length() - 6);
    name.trim();
    if (!mdns_name_valid(name.c_str())) return false;
    out = name;
    return true;
}

/**
 * 生成带芯片唯一 ID 的配网热点名
 * @return 指向静态 SSID 缓冲区的指针
 * 取芯片 64 位唯一 ID 的低 24 位格式化为 6 位大写十六进制，拼成 BMCU_XXXXXX。
 */
static const char *build_ap_ssid() {
    const uint32_t suffix = static_cast<uint32_t>(hal_get_device_id() & 0xFFFFFFULL);
    snprintf(ap_ssid, sizeof(ap_ssid), "%s_%06lX", AP_SSID_PREFIX,
             static_cast<unsigned long>(suffix));
    return ap_ssid;
}

// ============================================================
// WiFi 凭据（NVS 独立命名空间 "wifi"）
// ============================================================
/**
 * 读取已保存的 WiFi 凭据
 * 从 NVS 的 wifi 命名空间读取 SSID、密码和 mDNS 名称到内存；无有效名称时使用默认值。
 */
static void load_wifi_creds() {
    sta_ssid[0] = sta_pass[0] = 0;
    copy_str(mdns_name, sizeof(mdns_name), DEFAULT_MDNS_NAME);
    if (!wprefs.begin(WIFI_NS, true)) return;
    wprefs.getString("ssid", sta_ssid, sizeof(sta_ssid));
    wprefs.getString("pass", sta_pass, sizeof(sta_pass));
    char stored_mdns[MDNS_NAME_MAX + 1] = {};
    wprefs.getString("mdns", stored_mdns, sizeof(stored_mdns));
    wprefs.end();

    String normalized;
    if (normalize_mdns_name(String(stored_mdns), normalized))
        copy_str(mdns_name, sizeof(mdns_name), normalized.c_str());
}

/**
 * 保存 WiFi 凭据
 * @param ssid WiFi 名称
 * @param pass WiFi 密码
 * 写入 NVS 并同步更新内存中的 SSID/密码。
 */
static void save_wifi_creds(const char *ssid, const char *pass) {
    if (wprefs.begin(WIFI_NS, false)) {
        wprefs.putString("ssid", ssid);
        wprefs.putString("pass", pass);
        wprefs.end();
    }
    copy_str(sta_ssid, sizeof(sta_ssid), ssid);
    copy_str(sta_pass, sizeof(sta_pass), pass);
}

/**
 * 保存 mDNS 名称
 * @param name 已规范化的 mDNS 主机名
 * @return true 表示写入成功
 * 把名称写入 NVS 的 wifi 命名空间，成功后同步更新内存中的名称。
 */
static bool save_mdns_name(const String &name) {
    bool saved = false;
    if (wprefs.begin(WIFI_NS, false)) {
        saved = wprefs.putString("mdns", name) == name.length();
        wprefs.end();
    }
    if (saved) copy_str(mdns_name, sizeof(mdns_name), name.c_str());
    return saved;
}

/**
 * 清除 WiFi 凭据
 * 清空 NVS 的 wifi 命名空间和内存中的 SSID/密码。
 */
static void forget_wifi_creds() {
    if (wprefs.begin(WIFI_NS, false)) {
        wprefs.clear();
        wprefs.end();
    }
    sta_ssid[0] = sta_pass[0] = 0;
    copy_str(mdns_name, sizeof(mdns_name), DEFAULT_MDNS_NAME);
}

// ============================================================
// WiFi 状态机
// ============================================================
// 切换 WiFi 模式（模式确定后 WiFi 协议栈才算初始化，此时才能关省电）
/**
 * 切换 WiFi 工作模式
 * @param m WiFi 模式
 * 设置模式后关闭 WiFi 省电，保证网页和串口响应及时。
 */
static void wifi_set_mode(wifi_mode_t m) {
    WiFi.mode(m);
    WiFi.setSleep(false);
}

/**
 * 启动配网热点
 * 创建固定网段的 AP，并把 DNS 通配请求指向自身，实现强制门户。
 */
static void start_config_ap() {
    wifi_set_mode(WIFI_AP_STA);
    const char *ssid = build_ap_ssid();
    // 固定 AP 网段，并把网关/DNS 都指向自己，客户端才能被强制门户接管
    WiFi.softAPConfig(IPAddress(192, 168, 4, 1), IPAddress(192, 168, 4, 1), IPAddress(255, 255, 255, 0));
    WiFi.softAP(ssid, AP_PASS);
    dns.start(53, "*", WiFi.softAPIP());
    ap_mode = true;
    wifi_state = ST_IDLE;
    LOG_MAIN("[web] 未连接 WiFi，配网热点 '%s' 已开启：连上后访问 http://%s/",
             ssid, WiFi.softAPIP().toString().c_str());
}

/**
 * 启动或重载 mDNS 服务
 * @return true 表示 mDNS 启动成功
 * 使用当前 mdns_name 重启 mDNS 并发布 HTTP 服务，供 http://<名称>.local/ 访问。
 */
static bool start_mdns() {
    MDNS.end();
    if (!MDNS.begin(mdns_name)) {
        LOG_MAIN("[web] mDNS 启动失败：%s.local", mdns_name);
        return false;
    }
    MDNS.addService("http", "tcp", 80);
    LOG_MAIN("[web] mDNS 已启动：http://%s.local/", mdns_name);
    return true;
}

/**
 * 处理 STA 连接成功
 * 退出连接状态、关闭 AP 模式并按当前配置启动 mDNS 服务。
 */
static void on_sta_connected() {
    wifi_state = ST_IDLE;
    ap_mode = false;
    start_mdns();
    LOG_MAIN("[web] 已连接 WiFi '%s'：http://%s/ 或 http://%s.local/",
             sta_ssid, WiFi.localIP().toString().c_str(), mdns_name);
}

// ============================================================
// REST API
// ============================================================
/**
 * 检查是否允许维护操作
 * @return true 表示当前可以执行配置、重启或升级
 * 调用 BMCU_can_maintain()；不满足时直接向客户端发送 409 响应。
 */
static bool maintenance_required() {
    if (BMCU_can_maintain()) return true;
    server.send(409, "application/json",
                "{\"ok\":false,\"msg\":\"请先停止所有电机运动\"}");
    return false;
}

/**
 * 处理首页请求
 * 禁用浏览器缓存并发送嵌入固件的 HTML 管理页面。
 */
static void handle_root() {
    // 页面必须和固件配套：禁掉缓存，避免升级后浏览器仍用旧页面（旧页面不认新字段）。
    server.sendHeader("Cache-Control", "no-store");
    server.send_P(200, "text/html; charset=utf-8", (PGM_P)PAGE_HTML, page_html_len());
}

/**
 * 把传感器读数格式化为 JSON 数字
 * @param valid 读数是否有效
 * @param value 读数值
 * @param digits 小数位数
 * @return JSON 数字文本；无效时返回 null
 * 网页据此区分“未检测/读取失败”和真实的 0 值。
 */
static String sensor_json_value(bool valid, float value, int digits) {
    return valid ? String(value, digits) : String("null");
}

/**
 * 处理运行状态查询
 * 汇总 WiFi、IP、堆内存、总线、电机、耗材、I2C/温湿度、维护状态和版本信息为 JSON。
 */
static void handle_status() {
    IPAddress ip = ap_mode ? WiFi.softAPIP() : WiFi.localIP();

    String j;
    j.reserve(2200);
    j  = "{\"mode\":\"" + String(ap_mode ? "AP" : "STA") + "\"";
    j += ",\"ip\":\"" + ip.toString() + "\"";
    j += ",\"ssid\":\"" + json_escape(ap_mode ? "" : sta_ssid) + "\"";
    j += ",\"mdns\":\"" + json_escape(mdns_name) + "\"";
    j += ",\"rssi\":" + String(ap_mode ? 0 : WiFi.RSSI());
    j += ",\"mac\":\"" + WiFi.macAddress() + "\"";
    j += ",\"uptime\":" + String(millis() / 1000);
    j += ",\"heap\":" + String(ESP.getFreeHeap());
    j += ",\"heap_total\":" + String(ESP.getHeapSize());
    j += ",\"chip\":\"" + String(ESP.getChipModel()) + "\"";
    j += ",\"rs485\":" + String(RS485_MODE);
    j += ",\"buslog\":" + String(BUS_LOG_ENABLE);
    j += ",\"ams\":" + String(AMS_COUNT);
    j += ",\"channels\":" + String(BMCU_motor_count());
    j += ",\"maxams\":" + String(BMCU_max_ams_count());
    j += ",\"printer\":" + String(BambuBus_printer_status());
    j += ",\"bus_fault\":" + String(BMCU_printer_bus_fault() ? "true" : "false");
    j += ",\"bus_rx_errors\":" + String(BambuBus_rx_error_count());
    j += ",\"bus_rx_dropped\":" + String(BambuBus_rx_dropped_count());
    const uint32_t packet_age = BambuBus_last_valid_packet_age_ms();
    j += ",\"bus_last_valid_age_ms\":" +
         String(packet_age == UINT32_MAX ? -1 : static_cast<int32_t>(packet_age));
    j += ",\"motor_limit\":" + String(MOTOR_LIMIT);
    j += ",\"motor_running\":" + String(BMCU_running_count());
    j += ",\"motors\":[";
    for (int i = 0; i < BMCU_motor_count(); i++) {
        if (i) j += ',';
        auto m = BMCU_motor_status(i);
        j += "{\"dir\":" + String(m.direction) + ",\"source\":" + String((int)m.source) +
             ",\"remaining_ms\":" + String(m.remaining_ms) + ",\"blocked\":" +
             String(m.blocked ? "true" : "false") + ",\"queue_position\":" + String(m.queue_position) +
             ",\"queued_dir\":" + String(m.queued_direction) + "}";
    }
    j += ']';
    // 每个通道的微动开关状态：1=有料 0=无料 -1=该通道没有微动开关
    j += ",\"fl\":[";
    for (int i = 0; i < BMCU_motor_count(); i++) {
        if (i) j += ',';
        j += String(BMCU_channel_filament_state(i));
    }
    j += "]";
    j += ",\"maintenance\":" + String(BMCU_can_maintain() ? "true" : "false");
    j += ",\"manual\":" + String(BMCU_manual_active() ? "true" : "false");
    j += ",\"cli\":" + String(WiFi.softAPgetStationNum());
    j += ",\"version\":\"" + String(FW_VERSION) + "\"";
    j += ",\"target\":\"" + String(FW_TARGET) + "\"";
    j += ",\"build\":\"" + String(FW_BUILD) + "\"";
    j += ",\"otaurl\":\"" + json_escape(OTA_URL) + "\"";
    j += ",\"otanotify\":" + String(OTA_NOTIFY);
    j += ",\"otasize\":" + String(ESP.getSketchSize());
    const EnvSensorStatus &env = env_sensor_status();
    j += ",\"i2c\":{\"configured\":" + String(i2c_bus_configured() ? "true" : "false") +
         ",\"pcf\":" + String(PCF8575_is_online() ? "true" : "false") +
         ",\"aht20\":" + String(env.aht20_online ? "true" : "false") +
         ",\"bmp280\":" + String(env.bmp280_online ? "true" : "false") + "}";
    j += ",\"env\":{\"aht_temp\":" + sensor_json_value(env.aht20_online && env.aht20_valid, env.temperature_c, 2) +
         ",\"humidity\":" + sensor_json_value(env.aht20_online && env.aht20_valid, env.humidity_pct, 2) +
         ",\"bmp_temp\":" + sensor_json_value(env.bmp280_online && env.bmp280_valid, env.bmp_temperature_c, 2) +
         ",\"pressure\":" + sensor_json_value(env.bmp280_online && env.bmp280_valid, env.pressure_hpa, 2) + "}";
    j += "}";
    server.send(200, "application/json", j);
}

/**
 * 处理设置读取请求
 * 输出参数、通道模式、退料超时、IO/PCF/总线接线和可选引脚 JSON。
 */
static void handle_settings_get() {
    String j = "{\"rs485\":" + String(RS485_MODE) + ",\"buslog\":" + String(BUS_LOG_ENABLE) +
               ",\"ams\":" + String(AMS_COUNT) + ",\"manual_ms\":" + String(MANUAL_PULSE_MS) +
               ",\"auto_ms\":" + String(AUTO_LOAD_MS) +
               ",\"motor_limit\":" + String(MOTOR_LIMIT) +
               ",\"channels\":" + String(BMCU_motor_count()) +
               ",\"maxams\":" + String(BMCU_max_ams_count()) +
               ",\"serial\":\"" + json_escape(CUSTOM_SERIAL) + "\",\"to\":[";
    for (int i = 0; i < BMCU_motor_count(); i++) { if (i) j += ','; j += String(PULLBACK_TIMEOUT[i]); }
    j += "],\"cm\":[";
    for (int i = 0; i < BMCU_motor_count(); i++) { if (i) j += ','; j += String(CHANNEL_MODE[i]); }
    j += "],\"otaurl\":\"" + json_escape(OTA_URL) + "\",\"otanotify\":" + String(OTA_NOTIFY);
    j += ",\"io\":{\"motor\":[";
    for (int i = 0; i < IO_CONFIG_MAX_CHANNELS; i++) {
        if (i) j += ',';
        j += "[" + String(IO_CONFIG.motor[i][0]) + "," + String(IO_CONFIG.motor[i][1]) + "]";
    }
    j += "],\"microswitch\":[";
    for (int i = 0; i < IO_CONFIG_MAX_CHANNELS; i++) {
        if (i) j += ',';
        j += String(IO_CONFIG.microswitch[i]);
    }
    j += "],\"pins\":";
    io_config_append_pin_options(j);
    j += "},\"pcf\":{\"enabled\":" + String(i2c_bus_configured() ? "true" : "false") +
         ",\"configured\":" + String(i2c_bus_configured() ? "true" : "false") +
         ",\"sda\":" + String(PCF_SDA) + ",\"scl\":" + String(PCF_SCL) + ",\"pins\":";
    io_config_append_pin_options(j, true);
    j += "},\"bus\":{\"rx\":" + String(BUS_RX) + ",\"tx\":" + String(BUS_TX) +
         ",\"de\":" + String(BUS_DE) + ",\"pins\":";
    io_config_append_pin_options(j, true);
    j += "}}";
    server.sendHeader("Cache-Control", "no-store");
    server.send(200, "application/json", j);
}

// toInt() 会把非法字符串当作 0；修改参数前先完整校验，避免部分保存。
/**
 * 校验请求参数是否为范围内整数
 * @param key 参数名
 * @param low 允许的最小值
 * @param high 允许的最大值
 * @return true 表示参数缺失或数值合法
 * 先否定 toInt() 的容错行为，避免非法字符串被当作 0 保存。
 */
static bool valid_number(const String &key, int low, int high) {
    if (!server.hasArg(key)) return true;
    String raw = server.arg(key);
    char *end;
    long value = strtol(raw.c_str(), &end, 10);
    return end != raw.c_str() && !*end && value >= low && value <= high;
}

/**
 * 解析并校验引脚参数
 * @param key 参数名
 * @param value 输出的引脚编号
 * @param allow_none 是否允许 PIN_NONE
 * @return true 表示参数存在且引脚可用
 * 把请求中的十进制引脚号解析为整数并调用 io_config_pin_available()。
 */
static bool parse_io_pin(const String &key, int &value, bool allow_none) {
    if (!server.hasArg(key)) return false;
    String raw = server.arg(key);
    char *end = nullptr;
    long parsed = strtol(raw.c_str(), &end, 10);
    if (end == raw.c_str() || *end || parsed < 0 || parsed > 255) return false;
    value = static_cast<int>(parsed);
    return io_config_pin_available(value, allow_none);
}

// I2C 总线配置：SDA/SCL 同时有效才启用，同时留空表示关闭；保存后重启生效。
/**
 * 解析 I2C 引脚参数
 * @param key 请求参数名
 * @param value 输出引脚号，PIN_NONE 表示“无”
 * @return true 表示参数存在且格式合法
 * 接受 0-255 的数字或 none/255（表示未配置）。
 */
static bool parse_i2c_pin(const String &key, int &value) {
    if (!server.hasArg(key)) return false;
    String raw = server.arg(key);
    raw.trim();
    if (raw.equalsIgnoreCase("none") || raw == "255") {
        value = PIN_NONE;
        return true;
    }
    char *end = nullptr;
    const long parsed = strtol(raw.c_str(), &end, 10);
    if (end == raw.c_str() || *end || parsed < 0 || parsed > 255) return false;
    value = static_cast<int>(parsed);
    return true;
}

/**
 * 处理 I2C 总线配置保存
 * 校验 SDA/SCL 必须同时配置或同时留空，清理冲突通道引脚，写入设置并重启。
 */
static void handle_pcf_save() {
    int sda = PIN_NONE;
    int scl = PIN_NONE;
    if (!parse_i2c_pin("sda", sda) || !parse_i2c_pin("scl", scl)) {
        server.send(400, "application/json", "{\"ok\":false,\"msg\":\"请选择有效的 SDA 和 SCL 引脚\"}");
        return;
    }
    if ((sda == PIN_NONE) != (scl == PIN_NONE)) {
        server.send(400, "application/json",
                    "{\"ok\":false,\"msg\":\"SDA 和 SCL 必须同时配置，或同时选择“无”关闭总线\"}");
        return;
    }
    const bool configured = sda != PIN_NONE;
    if (!maintenance_required()) return;
    const SettingsData previous = settings_snapshot();
    PCF_SDA = sda;
    PCF_SCL = scl;
    // 只清掉在新状态下不可用的引脚：配置时是与 SDA/SCL 冲突的，关闭时是扩展脚，
    // 其余接线保留，用户已有接线不会被整套冲掉。
    const int cleared = io_config_clear_invalid_pins(IO_CONFIG, configured, PCF_SDA, PCF_SCL);
    String error;
    bool wiring_ok = !configured ||
        (io_config_i2c_validate(PCF_SDA, PCF_SCL, IO_CONFIG, active_channel_count(), &error) &&
         io_config_bus_validate(BUS_RX, BUS_TX, BUS_DE, RS485_MODE, PCF_SDA, PCF_SCL,
                                IO_CONFIG, active_channel_count(), &error));
    if (wiring_ok) wiring_ok = io_config_validate(IO_CONFIG, active_channel_count(), &error);
    if (!wiring_ok) {
        settings_restore_snapshot(previous);
        server.send(400, "application/json",
                    String("{\"ok\":false,\"msg\":\"") + json_escape(error.c_str()) + "\"}");
        return;
    }
    if (!settings_save_to_flash()) {
        settings_restore_snapshot(previous);
        server.send(500, "application/json", "{\"ok\":false,\"msg\":\"保存或回读校验失败，未切换接线，请重新保存\"}");
        return;
    }
    String msg = configured ? "I2C 总线已配置，设备正在重启"
                            : "I2C 总线已关闭，设备正在重启";
    if (cleared)
        msg += "；已清空 " + String(cleared) + " 个不再可用的引脚，请到「电机与微动引脚」重新配置";
    else if (configured)
        msg += "；请断电后按新引脚接线";
    if (configured)
        LOG_MAIN("[web] I2C 总线已配置：SDA=IO%d，SCL=IO%d，清空引脚数=%d，重启生效",
                 PCF_SDA, PCF_SCL, cleared);
    else
        LOG_MAIN("[web] I2C 总线已关闭，清空引脚数=%d，重启生效", cleared);
    server.send(200, "application/json",
                String("{\"ok\":true,\"msg\":\"") + json_escape(msg.c_str()) + "\"}");
    delay(300);
    ESP.restart();
}

/**
 * 处理 BambuBus 总线接线保存
 * 校验 RS485 模式与 RX/TX/DE 引脚，写入设置后重启；UART 重启前继续使用旧接线。
 */
static void handle_bus_save() {
    if (!server.hasArg("rx") || !server.hasArg("tx") || !server.hasArg("mode") ||
        !valid_number("rx", 0, 48) || !valid_number("tx", 0, 48) || !valid_number("mode", 1, 2) ||
        (server.arg("mode").toInt() == RS485_MODE_DE &&
         (!server.hasArg("de") || !valid_number("de", 0, 48)))) {
        server.send(400, "application/json", "{\"ok\":false,\"msg\":\"请完整填写总线模式与接线，模式 2 必须指定 DE\"}");
        return;
    }
    const int rx = server.arg("rx").toInt(), tx = server.arg("tx").toInt();
    const int mode = server.arg("mode").toInt();
    const int de = mode == RS485_MODE_DE ? server.arg("de").toInt() : BUS_DE;
    String error;
    if (!io_config_bus_validate(rx, tx, de, mode, PCF_SDA, PCF_SCL, IO_CONFIG, active_channel_count(), &error)) {
        server.send(400, "application/json", String("{\"ok\":false,\"msg\":\"") + json_escape(error.c_str()) + "\"}");
        return;
    }
    if (!maintenance_required()) return;
    const SettingsData previous = settings_snapshot();
    BUS_RX = rx; BUS_TX = tx; BUS_DE = de; RS485_MODE = mode;
    const bool saved = settings_save_to_flash();
    // UART 仍使用旧接线；重启前保持运行状态与硬件一致。
    settings_restore_snapshot(previous);
    if (!saved) {
        server.send(500, "application/json", "{\"ok\":false,\"msg\":\"保存或回读校验失败，未切换接线，请重试\"}");
        return;
    }
    LOG_MAIN("[web] BambuBus 接线已保存：模式=%d RX=IO%d TX=IO%d DE=IO%d，重启生效", mode, rx, tx, de);
    server.send(200, "application/json", "{\"ok\":true,\"msg\":\"BambuBus 配置已保存，设备正在重启；请断电后按新引脚接线\"}");
    delay(300);
    ESP.restart();
}

/**
 * 处理常规设置保存
 * 统一校验 AMS 数量、时长、并发上限、序列号、通道参数、IO 接线和 OTA 配置；保存失败时回滚内存快照。
 */
static void handle_settings_save() {
    if (server.hasArg("rs485")) {
        server.send(400, "application/json", "{\"ok\":false,\"msg\":\"请刷新页面，在 BambuBus 接线中同时保存模式与引脚并重启\"}");
        return;
    }
    int buslog_before = BUS_LOG_ENABLE;
    const SettingsData previous = settings_snapshot();
    const bool ams_valid = valid_number("ams", 1, BMCU_max_ams_count());
    const int candidate_ams = ams_valid && server.hasArg("ams") ? server.arg("ams").toInt() : AMS_COUNT;
    const int candidate_channels = candidate_ams * 4;
    const bool count_changed = candidate_ams != AMS_COUNT;
    bool valid = ams_valid &&
                 valid_number("manual_ms", 1, MAX_PULSE_MS - 1) &&
                 valid_number("auto_ms", 0, MAX_PULSE_MS - 1) &&
                 valid_number("motor_limit", 1, candidate_channels);
    if (server.hasArg("serial"))
        valid = valid && settings_custom_serial_valid(server.arg("serial").c_str());
    for (int i = 0; i < candidate_channels; i++)
        valid = valid && valid_number("to" + String(i), 3, 30) && valid_number("cm" + String(i), 0, 2);

    bool io_requested = false;
    bool io_fields_valid = true;
    // 先识别完整请求，不能在逐通道解析过程中才发现 IO 字段。
    // 同时拒绝旧页面的 io-* 拼写和未知字段，避免忽略修改却返回保存成功。
    for (int arg = 0; arg < server.args(); arg++) {
        const String key = server.argName(arg);
        if (!key.startsWith("io_") && !key.startsWith("io-")) continue;
        io_requested = true;
        bool known = false;
        for (int ch = 0; ch < candidate_channels; ch++) {
            if (key == "io_mf" + String(ch) || key == "io_mb" + String(ch) ||
                key == "io_sw" + String(ch)) known = true;
        }
        if (!known) io_fields_valid = false;
    }
    IoConfig candidate_io = IO_CONFIG;
    for (int i = 0; i < candidate_channels; i++) {
        const String k_forward = "io_mf" + String(i);
        const String k_reverse = "io_mb" + String(i);
        const String k_switch = "io_sw" + String(i);
        int pin = 0;
        // 电机引脚也允许选「无」：切换 PCF8575 后被清空的通道可以先留空，
        // 该通道只是暂时不能进退料，接好线再来补。
        if (server.hasArg(k_forward) && parse_io_pin(k_forward, pin, true)) candidate_io.motor[i][0] = pin;
        else if (server.hasArg(k_forward)) valid = false;
        if (server.hasArg(k_reverse) && parse_io_pin(k_reverse, pin, true)) candidate_io.motor[i][1] = pin;
        else if (server.hasArg(k_reverse)) valid = false;
        if (server.hasArg(k_switch) && parse_io_pin(k_switch, pin, true)) candidate_io.microswitch[i] = pin;
        else if (server.hasArg(k_switch)) valid = false;
        if (io_requested && (!server.hasArg(k_forward) || !server.hasArg(k_reverse) || !server.hasArg(k_switch)))
            io_fields_valid = false;
    }
    if (!io_fields_valid) {
        server.send(400, "application/json",
                    "{\"ok\":false,\"msg\":\"IO 配置字段错误或不完整，请刷新页面后重新提交全部通道引脚\"}");
        return;
    }
    String io_error;
    if ((io_requested || count_changed) && valid && !io_config_validate(candidate_io, candidate_channels, &io_error)) valid = false;
    if (!valid) {
        server.send(400, "application/json",
                    io_error.length() ?
                    String("{\"ok\":false,\"msg\":\"") + json_escape(io_error.c_str()) + "\"}" :
                    "{\"ok\":false,\"msg\":\"参数无效；自定义序列号必须为空或正好15位大写字母/数字\"}");
        return;
    }
    // 减少 AMS 时保留剩余通道的运动，停用通道由应用配置时停止并移出队列。
    const bool io_changed = memcmp(&candidate_io, &IO_CONFIG, sizeof(IO_CONFIG)) != 0;
    if ((io_changed || candidate_ams > AMS_COUNT) && !BMCU_can_maintain()) {
        server.send(409, "application/json", "{\"ok\":false,\"msg\":\"请先停止所有电机运动和当前通道动作\"}");
        return;
    }
    const int candidate_limit = server.hasArg("motor_limit")
        ? server.arg("motor_limit").toInt() : min(MOTOR_LIMIT, candidate_channels);
    int remaining_running = 0;
    for (int ch = 0; ch < candidate_channels; ch++)
        if (BMCU_motor_status(ch).direction) remaining_running++;
    if (candidate_limit < remaining_running) {
        server.send(409, "application/json", "{\"ok\":false,\"msg\":\"请先停止部分电机，再降低并发上限\"}");
        return;
    }
    if (server.hasArg("motor_limit")) MOTOR_LIMIT = server.arg("motor_limit").toInt();
    if (server.hasArg("buslog")) BUS_LOG_ENABLE = server.arg("buslog").toInt() ? 1 : 0;
    if (server.hasArg("serial")) {
        String serial = server.arg("serial");
        serial.toCharArray(CUSTOM_SERIAL, sizeof(CUSTOM_SERIAL));
    }
    AMS_COUNT = candidate_ams;
    MOTOR_LIMIT = min(MOTOR_LIMIT, candidate_channels);
    if (server.hasArg("manual_ms")) MANUAL_PULSE_MS = server.arg("manual_ms").toInt();
    if (server.hasArg("auto_ms")) AUTO_LOAD_MS = server.arg("auto_ms").toInt();
    if (io_requested) IO_CONFIG = candidate_io;
    for (int i = 0; i < candidate_channels; i++) {
        String kt = "to" + String(i), kc = "cm" + String(i);
        if (server.hasArg(kt)) {
            int v = server.arg(kt).toInt();
            if (v >= 3 && v <= 30) PULLBACK_TIMEOUT[i] = v;
        }
        if (server.hasArg(kc)) {
            int v = server.arg(kc).toInt();
            if (v >= 0 && v <= 2) CHANNEL_MODE[i] = v;
        }
    }
    if (server.hasArg("otaurl")) {
        String u = server.arg("otaurl");
        u.trim();
        u.toCharArray(OTA_URL, OTA_URL_MAX);
    }
    if (server.hasArg("otanotify")) OTA_NOTIFY = server.arg("otanotify").toInt() ? 1 : 0;
    if (!settings_save_to_flash()) {
        // 硬件尚未应用候选接线；回退完整内存快照，保持数量、接线和运行状态一致。
        settings_restore_snapshot(previous);
        BMCU_apply_channel_modes();
        server.send(500, "application/json", "{\"ok\":false,\"msg\":\"保存失败，请重试\"}");
        return;
    }
    if (io_changed || count_changed) BMCU_apply_io_config();
    BMCU_apply_channel_modes();
    LOG_MAIN("[web] 设置已保存");
    if (BUS_LOG_ENABLE != buslog_before) {
        LOG_MAIN("[web] BambuBus 原始报文日志已%s", BUS_LOG_ENABLE ? "启用" : "关闭");
    }
    settings_log_all();
    server.send(200, "application/json", "{\"ok\":true,\"msg\":\"参数已保存\"}");
}

// dir=0 停止任意来源的动作；启动只登记截止时间，立即返回。
/**
 * 处理手动电机控制请求
 * 方向为 0 时停止任意来源的动作；否则校验通道和时长后提交点动，忙时进入手动队列。
 */
static void handle_manual_motor() {
    if (!server.hasArg("motor") || !server.hasArg("dir") ||
        !valid_number("motor", 0, BMCU_motor_count() - 1) || !valid_number("dir", -1, 1)) {
        server.send(400, "application/json", "{\"ok\":false,\"msg\":\"电机编号或方向无效\"}");
        return;
    }
    int motor = server.arg("motor").toInt(), dir = server.arg("dir").toInt();
    if (!dir) {
        BMCU_stop_motor(motor);
        LOG_MAIN("[web] 已停止通道 %d", motor + 1);
        server.send(200, "application/json", "{\"ok\":true,\"msg\":\"已停止\"}");
        return;
    }
    if (!valid_number("duration", 1, MAX_PULSE_MS - 1)) {
        server.send(400, "application/json", "{\"ok\":false,\"msg\":\"点动时间无效\"}");
        return;
    }
    int duration = server.hasArg("duration") ? server.arg("duration").toInt() : MANUAL_PULSE_MS;
    if (!BMCU_channel_motor_ready(motor)) {
        server.send(409, "application/json",
                    "{\"ok\":false,\"msg\":\"该通道的电机引脚是「无」，请先到「电机与微动引脚」里配置\"}");
        return;
    }
    if (!BMCU_manual_pulse(motor, dir, duration)) {
        server.send(409, "application/json", "{\"ok\":false,\"msg\":\"通道正在运行或已排队\"}");
        return;
    }
    const bool queued = BMCU_motor_status(motor).queue_position > 0;
    LOG_MAIN("[web] 通道 %d %s%s，%dms", motor + 1, queued ? "排队" : "开始", dir > 0 ? "进料" : "退料", duration);
    server.send(202, "application/json", queued ? "{\"ok\":true,\"msg\":\"已排队\"}" : "{\"ok\":true,\"msg\":\"已启动\"}");
}

/**
 * 处理 WiFi 配网请求
 * 保留 AP 的同时尝试连接目标路由；成功后保存凭据并重启，失败则恢复原网络。
 */
static void handle_wifi_save() {
    if (!maintenance_required()) return;
    String ssid = server.arg("ssid");
    String pass = server.arg("pass");
    ssid.trim();

    if (ssid.length() == 0) {
        server.send(400, "application/json", "{\"ok\":false,\"msg\":\"SSID 不能为空\"}");
        return;
    }
    if (pass.length() > 0 && pass.length() < 8) {
        server.send(400, "application/json", "{\"ok\":false,\"msg\":\"WPA 密码至少 8 位\"}");
        return;
    }

    if (server.hasArg("mdns")) {
        String normalized;
        if (!normalize_mdns_name(server.arg("mdns"), normalized)) {
            server.send(400, "application/json",
                        "{\"ok\":false,\"msg\":\"mDNS 名称仅支持 1-32 位字母、数字和连字符，且首尾不能是连字符\"}");
            return;
        }
        if (!save_mdns_name(normalized)) {
            server.send(500, "application/json", "{\"ok\":false,\"msg\":\"mDNS 名称保存失败，请重试\"}");
            return;
        }
    }

    LOG_MAIN("[web] 正在连接 WiFi '%s' ...", ssid.c_str());

    // 保留 AP，同时尝试连路由器：失败也不至于失联
    wifi_set_mode(WIFI_AP_STA);
    WiFi.begin(ssid.c_str(), pass.c_str());

    uint32_t t0 = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - t0 < STA_TIMEOUT_MS) {
        hal_watchdog_feed();
        delay(100);
    }

    if (WiFi.status() == WL_CONNECTED) {
        save_wifi_creds(ssid.c_str(), pass.c_str());
        String ip = WiFi.localIP().toString();
        LOG_MAIN("[web] WiFi 连接成功，IP = %s，重启切换为 STA 模式", ip.c_str());
        // 页面拿到 ip 后会去轮询这个地址，能连上才跳转，所以这里必须把 ip 回给前端
        server.send(200, "application/json",
                    "{\"ok\":true,\"ip\":\"" + ip + "\",\"msg\":\"配网成功，设备即将重启\"}");
        delay(1000);
        ESP.restart();
        return;
    }

    // 失败：退回原网络（如果原来就在 STA 且保存过凭据）
    WiFi.disconnect();
    if (!ap_mode && sta_ssid[0]) WiFi.begin(sta_ssid, sta_pass);

    LOG_MAIN("[web] WiFi 连接失败: %s", ssid.c_str());
    server.send(200, "application/json",
                "{\"ok\":false,\"msg\":\"连接失败，请确认 SSID / 密码，且路由器为 2.4GHz\"}");
}

/**
 * 处理 mDNS 名称保存请求
 * 校验并写入 WiFi NVS；STA 在线时立即重载 mDNS，AP 模式下联网后生效。
 */
static void handle_mdns_save() {
    String normalized;
    if (!normalize_mdns_name(server.arg("mdns"), normalized)) {
        server.send(400, "application/json",
                    "{\"ok\":false,\"msg\":\"mDNS 名称仅支持 1-32 位字母、数字和连字符，且首尾不能是连字符\"}");
        return;
    }
    if (!save_mdns_name(normalized)) {
        server.send(500, "application/json", "{\"ok\":false,\"msg\":\"mDNS 名称保存失败，请重试\"}");
        return;
    }

    const bool active = !ap_mode && WiFi.status() == WL_CONNECTED;
    const bool started = !active || start_mdns();
    const String url = "http://" + normalized + ".local/";
    const String msg = active
        ? (started ? "mDNS 名称已保存并生效" : "mDNS 名称已保存，但服务启动失败，请重启设备")
        : "mDNS 名称已保存，联网后生效";
    server.send(200, "application/json",
                "{\"ok\":true,\"mdns\":\"" + normalized + "\",\"url\":\"" + url +
                "\",\"msg\":\"" + msg + "\"}");
}

/**
 * 处理忘记 WiFi 请求
 * 清除凭据并重启，使设备进入配网模式。
 */
static void handle_wifi_forget() {
    if (!maintenance_required()) return;
    forget_wifi_creds();
    LOG_MAIN("[web] 已清除 WiFi 凭据，重启后进入配网模式");
    server.send(200, "application/json", "{\"ok\":true,\"msg\":\"已忘记网络，设备重启中\"}");
    delay(300);
    ESP.restart();
}

/**
 * 处理 WiFi 扫描请求
 * 异步启动扫描；扫描未完成时返回 scanning，完成后返回 SSID、信号强度和加密状态列表。
 */
static void handle_scan() {
    int n = WiFi.scanComplete();
    if (n < 0) {
        if (n == WIFI_SCAN_FAILED) WiFi.scanNetworks(true);
        server.send(200, "application/json", "{\"scanning\":true}");
        return;
    }
    String j = "[";
    for (int i = 0; i < n; i++) {
        if (i) j += ',';
        j += "{\"ssid\":\"" + json_escape(WiFi.SSID(i).c_str()) + "\"";
        j += ",\"rssi\":" + String(WiFi.RSSI(i));
        j += ",\"enc\":" + String(WiFi.encryptionType(i) == WIFI_AUTH_OPEN ? 0 : 1) + "}";
    }
    j += "]";
    WiFi.scanDelete();
    server.send(200, "application/json", j);
}

/**
 * 处理日志读取请求
 * 支持按游标增量读取、清空和下载；主板日志和 BambuBus 日志通过 ch 参数选择。
 */
static void handle_log() {
    int ch = server.hasArg("ch") ? server.arg("ch").toInt() : LOG_CH_MAIN;
    if (ch < 0 || ch >= LOG_CH_COUNT) ch = LOG_CH_MAIN;

    if (server.hasArg("clear")) {
        logbus_clear(ch);
        server.sendHeader("X-Log-Seq", String(logbus_total(ch)));
        server.sendHeader("Cache-Control", "no-store");
        server.send(200, "text/plain; charset=utf-8", "");
        return;
    }

    uint32_t seq = server.hasArg("seq") ? strtoul(server.arg("seq").c_str(), nullptr, 10) : 0;

    // 日志响应复用缓冲区；容量在单次拉取上限外留出格式化余量
    static char buf[LOG_PULL_MAX + 64];
    logbus_read(ch, &seq, buf, sizeof(buf));

    server.sendHeader("X-Log-Seq", String(seq));
    server.sendHeader("Cache-Control", "no-store");
    if (server.hasArg("dl")) {
        server.sendHeader("Content-Disposition",
                          ch == LOG_CH_BUS ? "attachment; filename=bambubus.log"
                                           : "attachment; filename=main.log");
    }
    server.send(200, "text/plain; charset=utf-8", buf);
}

/**
 * 处理设备重启请求
 * 在维护状态下回复成功并重启设备。
 */
static void handle_reboot() {
    if (!maintenance_required()) return;
    LOG_MAIN("[web] 收到重启指令");
    server.send(200, "application/json", "{\"ok\":true,\"msg\":\"设备正在重启\"}");
    delay(300);
    ESP.restart();
}

/**
 * 处理恢复出厂设置请求
 * 恢复默认参数、清除 WiFi 凭据并重启。
 */
static void handle_factory_reset() {
    if (!maintenance_required()) return;
    settings_reset_to_defaults();
    settings_save_to_flash();
    forget_wifi_creds();
    LOG_MAIN("[web] 已恢复出厂设置（含 WiFi 凭据），即将重启");
    server.send(200, "application/json", "{\"ok\":true,\"msg\":\"已恢复默认，设备重启中\"}");
    delay(300);
    ESP.restart();
}

// ============================================================
// OTA：在线检查/更新 + 本地上传
// ============================================================
// 从 manifest 解析出的固件下载地址；为空表示当前没有可用更新
static String g_ota_url;
// manifest 中固件的 32 位 MD5；下载时交给 Update.setMD5() 校验
static String g_ota_md5;
// 服务器清单中的版本号，用于和 FW_VERSION 比较
static String g_ota_ver;
// 服务器清单中的更新说明，展示在网页更新卡片
static String g_ota_notes;
// 固件声明的字节数；0 表示清单未提供，下载时跳过长度比对
static size_t g_ota_size = 0;

/**
 * 获取 OTA 服务基础地址
 * @return 去掉首尾空白和末尾斜杠的地址
 * 用于拼接 manifest.json 和相对固件路径。
 */
static String ota_base() {
    String b = OTA_URL;
    b.trim();
    while (b.endsWith("/")) b.remove(b.length() - 1);
    return b;
}

// ---- 够用就好的迷你 JSON 取值，不引第三方库 ----
/**
 * 跳过 JSON 空白字符
 * @param s JSON 字符串
 * @param i 当前解析位置
 * 推进索引越过空格、制表符、回车和换行。
 */
static void json_ws(const String &s, int &i) {
    while (i < static_cast<int>(s.length()) && (s[i] == ' ' || s[i] == '\t' || s[i] == '\r' || s[i] == '\n')) i++;
}

/**
 * 解析 JSON 字符串值
 * @param s JSON 字符串
 * @param i 当前解析位置
 * @param out 输出的字符串
 * @return true 表示解析成功
 * 处理常见转义字符，直到遇到结束引号。
 */
static bool json_read_string(const String &s, int &i, String &out) {
    if (i >= static_cast<int>(s.length()) || s[i] != '"') return false;
    i++;
    out = "";
    while (i < static_cast<int>(s.length())) {
        char c = s[i++];
        if (c == '\\' && i < static_cast<int>(s.length())) {
            char e = s[i++];
            out += (e == 'n') ? '\n' : (e == 't') ? '\t' : e;
        } else if (c == '"') {
            return true;
        } else {
            out += c;
        }
    }
    return false;
}

// 取 "key": 后面的值，字符串或裸标量都能取
/**
 * 读取 JSON 键对应的原始值
 * @param s JSON 字符串
 * @param key 键名
 * @param out 输出值
 * @return true 表示找到并成功读取
 * 支持带引号字符串和裸标量，裸标量会去掉尾部空白。
 */
static bool json_get_raw(const String &s, const char *key, String &out) {
    String pat = "\"" + String(key) + "\"";
    int p = s.indexOf(pat);
    if (p < 0) return false;

    int i = p + static_cast<int>(pat.length());
    json_ws(s, i);
    if (i >= static_cast<int>(s.length()) || s[i] != ':') return false;
    i++;
    json_ws(s, i);

    if (i < static_cast<int>(s.length()) && s[i] == '"') return json_read_string(s, i, out);

    out = "";
    while (i < static_cast<int>(s.length()) && s[i] != ',' && s[i] != '}' && s[i] != '\n') out += s[i++];
    out.trim();
    return out.length() > 0;
}

/**
 * 读取 JSON 键对应的值
 * @param s JSON 字符串
 * @param key 键名
 * @param out 输出值
 * @return true 表示找到并成功读取
 * 先清空输出，再复用 json_get_raw()。
 */
static bool json_get(const String &s, const char *key, String &out) {
    out = "";
    return json_get_raw(s, key, out);
}

// 取出 "key": { ... } 那个对象（含大括号），用来读某个目标平台的固件信息
/**
 * 提取 JSON 子对象文本
 * @param s JSON 字符串
 * @param key 键名
 * @param out 输出的对象文本（含大括号）
 * @return true 表示找到完整对象
 * 按大括号深度和字符串状态扫描，避免把嵌套对象截断。
 */
static bool json_get_object(const String &s, const char *key, String &out) {
    String pat = "\"" + String(key) + "\"";
    int p = s.indexOf(pat);
    if (p < 0) return false;

    int i = p + static_cast<int>(pat.length());
    json_ws(s, i);
    if (i >= static_cast<int>(s.length()) || s[i] != ':') return false;
    i++;
    json_ws(s, i);
    if (i >= static_cast<int>(s.length()) || s[i] != '{') return false;

    int depth = 0;
    const int start = i;
    bool in_str = false;
    for (; i < static_cast<int>(s.length()); i++) {
        const char c = s[i];
        if (in_str) {
            if (c == '\\') i++;
            else if (c == '"') in_str = false;
            continue;
        }
        if (c == '"') in_str = true;
        else if (c == '{') depth++;
        else if (c == '}' && --depth == 0) { out = s.substring(start, i + 1); return true; }
    }
    return false;
}

// a 比 b 新返回 true（按 . 分段比数字，1.0.10 > 1.0.9）
/**
 * 比较两个版本号的新旧
 * @param a 待比较版本
 * @param b 基准版本
 * @return true 表示 a 比 b 新
 * 按点分段比较数字，可正确处理 1.0.10 > 1.0.9。
 */
static bool version_newer(const String &a, const String &b) {
    int ia = 0, ib = 0;
    while (ia < static_cast<int>(a.length()) || ib < static_cast<int>(b.length())) {
        int va = 0, vb = 0;
        while (ia < static_cast<int>(a.length()) && a[ia] != '.') { if (isdigit(static_cast<unsigned char>(a[ia]))) va = va * 10 + (a[ia] - '0'); ia++; }
        while (ib < static_cast<int>(b.length()) && b[ib] != '.') { if (isdigit(static_cast<unsigned char>(b[ib]))) vb = vb * 10 + (b[ib] - '0'); ib++; }
        if (va != vb) return va > vb;
        if (ia < static_cast<int>(a.length())) ia++;
        if (ib < static_cast<int>(b.length())) ib++;
    }
    return false;
}

/**
 * 校验 OTA 版本号格式
 * @param v 版本字符串
 * @return true 表示格式有效
 * 要求 YY.MM.DD 或 YY.MM.DD.修订号格式。
 */
static bool version_valid(const String &v) {
    int groups = 1, digits = 0;
    for (unsigned int i = 0; i < v.length(); i++) {
        if (v[i] == '.') {
            if (digits < 1 || digits > (groups == 4 ? 3 : 2) || groups >= 4) return false;
            groups++;
            digits = 0;
        } else if (isdigit(static_cast<unsigned char>(v[i]))) {
            digits++;
        } else return false;
    }
    return (groups == 3 && digits == 2) || (groups == 4 && digits >= 1 && digits <= 3);
}

// ---- HTTP ----
/**
 * 创建 OTA HTTP 客户端
 * @param url 固件或清单地址
 * @return 新建的 WiFiClient 指针
 * HTTPS 使用不校验证书的 WiFiClientSecure 以节省内存，HTTP 使用普通客户端。
 */
static WiFiClient *ota_make_client(const String &url) {
    if (url.startsWith("https")) {
        auto *c = new WiFiClientSecure();
        c->setInsecure();  // 不校验证书；省掉塞证书要用的几十 KB 内存
        return c;
    }
    return new WiFiClient();
}

namespace {
    // client 是 new 出来的，但 HTTPClient 析构时还会再碰一次 _client->stop()，
    // 而 end() 只在 socket 还连着的时候才把 _client 置空（连接已断开时它不置空）。
    // 所以 client 必须活得比 HTTPClient 久：用这个壳子持有，声明在 HTTPClient 前面，
    // 块内对象按声明逆序析构，正好保证「先 ~HTTPClient，再 delete client」。
    class OtaClientHolder {
    public:
        /**
         * 构造 OTA 客户端持有器
         * @param url 请求地址
         * 按地址创建 HTTP 或 HTTPS 客户端，并保证其生命周期长于 HTTPClient。
         */
        explicit OtaClientHolder(const String &url) : _client(ota_make_client(url)) {}
        /**
         * 析构 OTA 客户端持有器
         * 在 HTTPClient 析构之后释放底层网络客户端。
         */
        ~OtaClientHolder() { delete _client; }
        /**
         * 获取底层网络客户端引用
         * @return 底层 WiFiClient 引用
         * 供 HTTPClient.begin() 使用。
         */
        WiFiClient &ref() const { return *_client; }

    private:
        WiFiClient *_client;
    };
}

/**
 * 执行 OTA HTTP GET 请求
 * @param url 请求地址
 * @param body 响应正文
 * @param err 错误信息
 * @param timeout_ms 超时时间
 * @return true 表示请求成功
 * 跟随重定向，检查 HTTP 状态，并在下载前确认设备处于可维护状态。
 */
static bool ota_http_get(const String &url, String &body, String &err, uint16_t timeout_ms) {
    const OtaClientHolder client(url);
    HTTPClient http;
    http.setTimeout(timeout_ms);
    http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);

    if (!http.begin(client.ref(), url)) {
        err = "地址无法解析";
        http.end();
        return false;
    }
    const int code = http.GET();
    if (code != HTTP_CODE_OK) {
        err = "服务器返回 HTTP " + String(code);
        http.end();
        return false;
    }
    if (!BMCU_can_maintain()) {
        err = "下载前检测到打印机通信或电机动作";
        http.end();
        return false;
    }
    body = http.getString();
    http.end();
    return true;
}

/**
 * 获取并解析 OTA 清单
 * @param err 错误信息
 * @return true 表示成功取得当前板型固件信息
 * 读取 manifest.json，提取目标平台的 url、md5、version、notes 和 size，并校验版本与 MD5 格式。
 */
static bool ota_fetch_manifest(String &err) {
    const String base = ota_base();
    if (!base.length()) {
        err = "还没有配置更新源地址";
        return false;
    }

    String body;
    if (!ota_http_get(base + "/manifest.json", body, err, 3000)) return false;

    String obj;
    if (!json_get_object(body, FW_TARGET, obj)) {
        err = "清单里没有 " + String(FW_TARGET) + " 对应的固件";
        return false;
    }

    g_ota_url = g_ota_md5 = g_ota_ver = g_ota_notes = "";
    g_ota_size = 0;

    json_get(obj, "url", g_ota_url);
    json_get(obj, "md5", g_ota_md5);
    json_get(obj, "version", g_ota_ver);
    json_get(obj, "notes", g_ota_notes);
    if (!version_valid(g_ota_ver)) {
        err = "当前板型清单的 version 无效，应为 YY.MM.DD[.修订号]";
        return false;
    }

    // 清单里可能写的是大写 MD5，Update 库是拿小写十六进制摘要去比对的，统一转小写
    g_ota_md5.trim();
    g_ota_md5.toLowerCase();
    if (g_ota_md5.length() != 32) {
        err = "当前板型清单缺少有效的 md5";
        return false;
    }
    for (unsigned int i = 0; i < g_ota_md5.length(); i++) {
        if (!isxdigit(static_cast<unsigned char>(g_ota_md5[i]))) {
            err = "当前板型清单中的 md5 无效";
            return false;
        }
    }

    String sz;
    if (json_get_raw(obj, "size", sz)) g_ota_size = static_cast<size_t>(strtoul(sz.c_str(), nullptr, 10));

    g_ota_url.trim();
    if (g_ota_url.length() && !g_ota_url.startsWith("http")) {
        g_ota_url = base + "/" + g_ota_url;  // 清单里允许只写文件名
    }
    if (!g_ota_url.length()) {
        err = "清单里没有写 url";
        return false;
    }
    return true;
}

/**
 * 生成 OTA 错误 JSON
 * @param msg 错误文本
 * @return {"ok":false,"msg":"..."} 格式字符串
 * 统一网页接口的错误响应格式。
 */
static String ota_json_err(const String &msg) {
    return "{\"ok\":false,\"msg\":\"" + json_escape(msg.c_str()) + "\"}";
}

/**
 * 检查 OTA 前设备是否空闲
 * @return true 表示允许继续
 * 复用 maintenance_required() 的检查和 409 响应。
 */
static bool ota_require_stopped() {
    return maintenance_required();
}

// ---- 检查更新 ----
/**
 * 处理检查更新请求
 * 可配置关闭自动提示；带 force 参数时强制检查，返回当前版本、最新版本、更新说明和下载信息。
 */
static void handle_ota_check() {
    if (!ota_require_stopped()) return;
    // 关掉"自动提示"后，进页面时的静默检查直接跳过，不白跑一趟网络请求；
    // 用户手动点"检查更新"会带 force=1 绕过这个开关。
    if (OTA_NOTIFY == 0 && !server.hasArg("force")) {
        server.send(200, "application/json", "{\"ok\":true,\"skipped\":true}");
        return;
    }

    String err;
    if (!ota_fetch_manifest(err)) {
        LOG_MAIN("[ota] 检查更新失败: %s", err.c_str());
        server.send(200, "application/json", ota_json_err(err));
        return;
    }

    const bool has = version_newer(g_ota_ver, String(FW_VERSION));
    String j;
    j.reserve(320);
    j  = "{\"ok\":true";
    j += ",\"current\":\"" + String(FW_VERSION) + "\"";
    j += ",\"target\":\"" + String(FW_TARGET) + "\"";
    j += ",\"latest\":\"" + json_escape(g_ota_ver.c_str()) + "\"";
    j += ",\"has_update\":" + String(has ? "true" : "false");
    j += ",\"notes\":\"" + json_escape(g_ota_notes.c_str()) + "\"";
    j += ",\"size\":" + String((unsigned long)g_ota_size);
    j += ",\"md5\":\"" + json_escape(g_ota_md5.c_str()) + "\"";
    j += ",\"url\":\"" + json_escape(g_ota_url.c_str()) + "\"}";
    LOG_MAIN("[ota] 检查更新: 本机 %s, 服务器 %s, %s", FW_VERSION, g_ota_ver.c_str(),
             has ? "有新版" : "已是最新");
    server.send(200, "application/json", j);
}

// ---- 在线更新：下载 → 写 flash → 重启 ----
/**
 * 从 URL 下载并写入固件
 * @param err 错误信息
 * @return true 表示下载、校验和写入全部成功
 * 按 Content-Length 下载，写入 Update 分区并持续校验 MD5、设备空闲状态和看门狗。
 */
static bool ota_flash_from_url(String &err) {
    const OtaClientHolder client(g_ota_url);
    HTTPClient http;
    http.setTimeout(3000);
    http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);

    if (!http.begin(client.ref(), g_ota_url)) {
        err = "固件地址无法解析";
        http.end();
        return false;
    }
    int code = http.GET();
    if (code != HTTP_CODE_OK) {
        err = "下载固件失败，HTTP " + String(code);
        http.end();
        return false;
    }

    if (!BMCU_can_maintain()) {
        err = "下载前检测到打印机通信或电机动作";
        http.end();
        return false;
    }

    int len = http.getSize();
    if (len <= 0) {
        err = "服务器必须提供 Content-Length，不支持分块传输";
        http.end();
        return false;
    }
    if (g_ota_size && g_ota_size != static_cast<size_t>(len)) {
        err = "固件大小与清单不一致";
        http.end();
        return false;
    }
    if (!Update.begin(static_cast<size_t>(len))) {
        err = String("Update.begin 失败: ") + Update.errorString();
        http.end();
        return false;
    }
    if (!Update.setMD5(g_ota_md5.c_str())) {
        err = "固件 MD5 设置失败";
        Update.abort();
        http.end();
        return false;
    }

    WiFiClient *stream = http.getStreamPtr();
    uint8_t buf[1460];
    size_t written = 0;
    uint32_t last_rx = millis();
    bool timeout = false;

    while (true) {
        if (!BMCU_can_maintain()) {
            err = "更新期间检测到打印机通信或电机动作";
            break;
        }
        if (written >= (size_t)len) break;

        size_t want = sizeof(buf);
        if (static_cast<size_t>(len) - written < want) want = static_cast<size_t>(len) - written;

        int n = stream->readBytes(buf, want);
        if (n <= 0) {
            if (millis() - last_rx > 15000) { timeout = true; break; }
            hal_watchdog_feed();
            continue;
        }
        last_rx = millis();
        if (Update.write(buf, static_cast<size_t>(n)) != static_cast<size_t>(n)) {
            err = String("写入 Flash 失败: ") + Update.errorString();
            break;
        }
        written += static_cast<size_t>(n);
        hal_watchdog_feed();
    }

    http.end();

    if (err.length()) { Update.abort(); return false; }
    if (timeout) { Update.abort(); err = "下载超时"; return false; }
    if (written != static_cast<size_t>(len)) {
        Update.abort();
        err = "下载不完整（" + String(static_cast<unsigned long>(written)) + "/" + String(len) + " 字节）";
        return false;
    }
    if (!BMCU_can_maintain()) {
        Update.abort();
        err = "写入结束前检测到打印机通信或电机动作";
        return false;
    }
    if (!Update.end()) {
        err = String("写入结束失败: ") + Update.errorString();
        return false;
    }
    return true;
}

/**
 * 处理在线升级请求
 * 检查清单和版本，仅当服务器版本更新时下载写入，成功后重启。
 */
static void handle_ota_pull() {
    if (!ota_require_stopped()) return;
    String err;
    if (!ota_fetch_manifest(err)) {
        server.send(200, "application/json", ota_json_err(err));
        return;
    }
    if (!ota_require_stopped()) return;
    if (!version_newer(g_ota_ver, String(FW_VERSION))) {
        server.send(200, "application/json",
                    ota_json_err("服务器上的版本不高于本机（" + g_ota_ver + "），已跳过"));
        return;
    }

    LOG_MAIN("[ota] 开始在线更新到 %s：%s", g_ota_ver.c_str(), g_ota_url.c_str());
    uint32_t t0 = millis();
    bool ok = ota_flash_from_url(err);

    if (!ok) {
        LOG_MAIN("[ota] 更新失败: %s", err.c_str());
        server.send(200, "application/json", ota_json_err("更新失败：" + err));
        return;
    }

    LOG_MAIN("[ota] 更新成功（%lu ms），重启", static_cast<unsigned long>(millis() - t0));
    server.send(200, "application/json",
                "{\"ok\":true,\"msg\":\"已更新到 " + json_escape(g_ota_ver.c_str()) +
                    "，设备即将重启\"}");
    delay(1000);
    ESP.restart();
}

// ---- 本地升级：浏览器直接传 .bin ----
// 本次浏览器上传是否已失败；开始新上传时清零
static bool  ota_up_failed = false;
// 本地升级失败原因，用于日志和返回给网页的错误提示
static String ota_up_err;

/**
 * 处理浏览器固件上传
 * 按开始、写入、结束、中断四个阶段驱动 Update 接口，并在上传过程中喂狗和检查设备状态。
 */
static void handle_ota_upload() {
    HTTPUpload &up = server.upload();

    if (up.status == UPLOAD_FILE_START) {
        ota_up_failed = false;
        ota_up_err = "";
        if (!BMCU_can_maintain()) {
            ota_up_failed = true;
            ota_up_err = "电机正在运动，拒绝更新";
            return;
        }
        LOG_MAIN("[ota] 开始接收固件上传: %s", up.filename.c_str());
        if (!Update.begin(UPDATE_SIZE_UNKNOWN)) {
            ota_up_failed = true;
            ota_up_err = String("Update.begin 失败: ") + Update.errorString();
        }
    } else if (up.status == UPLOAD_FILE_WRITE) {
        if (!ota_up_failed && !BMCU_can_maintain()) {
            ota_up_failed = true;
            ota_up_err = "上传期间检测到打印机通信或电机动作";
            Update.abort();
        }
        if (!ota_up_failed && Update.write(up.buf, up.currentSize) != up.currentSize) {
            ota_up_failed = true;
            ota_up_err = String("写入 Flash 失败: ") + Update.errorString();
        }
        hal_watchdog_feed();  // 上传期间 loop 被占住，必须自己喂狗
    } else if (up.status == UPLOAD_FILE_END) {
        if (!ota_up_failed && !BMCU_can_maintain()) {
            ota_up_failed = true;
            ota_up_err = "上传结束前检测到打印机通信或电机动作";
            Update.abort();
        }
        if (!ota_up_failed && !Update.end(true)) {
            ota_up_failed = true;
            ota_up_err = String("写入结束失败: ") + Update.errorString();
        }
    } else if (up.status == UPLOAD_FILE_ABORTED) {
        Update.abort();
        ota_up_failed = true;
        ota_up_err = "上传被中断";
    }
}

/**
 * 处理本地升级完成回调
 * 根据上传阶段记录的结果返回成功或失败，成功后重启设备。
 */
static void handle_ota_upload_done() {
    if (ota_up_failed) {
        LOG_MAIN("[ota] 本地升级失败: %s", ota_up_err.c_str());
        server.send(409, "application/json", ota_json_err("升级失败：" + ota_up_err));
        return;
    }
    LOG_MAIN("[ota] 本地升级成功，重启");
    server.send(200, "application/json", "{\"ok\":true,\"msg\":\"升级成功，设备即将重启\"}");
    delay(1000);
    ESP.restart();
}

// 配网模式下的强制门户：任何未知请求都跳回首页
/**
 * 处理未匹配的 HTTP 请求
 * 配网模式下跳转到强制门户首页，STA 模式下返回 404。
 */
static void handle_not_found() {
    if (ap_mode) {
        server.sendHeader("Location", "http://" + WiFi.softAPIP().toString() + "/", true);
        server.send(302, "text/plain", "");
    } else {
        server.send(404, "text/plain", "Not Found");
    }
}

/**
 * 注册 Web 路由
 * 把首页、状态、设置、接线、电机、WiFi、日志、重启、恢复出厂和 OTA 接口绑定到处理函数。
 */
static void setup_routes() {
    server.on("/", HTTP_GET, handle_root);
    server.on("/api/status", HTTP_GET, handle_status);
    server.on("/api/settings", HTTP_GET, handle_settings_get);
    server.on("/api/settings", HTTP_POST, handle_settings_save);
    server.on("/api/bus", HTTP_POST, handle_bus_save);
    server.on("/api/pcf", HTTP_POST, handle_pcf_save);
    server.on("/api/motor", HTTP_POST, handle_manual_motor);
    server.on("/api/scan", HTTP_GET, handle_scan);
    server.on("/api/wifi", HTTP_POST, handle_wifi_save);
    server.on("/api/mdns", HTTP_POST, handle_mdns_save);
    server.on("/api/forget", HTTP_POST, handle_wifi_forget);
    server.on("/api/log", HTTP_GET, handle_log);
    server.on("/api/reboot", HTTP_GET, handle_reboot);
    server.on("/api/factory", HTTP_POST, handle_factory_reset);
    server.on("/api/ota/check", HTTP_POST, handle_ota_check);
    server.on("/api/ota/pull", HTTP_POST, handle_ota_pull);
    server.on("/api/ota/upload", HTTP_POST, handle_ota_upload_done, handle_ota_upload);
    server.onNotFound(handle_not_found);
}

// ============================================================
// 对外接口
// ============================================================
/**
 * 初始化 Web 管理服务
 * 读取 WiFi 凭据，优先连接已保存网络，失败或未配置时启动配网热点，然后注册路由并启动 HTTP 服务。
 */
void web_config_init() {
    load_wifi_creds();
    WiFi.persistent(false);

    if (sta_ssid[0]) {
        wifi_state = ST_CONNECTING;
        connect_deadline = millis() + STA_TIMEOUT_MS;
        wifi_set_mode(WIFI_STA);
        WiFi.begin(sta_ssid, sta_pass);
        LOG_MAIN("[web] 正在连接已保存的 WiFi '%s' ...", sta_ssid);
    } else {
        start_config_ap();
    }

    setup_routes();
    server.begin();
    LOG_MAIN("[web] HTTP 管理服务已启动");
}

/**
 * 轮询 Web 与 WiFi 状态
 * 处理 STA 连接成功/超时切换，调用 WebServer 处理请求，并在 AP 模式处理 DNS 强制门户请求。
 */
void web_config_run() {
    if (wifi_state == ST_CONNECTING) {
        if (WiFiClass::status() == WL_CONNECTED) {
            on_sta_connected();
        } else if (static_cast<int32_t>(millis() - connect_deadline) >= 0) {
            LOG_MAIN("[web] 连接 WiFi 超时，转入配网模式");
            start_config_ap();
        }
    }

    server.handleClient();
    if (ap_mode) dns.processNextRequest();
}

#endif  // ARDUINO_ARCH_ESP32
