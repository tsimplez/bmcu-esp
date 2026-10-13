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
#include "settings_storage.h"
#include "BMCU.h"
#include "version.h"

// ============================================================
// 常量
// ============================================================
#define AP_SSID         "BMCU"     // 配网热点名
#define AP_PASS         "12345678" // 配网热点密码（至少 8 位）
#define WIFI_NS         "wifi"     // NVS 中 WiFi 凭据的命名空间
#define STA_TIMEOUT_MS  15000      // 连接路由器超时（毫秒）
#define LOG_PULL_MAX    16384      // 单次拉取日志的最大字节数

static WebServer   server(80);
static DNSServer   dns;
static Preferences wprefs;

enum { ST_IDLE = 0, ST_CONNECTING };
static int      wifi_state = ST_IDLE;
static uint32_t connect_deadline = 0;
static bool     ap_mode = false;
static char     sta_ssid[64] = {};
static char     sta_pass[64] = {};

// ============================================================
// 页面
// ============================================================
// 页面本体在 web/index.html，由 PlatformIO 的 board_build.embed_txtfiles 编进固件，
// 改完 html 直接 pio run -t upload 即可，不需要单独烧文件系统
extern const uint8_t web_index_html_start[] asm("_binary_web_index_html_start");
extern const uint8_t web_index_html_end[] asm("_binary_web_index_html_end");
#define PAGE_HTML ((const char *)web_index_html_start)

// embed_txtfiles 会在末尾补一个 '\0'，发给浏览器时要去掉，否则 HTML 尾巴上会多一个空字节
static size_t page_html_len() {
    size_t n = (size_t)(web_index_html_end - web_index_html_start);
    if (n && web_index_html_start[n - 1] == 0) n--;
    return n;
}
// ============================================================
// 小工具
// ============================================================
static void copy_str(char *dst, size_t cap, const char *src) {
    if (cap == 0) return;
    size_t n = src ? strlen(src) : 0;
    if (n >= cap) n = cap - 1;
    if (n) memcpy(dst, src, n);
    dst[n] = 0;
}

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

// ============================================================
// WiFi 凭据（NVS 独立命名空间 "wifi"）
// ============================================================
static void load_wifi_creds() {
    sta_ssid[0] = sta_pass[0] = 0;
    if (!wprefs.begin(WIFI_NS, true)) return;
    wprefs.getString("ssid", sta_ssid, sizeof(sta_ssid));
    wprefs.getString("pass", sta_pass, sizeof(sta_pass));
    wprefs.end();
}

static void save_wifi_creds(const char *ssid, const char *pass) {
    if (wprefs.begin(WIFI_NS, false)) {
        wprefs.putString("ssid", ssid);
        wprefs.putString("pass", pass);
        wprefs.end();
    }
    copy_str(sta_ssid, sizeof(sta_ssid), ssid);
    copy_str(sta_pass, sizeof(sta_pass), pass);
}

static void forget_wifi_creds() {
    if (wprefs.begin(WIFI_NS, false)) {
        wprefs.clear();
        wprefs.end();
    }
    sta_ssid[0] = sta_pass[0] = 0;
}

// ============================================================
// WiFi 状态机
// ============================================================
// 切换 WiFi 模式（模式确定后 WiFi 协议栈才算初始化，此时才能关省电）
static void wifi_set_mode(wifi_mode_t m) {
    WiFi.mode(m);
    WiFi.setSleep(false);
}

static void start_config_ap() {
    wifi_set_mode(WIFI_AP_STA);
    // 固定 AP 网段，并把网关/DNS 都指向自己，客户端才能被强制门户接管
    WiFi.softAPConfig(IPAddress(192, 168, 4, 1), IPAddress(192, 168, 4, 1), IPAddress(255, 255, 255, 0));
    WiFi.softAP(AP_SSID, AP_PASS);
    dns.start(53, "*", WiFi.softAPIP());
    ap_mode = true;
    wifi_state = ST_IDLE;
    LOG_MAIN("[web] 未连接 WiFi，配网热点 '%s' 已开启：连上后访问 http://%s/",
             AP_SSID, WiFi.softAPIP().toString().c_str());
}

static void on_sta_connected() {
    wifi_state = ST_IDLE;
    ap_mode = false;
    if (MDNS.begin("bmcu")) MDNS.addService("http", "tcp", 80);
    LOG_MAIN("[web] 已连接 WiFi '%s'：http://%s/ 或 http://bmcu.local/",
             sta_ssid, WiFi.localIP().toString().c_str());
}

// ============================================================
// REST API
// ============================================================
static bool maintenance_required() {
    if (BMCU_can_maintain()) return true;
    server.send(409, "application/json",
                "{\"ok\":false,\"msg\":\"请先停止所有电机运动\"}");
    return false;
}

static void handle_root() {
    // 页面必须和固件配套：禁掉缓存，避免升级后浏览器仍用旧页面（旧页面不认新字段）。
    server.sendHeader("Cache-Control", "no-store");
    server.send_P(200, "text/html; charset=utf-8", (PGM_P)PAGE_HTML, page_html_len());
}

static void handle_status() {
    IPAddress ip = ap_mode ? WiFi.softAPIP() : WiFi.localIP();

    String j;
    j.reserve(1600);
    j  = "{\"mode\":\"" + String(ap_mode ? "AP" : "STA") + "\"";
    j += ",\"ip\":\"" + ip.toString() + "\"";
    j += ",\"ssid\":\"" + json_escape(ap_mode ? "" : sta_ssid) + "\"";
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
    j += "}";
    server.send(200, "application/json", j);
}

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
    j += "},\"pcf\":{\"enabled\":" + String(PCF_ENABLE ? "true" : "false") +
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
static bool valid_number(const String &key, int low, int high) {
    if (!server.hasArg(key)) return true;
    String raw = server.arg(key);
    char *end;
    long value = strtol(raw.c_str(), &end, 10);
    return end != raw.c_str() && !*end && value >= low && value <= high;
}

static bool parse_io_pin(const String &key, int &value, bool allow_none) {
    if (!server.hasArg(key)) return false;
    String raw = server.arg(key);
    char *end = nullptr;
    long parsed = strtol(raw.c_str(), &end, 10);
    if (end == raw.c_str() || *end || parsed < 0 || parsed > 255) return false;
    value = static_cast<int>(parsed);
    return io_config_pin_available(value, allow_none);
}

// PCF8575 开关 + I2C 接线：切换只清掉在新状态下不能用的引脚，保存后重启生效。
static void handle_pcf_save() {
    if (server.hasArg("enable") && !valid_number("enable", 0, 1)) {
        server.send(400, "application/json", "{\"ok\":false,\"msg\":\"启用开关取值无效\"}");
        return;
    }
    // 兼容旧页面：只带 sda/scl 的请求按"启用并保存接线"处理。
    const bool enable = !server.hasArg("enable") || server.arg("enable").toInt() != 0;
    int sda = PCF_SDA, scl = PCF_SCL;
    if (enable) {
        if (!server.hasArg("sda") || !server.hasArg("scl") ||
            !valid_number("sda", 0, 48) || !valid_number("scl", 0, 48)) {
            server.send(400, "application/json", "{\"ok\":false,\"msg\":\"请完整填写 SDA 和 SCL 的主控 GPIO\"}");
            return;
        }
        sda = server.arg("sda").toInt();
        scl = server.arg("scl").toInt();
    }
    if (!maintenance_required()) return;
    const SettingsData previous = settings_snapshot();
    PCF_ENABLE = enable ? 1 : 0;
    if (enable) {
        PCF_SDA = sda;
        PCF_SCL = scl;
    }
    // 只清掉在新状态下不可用的引脚：开启时是与 SDA/SCL 冲突的，关闭时是扩展脚，
    // 其余接线保留，用户已有接线不会被整套冲掉。
    const int cleared = io_config_clear_invalid_pins(IO_CONFIG, enable, PCF_SDA, PCF_SCL);
    String error;
    bool wiring_ok = !enable ||
        io_config_i2c_validate(PCF_SDA, PCF_SCL, IO_CONFIG, active_channel_count(), &error);
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
    String msg = String("PCF8575 已") + (enable ? "启用" : "关闭") + "，设备正在重启";
    if (cleared)
        msg += "；已清空 " + String(cleared) + " 个不再可用的引脚，请到「电机与微动引脚」重新配置";
    else if (enable)
        msg += "；请断电后按新引脚接线";
    LOG_MAIN("[web] PCF8575 已%s：SDA=IO%d，SCL=IO%d，清空引脚数=%d，重启生效",
             enable ? "启用" : "关闭", PCF_SDA, PCF_SCL, cleared);
    server.send(200, "application/json",
                String("{\"ok\":true,\"msg\":\"") + json_escape(msg.c_str()) + "\"}");
    delay(300);
    ESP.restart();
}

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

static void handle_wifi_forget() {
    if (!maintenance_required()) return;
    forget_wifi_creds();
    LOG_MAIN("[web] 已清除 WiFi 凭据，重启后进入配网模式");
    server.send(200, "application/json", "{\"ok\":true,\"msg\":\"已忘记网络，设备重启中\"}");
    delay(300);
    ESP.restart();
}

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

static void handle_reboot() {
    if (!maintenance_required()) return;
    LOG_MAIN("[web] 收到重启指令");
    server.send(200, "application/json", "{\"ok\":true,\"msg\":\"设备正在重启\"}");
    delay(300);
    ESP.restart();
}

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
static String g_ota_url;    // 从 manifest 里取到的固件下载地址
static String g_ota_md5;    // 可选的 md5
static String g_ota_ver;    // 服务器上的版本号
static String g_ota_notes;  // 更新说明
static size_t g_ota_size = 0;

static String ota_base() {
    String b = OTA_URL;
    b.trim();
    while (b.endsWith("/")) b.remove(b.length() - 1);
    return b;
}

// ---- 够用就好的迷你 JSON 取值，不引第三方库 ----
static void json_ws(const String &s, int &i) {
    while (i < static_cast<int>(s.length()) && (s[i] == ' ' || s[i] == '\t' || s[i] == '\r' || s[i] == '\n')) i++;
}

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

static bool json_get(const String &s, const char *key, String &out) {
    out = "";
    return json_get_raw(s, key, out);
}

// 取出 "key": { ... } 那个对象（含大括号），用来读某个目标平台的固件信息
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
        explicit OtaClientHolder(const String &url) : _client(ota_make_client(url)) {}
        ~OtaClientHolder() { delete _client; }
        WiFiClient &ref() const { return *_client; }

    private:
        WiFiClient *_client;
    };
}

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

static String ota_json_err(const String &msg) {
    return "{\"ok\":false,\"msg\":\"" + json_escape(msg.c_str()) + "\"}";
}

static bool ota_require_stopped() {
    return maintenance_required();
}

// ---- 检查更新 ----
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
static bool  ota_up_failed = false;
static String ota_up_err;

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
static void handle_not_found() {
    if (ap_mode) {
        server.sendHeader("Location", "http://" + WiFi.softAPIP().toString() + "/", true);
        server.send(302, "text/plain", "");
    } else {
        server.send(404, "text/plain", "Not Found");
    }
}

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
