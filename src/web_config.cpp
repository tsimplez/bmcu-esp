#include "web_config.h"

#if defined(ARDUINO_ARCH_ESP32)

#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <ESPmDNS.h>
#include <mdns.h>
#include <Preferences.h>
#include <Update.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
// 两套 HTTP 服务各自定义 HTTP_ANY，包含 SDK 头文件后恢复 Arduino 宏
#pragma push_macro("HTTP_ANY")
#undef HTTP_ANY
#include <esp_http_server.h>
#pragma pop_macro("HTTP_ANY")
#include <esp_random.h>
#include <esp_heap_caps.h>
#include <cJSON.h>
#include "json_utils.h"
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <lwip/sockets.h>
#include <new>
#include <memory>
#include <utility>

#include "log_bus.h"
#include "platform.h"
#include "settings_storage.h"
#include "project_presets.h"
#include "BMCU.h"
#include "BambuBus.h"
#include "version.h"
#include "i2c_bus.h"
#include "env_sensor.h"
#include "hall_sensor.h"
#include "lan_ams.h"
#include "PCF8575_PinMap.h"
#include <web_page_gzip.h>

#define AP_SSID_PREFIX  "BMCU"     // 配网热点名前缀
#define AP_PASS         "12345678" // 配网热点密码（至少 8 位）
#define WIFI_NS         "wifi"     // NVS 中 WiFi 凭据的命名空间
#define DEFAULT_MDNS_NAME "bmcu"   // 默认 mDNS 主机名
#define MDNS_NAME_MAX   32         // mDNS 主机名最大长度
#define STA_TIMEOUT_MS  15000      // 连接路由器超时（毫秒）
#define STA_RETRY_MS    30000      // 连接失败后再次尝试的间隔（毫秒）
#define LOG_PULL_MAX    16384      // 单次拉取日志的最大字节数

static WebServer   server(80);
// 配网热点模式下的 DNS 服务，把所有域名解析到设备自身以触发配置页
static DNSServer   dns;
// NVS 访问对象，保存 STA 的 SSID、密码和 mDNS 名称
static Preferences wprefs;

enum { ST_IDLE = 0, ST_CONNECTING, ST_RETRY_WAIT };
// WiFi 连接状态：ST_IDLE=未配置/已连接，ST_CONNECTING=正在连接，ST_RETRY_WAIT=等待重试
static int      wifi_state = ST_IDLE;
// 本次 STA 连接的超时截止时间（millis），超过后保持配网热点并等待重试
static uint32_t connect_deadline = 0;
// 下次重连已保存网络的时间（millis），仅 ST_RETRY_WAIT 状态使用
static uint32_t retry_deadline = 0;
// 当前是否运行在 AP 配网模式
static bool     ap_mode = false;
// 首次提交的新 WiFi 凭据失败后，短暂双闪提示再恢复配网慢闪
static uint32_t wifi_failure_led_deadline = 0;
static bool     wifi_failure_led_active = false;
// 已保存的路由器 SSID（最多 63 个可见字符 + NUL）
static char     sta_ssid[64] = {};
// 已保存的路由器密码
static char     sta_pass[64] = {};
// 已配置的 mDNS 主机名（不含 .local 后缀），自动避让重名不覆盖此配置
static char     mdns_name[MDNS_NAME_MAX + 1] = DEFAULT_MDNS_NAME;
// mDNS 协议栈当前使用的名称，重名时自动追加或递增 -2、-3 等编号
static char     mdns_active_name[MDNS_NAME_BUF_LEN] = {};
// mDNS 服务是否已成功启动
static bool     mdns_started = false;
static bool     ha_service_advertised = false;
// 根据芯片 ID 生成的配网热点名，例如 BMCU_1A2B3C
static char     ap_ssid[16] = {};

// ============================================================
// 小工具
// ============================================================
/**
 * 安全复制字符串到固定缓冲区
 * @param dst 目标缓冲区
 * @param cap 目标容量
 * @param src 源字符串
 * 按容量截断并保证以 NUL 结尾，源指针为空时写入空串
 */
static void copy_str(char *dst, size_t cap, const char *src) {
    if (cap == 0) return;
    size_t n = src ? strlen(src) : 0;
    if (n >= cap) n = cap - 1;
    if (n) memcpy(dst, src, n);
    dst[n] = 0;
}

// HA 配置独立存放，避免增加网络凭据时改变电机和接线记录格式
static constexpr uint32_t HA_RECORD_MAGIC = 0x48413032;
static constexpr uint32_t HA_FRESH_MS = 6000;
static constexpr uint32_t HA_REQUEST_MS = 4000;
static constexpr size_t HA_BODY_LIMIT = 8192;
struct HaConfig {
    char url[192] = {};
    char token[512] = {};
    char stage_entity[128] = {};
    char status_entity[128] = {};
    uint8_t enabled = 0;
    uint8_t reserved[3] = {};
};
struct HaRecord {
    uint32_t magic = HA_RECORD_MAGIC;
    HaConfig config;
    uint32_t checksum = 0;
};
struct HaStatus {
    bool connected = false, sampled = false;
    uint32_t received = 0;
    char stage[96] = {}, print_status[40] = {};
    char error[160] = {};
};
static HaConfig ha_config;
static HaStatus ha_status;
static uint32_t ha_generation = 1;
static uint32_t ha_connection_generation = 1;
static portMUX_TYPE ha_lock = portMUX_INITIALIZER_UNLOCKED;
static TaskHandle_t ha_task = nullptr;
struct HaEntityPage {
    uint32_t generation = 0, offset = 0, request = 0, completed = 0;
    bool requested = false, loading = false, ready = false, delivered = false;
    String body;
    char error[160] = {};
};
static HaEntityPage ha_entity_page;
static SemaphoreHandle_t ha_entity_mutex = nullptr;

// 在 HA 侧只筛选打印机的阶段和状态传感器，ESP32 每次接收一个小页
static constexpr char HA_ENTITY_TEMPLATE[] = R"HA(
{%- set offset = offset | int(0) -%}
{%- set sensors = states.sensor | list -%}
{%- set candidates = sensors | selectattr('entity_id', 'match', '^sensor\\.[a-z0-9_]+_(current_stage|print_status)$') | selectattr('entity_id', 'match', '^sensor\\.[a-z0-9_]{1,120}$') | sort(attribute='entity_id') | list -%}
{%- set printers = sensors | selectattr('entity_id', 'search', '_printer_name$') | list -%}
{%- set related = namespace(entities=[]) -%}
{%- for entity in candidates -%}
    {%- set identity = entity.entity_id -%}
    {%- set object_id = identity[7:] -%}
    {%- set printer = namespace(name='', prefix_length=0) -%}
    {%- for item in printers -%}
      {%- set prefix = item.entity_id[7:-13] -%}
      {%- if prefix and object_id.startswith(prefix ~ '_') and (prefix | length) > printer.prefix_length -%}
        {%- set printer.name = ((item.state if item.state not in ['unknown', 'unavailable', ''] else item.name | default(prefix, true)) | string)[:48] -%}
        {%- set printer.prefix_length = prefix | length -%}
      {%- endif -%}
    {%- endfor -%}
    {%- if printer.prefix_length -%}
      {%- set kind = 'stage' if object_id.endswith('_current_stage') else 'status' -%}
      {%- set related.entities = related.entities + [{'entity_id': identity, 'name': (entity.name | default(identity, true) | string)[:48], 'state': (entity.state | string)[:48], 'printer': printer.name, 'kind': kind}] -%}
    {%- endif -%}
{%- endfor -%}
{%- set total = related.entities | length -%}
{%- set page = namespace(entities=[], full=false) -%}
{%- for row in related.entities[offset:offset + 16] -%}
  {%- if not page.full -%}
    {%- set trial_offset = offset + (page.entities | length) + 1 -%}
    {%- set trial = {'total': total, 'next_offset': trial_offset if trial_offset < total else none, 'entities': page.entities + [row]} -%}
    {%- set encoded = trial | to_json(ensure_ascii=False) -%}
    {%- if (encoded.encode('utf-8') | length) <= 7600 -%}
      {%- set page.entities = page.entities + [row] -%}
    {%- else -%}
      {%- set page.full = true -%}
    {%- endif -%}
  {%- endif -%}
{%- endfor -%}
{%- set next_offset = offset + (page.entities | length) -%}
{{- {'total': total, 'next_offset': next_offset if next_offset < total else none, 'entities': page.entities} | to_json(ensure_ascii=False) -}}
)HA";

/**
 * 检查 HA 实体 ID 的域及字符范围
 * @param entity 实体 ID
 * @param domain 包含结尾点号的实体域
 * @return 空串或有效实体 ID 时返回 true
 */
static bool ha_entity_valid(const char *entity, const char *domain) {
    if (!*entity) return true;
    const size_t prefix = strlen(domain);
    if (strncmp(entity, domain, prefix) != 0 || !entity[prefix]) return false;
    for (const char *p = entity + prefix; *p; ++p)
        if (!((*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') || *p == '_')) return false;
    return true;
}

/**
 * 校验 HA 局域网 HTTP 地址
 * @param url 不带结尾斜杠的服务地址
 * @return 空串或主机、端口及可选路径前缀有效时返回 true
 * 拒绝凭据、控制字符、查询串和重定向目标；HTTPS 不隐式关闭证书校验
 */
static bool ha_url_valid(const char *url) {
    if (!*url) return true;
    if (strncmp(url, "http://", 7) != 0) return false;
    const char *p = url + 7, *host = p;
    while ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
           (*p >= '0' && *p <= '9') || *p == '.' || *p == '-') ++p;
    if (p == host || *host == '.' || *host == '-' || p[-1] == '.' || p[-1] == '-') return false;
    if (*p == ':') {
        const char *start = ++p;
        uint32_t port = 0;
        while (*p >= '0' && *p <= '9') {
            port = port * 10u + static_cast<unsigned>(*p++ - '0');
            if (port > 65535u || p - start > 5) return false;
        }
        if (p == start || !port) return false;
    }
    if (*p && *p != '/') return false;
    for (; *p; ++p)
        if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
              (*p >= '0' && *p <= '9') || strchr("/._~-", *p))) return false;
    return true;
}

/**
 * 判断 HA 连接信息是否完整
 * @param config 已校验的配置
 * @return 地址和令牌均已填写时返回 true
 */
static bool ha_configured(const HaConfig &config) {
    return config.url[0] && config.token[0];
}

/**
 * 判断是否允许局域网 HA 集成操作通道，无需先选择打印机状态实体
 * @return 已联网且已保存并启用 HA 连接时返回 true
 */
static bool ha_controls_enabled() {
    return !ap_mode && WiFi.status() == WL_CONNECTED && ha_config.enabled && ha_configured(ha_config);
}

/**
 * 生成 HA 设备标识，避免地址重分配后把请求发给另一台 BMCU
 * @return 芯片唯一标识的十六进制文本
 */
static String ha_device_id() {
    char id[17];
    snprintf(id, sizeof(id), "%016llx", static_cast<unsigned long long>(hal_get_device_id()));
    return String(id);
}

/**
 * 判断是否已选择两个打印状态实体
 * @param config 已校验的配置
 * @return 当前阶段与打印状态均已选择时返回 true
 */
static bool ha_entities_selected(const HaConfig &config) {
    return config.stage_entity[0] && config.status_entity[0];
}

/**
 * 校验 HA 当前格式的配置范围
 * @param config 待检查的配置
 * @return 字符串有界、格式有效且启用时连接信息完整则返回 true；实体允许稍后选择
 */
static bool ha_config_valid(const HaConfig &config) {
    if (!memchr(config.url, 0, sizeof(config.url)) || !memchr(config.token, 0, sizeof(config.token)) ||
        !memchr(config.stage_entity, 0, sizeof(config.stage_entity)) ||
        !memchr(config.status_entity, 0, sizeof(config.status_entity)) || config.enabled > 1 ||
        config.reserved[0] || config.reserved[1] || config.reserved[2]) return false;
    for (const char *p = config.token; *p; ++p)
        if (static_cast<uint8_t>(*p) < 33 || static_cast<uint8_t>(*p) > 126) return false;
    return ha_url_valid(config.url) && ha_entity_valid(config.stage_entity, "sensor.") &&
           ha_entity_valid(config.status_entity, "sensor.") &&
           (!config.enabled || ha_configured(config));
}

/**
 * 计算 HA 记录格式标识与配置的 CRC32
 * @param record 待检查的记录
 * @return 不包含 checksum 字段的 CRC32
 */
static uint32_t ha_record_crc(const HaRecord &record) {
    uint32_t crc = 0xFFFFFFFFu;
    const auto *bytes = reinterpret_cast<const uint8_t *>(&record);
    for (size_t i = 0; i < offsetof(HaRecord, checksum); ++i) {
        crc ^= bytes[i];
        for (int bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

/**
 * 检查 HA 存储记录的格式、配置及 CRC
 * @param record 已按当前长度读取的记录
 * @return 当前格式且内容完整时返回 true
 */
static bool ha_record_valid(const HaRecord &record) {
    return record.magic == HA_RECORD_MAGIC && ha_config_valid(record.config) &&
           record.checksum == ha_record_crc(record);
}

/**
 * 加载独立 HA 配置
 * 只接受当前长度、格式及 CRC；缺失或损坏时保持未配置默认值，不迁移旧记录
 */
static void ha_load_config() {
    ha_connection_generation = esp_random();
    if (!ha_connection_generation) ha_connection_generation = 1;
    Preferences prefs;
    HaRecord record;
    if (!prefs.begin("home_assistant", true)) return;
    const bool valid = prefs.getBytesLength("config") == sizeof(record) &&
                       prefs.getBytes("config", &record, sizeof(record)) == sizeof(record) && ha_record_valid(record);
    prefs.end();
    if (valid) ha_config = record.config;
}

/**
 * 保存 HA 候选记录并回读验证
 * @param config 已校验的新配置
 * @return 写入及回读一致时返回 true；失败时尝试回写原配置并保持运行配置不变
 */
static bool ha_save_config(const HaConfig &config) {
    HaRecord candidate, previous, verified;
    candidate.config = config;
    candidate.checksum = ha_record_crc(candidate);
    previous.config = ha_config;
    previous.checksum = ha_record_crc(previous);
    Preferences prefs;
    if (!prefs.begin("home_assistant", false)) return false;
    const bool saved = prefs.putBytes("config", &candidate, sizeof(candidate)) == sizeof(candidate) &&
                       prefs.getBytesLength("config") == sizeof(verified) &&
                       prefs.getBytes("config", &verified, sizeof(verified)) == sizeof(verified) &&
                       ha_record_valid(verified) && !memcmp(&candidate, &verified, sizeof(candidate));
    if (!saved) {
        const bool restored = prefs.putBytes("config", &previous, sizeof(previous)) == sizeof(previous) &&
                              prefs.getBytesLength("config") == sizeof(verified) &&
                              prefs.getBytes("config", &verified, sizeof(verified)) == sizeof(verified) &&
                              ha_record_valid(verified) && !memcmp(&previous, &verified, sizeof(previous));
        LOG_MAIN("[ha] 配置保存或回读失败，原记录回滚%s", restored ? "成功" : "失败，需重新保存");
    }
    prefs.end();
    return saved;
}

namespace {
    // HTTPClient 的正文循环可能在空读后继续等待，由底层客户端统一终止过期连接
    class HaClient final : public WiFiClient {
        uint32_t started = millis();
        /**
     * 检查本次请求的总期限，过期时关闭连接
     * @return 请求仍在期限内时返回 true
     */
        bool within_deadline() {
            if (millis() - started < HA_REQUEST_MS) return true;
            WiFiClient::stop();
            return false;
        }
    public:
        /**
     * 查询请求期限内可读取的字节数
     * @return 连接过期时为 0，否则为网络缓冲字节数
     */
        int available() override { return within_deadline() ? WiFiClient::available() : 0; }
        /**
     * 查询请求期限内的连接状态
     * @return 连接未过期且仍有效时返回非零值
     */
        uint8_t connected() override { return within_deadline() ? WiFiClient::connected() : 0; }
        /**
     * 读取请求期限内的一个字节
     * @return 字节值，连接过期或没有数据时为 -1
     */
        int read() override { return within_deadline() ? WiFiClient::read() : -1; }
        /**
     * 读取请求期限内的响应片段
     * @param buffer 输出缓冲
     * @param size 缓冲容量
     * @return 实际读取字节数，连接过期时为 -1
     */
        int read(uint8_t *buffer, size_t size) override {
            return within_deadline() ? WiFiClient::read(buffer, size) : -1;
        }
    };

    class HaBodyStream final : public Stream {
    public:
        String body;
        bool failed = false;
        /**
     * 将响应单字节写入有界缓冲
     * @param value 待追加字节
     * @return 成功追加的字节数
     */
        size_t write(uint8_t value) override { return write(&value, 1); }
        /**
     * 将 HTTP 响应片段写入有界缓冲
     * @param data 响应片段
     * @param size 片段长度
     * @return 成功追加的字节数，超限或内存不足时返回 0
     */
        size_t write(const uint8_t *data, size_t size) override {
            if (failed || size > HA_BODY_LIMIT - body.length() || !body.concat(reinterpret_cast<const char *>(data), size)) {
                failed = true;
                return 0;
            }
            return size;
        }
        /**
     * 查询只写缓冲可读取的字节数
     * @return 始终为 0
     */
        int available() override { return 0; }
        /**
     * 读取只写缓冲
     * @return 始终为 -1
     */
        int read() override { return -1; }
        /**
     * 查看只写缓冲的首字节
     * @return 始终为 -1
     */
        int peek() override { return -1; }
        /**
     * 完成内存写入，无需刷新外设
     */
        void flush() override {}
    };
}

/**
 * 在独立任务发起一次有界 HA REST 请求
 * @param config 本轮不可变配置
 * @param path API 路径
 * @param payload 非空时发送 POST，否则发送 GET
 * @param body 输出响应正文
 * @param error 输出不包含令牌和响应正文的错误信息
 * @return HTTP 状态码；传输或正文读取失败时返回负数
 * 禁止跟随重定向；连接及空闲读写限时，客户端总期限终止迟迟不结束的正文；析构期间客户端仍然存活
 */
static int ha_http(const HaConfig &config, const String &path, const char *payload, String &body, String &error) {
    HaClient client;
    HTTPClient http;
    http.setConnectTimeout(1500);
    http.setTimeout(1500);
    http.setReuse(false);
    http.setFollowRedirects(HTTPC_DISABLE_FOLLOW_REDIRECTS);
    if (!http.begin(client, String(config.url) + path)) {
        error = "无法创建 HA 请求";
        return -1;
    }
    http.addHeader("Authorization", String("Bearer ") + config.token);
    http.addHeader("Accept", "application/json");
    if (payload) http.addHeader("Content-Type", "application/json");
    const int code = payload ? http.POST(String(payload)) : http.GET();
    if (code < 200 || code >= 300) {
        error = code <= 0 ? "HA 连接失败或请求超时" :
                code == 401 ? "HA 令牌无效或已过期（401）" :
                code == 403 ? "HA 拒绝访问，请检查权限（403）" :
                code == 404 ? "HA 实体或接口不存在（404）" :
                code >= 300 && code < 400 ? "HA 地址发生重定向，请填写最终局域网 HTTP 地址" :
                String("HA 请求失败（HTTP ") + String(code) + "）";
        return code;
    }
    HaBodyStream sink;
    if (http.getSize() > static_cast<int>(HA_BODY_LIMIT) || http.writeToStream(&sink) < 0 || sink.failed) {
        error = "HA 响应过大或读取不完整";
        return -1;
    }
    body = std::move(sink.body);
    return code;
}

/**
 * 读取并严格解析一个 HA 实体状态
 * @param config 本轮配置
 * @param entity 要读取的实体 ID
 * @param state 输出原始状态
 * @param capacity 状态缓冲容量
 * @param error 输出错误原因
 * @return HTTP、JSON、实体 ID 和状态字符串全部有效时返回 true
 */
static bool ha_read_entity(const HaConfig &config, const char *entity, char *state, const size_t capacity, String &error) {
    String body;
    const int code = ha_http(config, String("/api/states/") + entity, nullptr, body, error);
    if (code < 200 || code >= 300) return false;
    const auto json = Json::parse(body.c_str(), body.length());
    String id, value;
    bool valid = Json::read_string(json.get(), "entity_id", id) && id == entity &&
                 Json::read_string(json.get(), "state", value) && value.length() > 0 && value.length() < capacity;
    if (valid) {
        for (unsigned int i = 0; i < value.length(); ++i) {
            const char p = value[i];
            if (static_cast<uint8_t>(p) < 32) valid = false;
        }
    }
    if (valid) copy_str(state, capacity, value.c_str());
    else error = "HA 实体响应缺少有效状态";
    return valid;
}

/**
 * 提交当前代次的网络读取结果
 * @param generation 请求使用的配置代次
 * @param status 新的实体快照
 * @return 配置仍一致且快照已发布时返回 true
 * 旧配置的网络读取不能覆盖当前配置的快照
 */
static bool ha_publish_status(uint32_t generation, const HaStatus &status) {
    portENTER_CRITICAL(&ha_lock);
    const bool current = generation == ha_generation;
    if (current) ha_status = status;
    portEXIT_CRITICAL(&ha_lock);
    return current;
}

/**
 * 读取一轮 HA 当前阶段和打印状态
 * @param config 本轮配置
 * @param generation 配置代次
 * @param status 输出实体快照
 * @return 两个传感器读取成功且配置代次未改变时返回 true
 */
static bool ha_poll(const HaConfig &config,const  uint32_t generation, HaStatus &status) {
    const uint32_t started = millis();
    String error;
    status = HaStatus();
    const bool valid = ha_read_entity(config, config.stage_entity, status.stage, sizeof(status.stage), error) &&
                       ha_read_entity(config, config.status_entity, status.print_status, sizeof(status.print_status), error);
    if (valid) {
        status.connected = status.sampled = true;
        status.received = started;
    } else {
        status.stage[0] = status.print_status[0] = 0;
        copy_str(status.error, sizeof(status.error), error.c_str());
    }
    return ha_publish_status(generation, status) && valid;
}

/**
 * 校验 HA 实体分页响应的范围、推进方向和候选字段
 * @param body HA 模板返回的 JSON
 * @param offset 本轮请求的偏移
 * @return 小页结构完整、实体类型正确且游标可以前进时返回 true
 */
static bool ha_entity_page_valid(const String &body,const  uint32_t offset) {
    const auto json = Json::parse(body.c_str(), body.length());
    const auto *next = Json::item(json.get(), "next_offset");
    const auto *entities = Json::item(json.get(), "entities");
    const int count = cJSON_GetArraySize(entities);
    uint32_t total_count = 0;
    bool valid = Json::read_uint(json.get(), "total", total_count, 100000) && cJSON_IsArray(entities) && count <= 16;
    if (valid) {
        const uint32_t end = offset + static_cast<uint32_t>(count);
        valid = count ? end <= total_count : offset >= total_count;
        if (cJSON_IsNull(next)) valid = valid && end >= total_count;
        else {
            uint32_t next_offset = 0;
            valid = valid && count > 0 && Json::read_uint(json.get(), "next_offset", next_offset, total_count) &&
                    next_offset == end && next_offset < total_count;
        }
    }
    const cJSON *row = nullptr;
    cJSON_ArrayForEach(row, entities) {
        String id, kind;
        if (!Json::read_string(row, "entity_id", id) || id.isEmpty() || id.length() >= 128 ||
            !Json::read_string(row, "kind", kind)) { valid = false; break; }
        if ((kind != "stage" && kind != "status") || !ha_entity_valid(id.c_str(), "sensor.")) {
            valid = false;
            break;
        }
        const char *fields[] = {"name", "state", "printer"};
        for (const char *field : fields) {
            String value;
            if (!Json::read_string(row, field, value) || value.length() > 192) valid = false;
        }
    }
    return valid;
}

/**
 * 请求实体列表的一页，只更新后台任务队列
 * @param generation 当前连接代次
 * @param offset 候选偏移
 * @param refresh 是否重新读取同一页
 * @return 队列和缓存可以访问时返回 true
 * 锁内不进行网络操作，新连接和显式刷新作废旧响应；普通请求等待在途页完成并被取走
 * 已完成页最多保留五秒等待原客户端，避免关闭网页后阻挡其他客户端
 */
static bool ha_queue_entity_page(const uint32_t generation,const  uint32_t offset,const  bool refresh) {
    if (!ha_entity_mutex || xSemaphoreTake(ha_entity_mutex, 0) != pdTRUE) return false;
    const bool serving_other = !refresh && ha_entity_page.generation == generation && ha_entity_page.offset != offset &&
        (ha_entity_page.loading || (!ha_entity_page.delivered && (ha_entity_page.ready || ha_entity_page.error[0]) &&
                                   millis() - ha_entity_page.completed < 5000u));
    if (!serving_other && (refresh || ha_entity_page.generation != generation || ha_entity_page.offset != offset ||
        (!ha_entity_page.ready && !ha_entity_page.loading && !ha_entity_page.error[0]))) {
        const uint32_t request = ha_entity_page.request + 1u;
        ha_entity_page = HaEntityPage();
        ha_entity_page.generation = generation;
        ha_entity_page.offset = offset;
        ha_entity_page.request = request;
        ha_entity_page.requested = ha_entity_page.loading = true;
    }
    xSemaphoreGive(ha_entity_mutex);
    return true;
}

/**
 * 在后台执行一次已排队的实体发现请求
 * @param config 本轮连接配置
 * @param generation 运行配置代次
 * @param connection_generation 连接代次
 * 分页正文只在互斥锁内交换，不持锁等待网络；旧连接和旧页请求均不能覆盖新缓存
 */
static void ha_discover_entities(const HaConfig &config,const  uint32_t generation,const  uint32_t connection_generation) {
    if (!ha_entity_mutex || xSemaphoreTake(ha_entity_mutex, 0) != pdTRUE) return;
    const bool requested = ha_entity_page.requested && ha_entity_page.generation == connection_generation;
    const uint32_t offset = ha_entity_page.offset, request = ha_entity_page.request;
    if (requested) ha_entity_page.requested = false;
    xSemaphoreGive(ha_entity_mutex);
    if (!requested) return;

    String body, error;
    bool valid = false;
    if (WiFi.status() != WL_CONNECTED) error = "WiFi 未连接，请恢复后刷新实体列表";
    else {
        Json::Document payload(cJSON_CreateObject(), &cJSON_Delete);
        cJSON *variables = cJSON_AddObjectToObject(payload.get(), "variables");
        const bool built = cJSON_AddStringToObject(payload.get(), "template", HA_ENTITY_TEMPLATE) &&
                           cJSON_AddNumberToObject(variables, "offset", offset);
        char *encoded = built ? cJSON_PrintUnformatted(payload.get()) : nullptr;
        if (!encoded) error = "内存不足，无法准备实体查询";
        else {
            const int code = ha_http(config, "/api/template", encoded, body, error);
            if (code >= 200 && code < 300) {
                valid = ha_entity_page_valid(body, offset);
                if (!valid) error = "HA 实体列表格式不完整，请刷新重试";
            } else if (code == 403) error = "发现实体需要 HA 管理员的长期访问令牌（403）";
        }
        cJSON_free(encoded);
    }
    bool published = false;
    if (xSemaphoreTake(ha_entity_mutex, portMAX_DELAY) == pdTRUE) {
        if (ha_entity_page.generation == connection_generation && ha_entity_page.request == request) {
            ha_entity_page.loading = false;
            ha_entity_page.ready = valid;
            ha_entity_page.completed = millis();
            ha_entity_page.body = valid ? std::move(body) : String();
            copy_str(ha_entity_page.error, sizeof(ha_entity_page.error), error.c_str());
            published = true;
        }
        xSemaphoreGive(ha_entity_mutex);
    }
    // 已选实体的状态由轮询更新，列表发现失败不能遮盖正常的打印状态
    if (published) {
        portENTER_CRITICAL(&ha_lock);
        if (generation == ha_generation && !ha_entities_selected(ha_config)) {
            ha_status.connected = valid;
            copy_str(ha_status.error, sizeof(ha_status.error), error.c_str());
        }
        portEXIT_CRITICAL(&ha_lock);
    }
}

/**
 * 在后台持续读取 HA 当前阶段和打印状态
 * @param arg 未使用的任务参数
 * 按需分页发现实体，选择齐全后轮询打印状态；代次改变后丢弃旧响应
 */
static void ha_worker(void *arg) {
    (void)arg;
    uint32_t seen_generation = 0, next_poll = 0;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(100));
        HaConfig config;
        portENTER_CRITICAL(&ha_lock);
        config = ha_config;
        const uint32_t generation = ha_generation;
        const uint32_t connection_generation = ha_connection_generation;
        portEXIT_CRITICAL(&ha_lock);
        if (seen_generation != generation) { seen_generation = generation; next_poll = 0; }
        if (!ha_configured(config)) continue;
        ha_discover_entities(config, generation, connection_generation);
        portENTER_CRITICAL(&ha_lock);
        const bool changed = generation != ha_generation;
        portEXIT_CRITICAL(&ha_lock);
        if (changed || !config.enabled || !ha_entities_selected(config)) continue;
        if (WiFi.status() != WL_CONNECTED) {
            HaStatus offline;
            copy_str(offline.error, sizeof(offline.error), "WiFi 未连接，等待恢复");
            ha_publish_status(generation, offline);
            continue;
        }
        if (next_poll && static_cast<int32_t>(millis() - next_poll) < 0) continue;
        HaStatus status;
        ha_poll(config, generation, status);
        next_poll = millis() + 1000u;
    }
}

/**
 * 按需启动 HA 后台网络任务
 * 连接信息齐全后允许按需发现实体，启用并选好实体后持续轮询；创建失败时在状态接口显示原因
 */
static void ha_start_worker() {
    if (!ha_configured(ha_config) || ha_task) return;
    if (!ha_entity_mutex) ha_entity_mutex = xSemaphoreCreateMutex();
    if (!ha_entity_mutex) {
        copy_str(ha_status.error, sizeof(ha_status.error), "内存不足，无法准备实体列表，请稍后重新保存");
        return;
    }
    if (xTaskCreate(ha_worker, "ha_network", 8192, nullptr, 1, &ha_task) != pdPASS) {
        ha_task = nullptr;
        portENTER_CRITICAL(&ha_lock);
        copy_str(ha_status.error, sizeof(ha_status.error), "内存不足，无法启动 HA 读取，请稍后重新保存");
        portEXIT_CRITICAL(&ha_lock);
    }
}

/**
 * 返回 HA 配置并隐藏已保存令牌
 * 令牌只返回是否已设置，禁止回显到网页、状态或日志
 */
static void handle_ha_get() {
    String json = String("{\"enabled\":") + (ha_config.enabled ? "true" : "false") +
        ",\"configured\":" + (ha_configured(ha_config) ? "true" : "false") +
        ",\"selected\":" + (ha_entities_selected(ha_config) ? "true" : "false") +
        ",\"generation\":" + String(ha_connection_generation) +
        R"(,"url":")" + Json::escape(ha_config.url) + R"(","stage_entity":")" + ha_config.stage_entity +
        R"(","status_entity":")" + ha_config.status_entity +
        R"(","token_set":)" + (ha_config.token[0] ? "true" : "false") + "}";
    server.sendHeader("Cache-Control", "no-store");
    server.send(200, "application/json", json);
}

/**
 * 拒绝 HA 表单中的未知字段和重复字段
 * @param keys 允许的字段名数组
 * @param count 字段数量
 * @return 所有提交字段均已识别且未重复时返回 true
 */
static bool ha_fields_valid(const char *const *keys,const  size_t count) {
    for (int i = 0; i < server.args(); ++i) {
        const String key = server.argName(i);
        bool known = false;
        for (size_t k = 0; k < count; ++k) known |= key == keys[k];
        for (int other = 0; other < i; ++other) if (server.argName(other) == key) return false;
        if (!known) return false;
    }
    return true;
}

/**
 * 严格解析 HA 请求中的无符号整数
 * @param key 请求参数名
 * @param value 输出整数
 * @return 参数存在、无多余字符且未溢出时返回 true
 */
static bool ha_argument_uint(const char *key, uint32_t &value) {
    if (!server.hasArg(key)) return false;
    const String text = server.arg(key);
    if (text.isEmpty()) return false;
    uint32_t result = 0;
    for (size_t i = 0; i < text.length(); ++i) {
        if (text[i] < '0' || text[i] > '9') return false;
        const uint32_t digit = text[i] - '0';
        if (result > (UINT32_MAX - digit) / 10u) return false;
        result = result * 10u + digit;
    }
    value = result;
    return true;
}

/**
 * 持久化并应用 HA 候选配置
 * @param candidate 已完成范围校验的配置
 * @param new_connection 是否开始新的连接与实体列表代次
 * @return 保存及回读成功时返回 true，失败时已回复错误并保留原配置
 */
static bool ha_apply_config(const HaConfig &candidate,const  bool new_connection) {
    if (memcmp(&candidate, &ha_config, sizeof(candidate)) != 0 && !ha_save_config(candidate)) {
        server.send(500, "application/json", "{\"ok\":false,\"msg\":\"HA 保存或回读失败，原配置仍生效，请重试\"}");
        return false;
    }
    portENTER_CRITICAL(&ha_lock);
    ha_config = candidate;
    if (++ha_generation == 0) ++ha_generation;
    if (new_connection && ++ha_connection_generation == 0) ++ha_connection_generation;
    ha_status = HaStatus();
    portEXIT_CRITICAL(&ha_lock);
    ha_start_worker();
    return true;
}

/**
 * 只保存 HA 连接信息并排队验证连接、加载首个实体页
 * 地址和令牌可先独立保存，空令牌保留原值；更换地址时清除旧实体选择，避免关联到另一台设备
 */
static void handle_ha_save() {
    const char *keys[] = {"enabled", "url", "token"};
    bool valid = ha_fields_valid(keys, 3) && server.hasArg("url") &&
                 (!server.hasArg("enabled") || server.arg("enabled") == "0" || server.arg("enabled") == "1");
    HaConfig candidate = ha_config;
    String url = server.arg("url"), token = server.arg("token");
    url.trim();
    while (url.endsWith("/")) url.remove(url.length() - 1);
    valid &= url.length() < sizeof(candidate.url) && token.length() < sizeof(candidate.token) &&
             url.length() == strlen(url.c_str()) && token.length() == strlen(token.c_str());
    if (valid) {
        if (url != candidate.url) {
            memset(candidate.stage_entity, 0, sizeof(candidate.stage_entity));
            memset(candidate.status_entity, 0, sizeof(candidate.status_entity));
        }
        memset(candidate.url, 0, sizeof(candidate.url));
        copy_str(candidate.url, sizeof(candidate.url), url.c_str());
        if (!token.isEmpty()) {
            memset(candidate.token, 0, sizeof(candidate.token));
            copy_str(candidate.token, sizeof(candidate.token), token.c_str());
        }
        candidate.enabled = !server.hasArg("enabled") || server.arg("enabled") == "1";
    }
    if (!valid || !ha_configured(candidate) || !ha_config_valid(candidate)) {
        server.send(400, "application/json", "{\"ok\":false,\"msg\":\"请填写有效的局域网 HTTP 地址和令牌，实体可在连接后选择\"}");
        return;
    }
    if (!ha_apply_config(candidate, true)) return;
    ha_queue_entity_page(ha_connection_generation, 0, true);
    LOG_MAIN("[ha] 连接信息已保存，正在验证连接和发现实体");
    server.send(200, "application/json", String(R"({"ok":true,"generation":)") + String(ha_connection_generation) +
                ",\"msg\":\"连接信息已保存，正在加载实体列表\"}");
}

/**
 * 保存已选择的 HA 实体并开始状态轮询
 * 校验连接代次，两个传感器必须不同且完整选择，不从候选列表自动替换失踪实体
 */
static void handle_ha_selection() {
    uint32_t generation = 0;
    if (!ha_argument_uint("generation", generation) || generation != ha_connection_generation) {
        server.send(409, "application/json", "{\"ok\":false,\"msg\":\"HA 连接配置已改变，请重新读取配置和实体列表\"}");
        return;
    }
    const char *keys[] = {"stage_entity", "status_entity", "generation"};
    bool valid = ha_fields_valid(keys, 3) && ha_configured(ha_config);
    HaConfig candidate = ha_config;
    char *destinations[] = {candidate.stage_entity, candidate.status_entity};
    for (unsigned i = 0; i < 2; ++i) {
        const String value = server.arg(keys[i]);
        if (!server.hasArg(keys[i]) || value.length() >= sizeof(candidate.stage_entity) ||
            value.length() != strlen(value.c_str())) { valid = false; continue; }
        memset(destinations[i], 0, sizeof(candidate.stage_entity));
        copy_str(destinations[i], sizeof(candidate.stage_entity), value.c_str());
    }
    if (!valid || !ha_config_valid(candidate) || !ha_entities_selected(candidate) ||
        !strcmp(candidate.stage_entity, candidate.status_entity)) {
        server.send(400, "application/json", "{\"ok\":false,\"msg\":\"请选择不同的当前阶段和打印状态传感器\"}");
        return;
    }
    if (!ha_apply_config(candidate, false)) return;
    LOG_MAIN("[ha] 实体选择已保存，状态读取%s", candidate.enabled ? "启用" : "停用");
    server.send(200, "application/json", "{\"ok\":true,\"msg\":\"实体选择已保存\"}");
}

/**
 * 返回一页实体候选或后台加载进度
 * 只保留一个有界小页，浏览器按 next_offset 合并；其他客户端占用缓存时返回等待，回调不等待 HA 网络
 * 显式刷新和改连接会作废旧响应，已取走的页允许后续请求替换
 */
static void handle_ha_entities() {
    server.sendHeader("Cache-Control", "no-store");
    const char *keys[] = {"offset", "generation", "refresh"};
    uint32_t offset = 0, generation = 0;
    if (!ha_fields_valid(keys, 3) || !ha_argument_uint("offset", offset) || offset > 100000u ||
        (server.hasArg("refresh") && server.arg("refresh") != "1")) {
        server.send(400, "application/json", "{\"ok\":false,\"msg\":\"实体分页参数无效\"}");
        return;
    }
    if (!ha_argument_uint("generation", generation) || generation != ha_connection_generation) {
        server.send(409, "application/json", "{\"ok\":false,\"msg\":\"HA 连接配置已改变，请重新读取配置\"}");
        return;
    }
    if (!ha_configured(ha_config)) {
        server.send(409, "application/json", "{\"ok\":false,\"msg\":\"请先保存 HA 地址和令牌\"}");
        return;
    }
    ha_start_worker();
    if (!ha_task || !ha_queue_entity_page(generation, offset, server.hasArg("refresh")) ||
        xSemaphoreTake(ha_entity_mutex, 0) != pdTRUE) {
        server.send(503, "application/json", "{\"ok\":false,\"msg\":\"实体列表暂时忙或内存不足，请稍后刷新\"}");
        return;
    }
    const bool matching = ha_entity_page.generation == generation && ha_entity_page.offset == offset;
    const bool ready = matching && ha_entity_page.ready;
    String json = String("{\"generation\":") + String(generation) + ",\"offset\":" + String(offset) +
        ",\"loading\":" + (!matching || ha_entity_page.loading ? "true" : "false") +
        ",\"ready\":" + (ready ? "true" : "false") + R"(,"error":")" + Json::escape(matching ? ha_entity_page.error : "") + "\",";
    json += ready ? ha_entity_page.body.substring(1) : String(R"("total":0,"next_offset":null,"entities":[]})");
    if (matching && !ha_entity_page.loading) ha_entity_page.delivered = true;
    xSemaphoreGive(ha_entity_mutex);
    server.send(200, "application/json", json);
}

/**
 * 判断 HA 是否确认打印机正在打印
 * @return HA 已启用、配置完整且两项新鲜状态分别为 printing 和 running 时返回 true
 * 只读取加锁快照，不等待网络；WiFi 断开、读取失败或状态过期时不允许霍尔停转检测
 */
bool web_config_ha_printing() {
    HaStatus status;
    portENTER_CRITICAL(&ha_lock);
    status = ha_status;
    const bool enabled = ha_config.enabled && ha_configured(ha_config) && ha_entities_selected(ha_config);
    portEXIT_CRITICAL(&ha_lock);
    return enabled && WiFi.status() == WL_CONNECTED && status.connected && status.sampled &&
           millis() - status.received < HA_FRESH_MS &&
           !strcmp(status.stage, "printing") && !strcmp(status.print_status, "running");
}

/**
 * 向 HTTP 与 WebSocket 状态追加 HA 快照
 * @param json 正在构建的状态对象
 * 配置后即显示两个状态；WiFi 断开或数据过期时不把缓存值标为新鲜
 */
static void append_ha_status(String &json) {
    HaStatus status;
    portENTER_CRITICAL(&ha_lock);
    status = ha_status;
    const bool configured = ha_configured(ha_config);
    const bool selected = ha_entities_selected(ha_config);
    const bool enabled = ha_config.enabled;
    portEXIT_CRITICAL(&ha_lock);
    const uint32_t age = millis() - status.received;
    const bool connected = enabled && WiFi.status() == WL_CONNECTED && status.connected;
    const bool fresh = connected && status.sampled && age < HA_FRESH_MS;
    json += String(R"(,"home_assistant":{"configured":)") + (configured ? "true" : "false") +
        ",\"selected\":" + (selected ? "true" : "false") +
        ",\"enabled\":" + (enabled ? "true" : "false") + ",\"connected\":" + (connected ? "true" : "false") +
        ",\"fresh\":" + (fresh ? "true" : "false") + ",\"age_ms\":" + (status.sampled ? String(age) : String(-1)) +
        R"(,"stage":")" + Json::escape(status.stage) + R"(","print_status":")" + Json::escape(status.print_status) +
        R"(","error":")" + Json::escape(status.error) + "\"}";
}

/**
 * 校验 mDNS 主机名格式
 * @param name 待校验的主机名
 * @return true 表示名称合法
 * 只允许 1-32 位字母、数字和连字符，且首尾不能是连字符
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
 * 去除空白和可选的 .local 后缀并转为小写，再用 mdns_name_valid() 校验
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
 * 取芯片 64 位唯一 ID 的低 24 位格式化为 6 位大写十六进制，拼成 BMCU_XXXXXX
 */
static const char *build_ap_ssid() {
    const auto suffix = static_cast<uint32_t>(hal_get_device_id() & 0xFFFFFFULL);
    snprintf(ap_ssid, sizeof(ap_ssid), "%s_%06lX", AP_SSID_PREFIX,
             static_cast<unsigned long>(suffix));
    return ap_ssid;
}

// ============================================================
// WiFi 凭据（NVS 独立命名空间 "wifi"）
// ============================================================
/**
 * 读取已保存的 WiFi 凭据
 * 从 NVS 的 wifi 命名空间读取 SSID、密码和 mDNS 名称到内存；无有效名称时使用默认值
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
 * 写入 NVS 并同步更新内存中的 SSID/密码
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
 * 把名称写入 NVS 的 wifi 命名空间，成功后同步更新内存中的名称
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
 * 清空 NVS 的 wifi 命名空间和内存中的 SSID/密码
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
/**
 * 获取当前应显示的 WiFi 灯效
 * @return 当前配网、连接、重试、已连接或关闭状态
 * 首次提交的新凭据失败时短暂显示重试双闪，之后回到配网慢闪
 */
static HalWifiLedState wifi_indicator_state() {
    if (WiFi.status() == WL_CONNECTED) return HalWifiLedState::connected;
    if (wifi_state == ST_CONNECTING) return HalWifiLedState::connecting;
    if (wifi_state == ST_RETRY_WAIT) return HalWifiLedState::retry_wait;
    if (wifi_failure_led_active) {
        if (static_cast<int32_t>(millis() - wifi_failure_led_deadline) < 0) return HalWifiLedState::retry_wait;
        wifi_failure_led_active = false;
    }
    return ap_mode ? HalWifiLedState::configuring : HalWifiLedState::off;
}

/**
 * 刷新当前 WiFi 状态灯
 * 状态判断与 GPIO 驱动分层，具体板载灯脚和冲突检查由平台适配层处理
 */
static void wifi_indicator_run() {
    hal_wifi_led_run(wifi_indicator_state());
}

// 切换 WiFi 模式（模式确定后 WiFi 协议栈才算初始化，此时才能关省电）
/**
 * 切换 WiFi 工作模式
 * @param m WiFi 模式
 * 设置模式后关闭 WiFi 省电，保证网页和串口响应及时
 */
static void wifi_set_mode(wifi_mode_t m) {
    WiFi.mode(m);
    WiFi.setSleep(false);
}

/**
 * 发起已保存 WiFi 的异步连接
 * 清除上次临时失败提示并快速闪灯；保留配网热点，由主循环检查连接结果与超时，不等待路由器响应
 */
static void start_sta_connect() {
    wifi_failure_led_active = false;
    wifi_set_mode(ap_mode ? WIFI_AP_STA : WIFI_STA);
    WiFi.begin(sta_ssid, sta_pass);
    wifi_state = ST_CONNECTING;
    connect_deadline = millis() + STA_TIMEOUT_MS;
    LOG_MAIN("[web] 正在连接已保存的 WiFi '%s' ...", sta_ssid);
}

/**
 * 结束失败的 STA 连接并安排下次重试
 * 保留配网热点和已保存凭据；未配置网络时保持空闲，不安排重连
 */
static void schedule_sta_retry() {
    WiFi.disconnect();
    wifi_state = sta_ssid[0] ? ST_RETRY_WAIT : ST_IDLE;
    retry_deadline = millis() + STA_RETRY_MS;
    if (sta_ssid[0])
        LOG_MAIN("[web] %u 秒后重试已保存的 WiFi '%s'", STA_RETRY_MS / 1000, sta_ssid);
}

/**
 * 启动配网热点
 * 停止离线 mDNS，创建固定网段的 AP，并把 DNS 通配请求指向自身，实现强制门户
 */
static void start_config_ap() {
    MDNS.end();
    mdns_started = false;
    mdns_active_name[0] = 0;
    wifi_set_mode(WIFI_AP_STA);
    const char *ssid = build_ap_ssid();
    // 固定 AP 网段，并把网关/DNS 都指向自己，客户端才能被强制门户接管
    WiFi.softAPConfig(IPAddress(192, 168, 4, 1), IPAddress(192, 168, 4, 1), IPAddress(255, 255, 255, 0));
    WiFi.softAP(ssid, AP_PASS);
    dns.start(53, "*", WiFi.softAPIP());
    ap_mode = true;
    LOG_MAIN("[web] 未连接 WiFi，配网热点 '%s' 已开启：连上后访问 http://%s/", ssid, WiFi.softAPIP().toString().c_str());
}

/**
 * 同步 mDNS 协议栈实际使用的主机名
 * 每秒按 HA 配置发布或撤回通道发现服务，并跟随协议栈的重名自动编号；离线时清空地址，不改写 NVS
 */
static void sync_mdns_name() {
    if (!mdns_started || ap_mode || WiFi.status() != WL_CONNECTED) {
        mdns_active_name[0] = 0;
        return;
    }

    static uint32_t last_check = 0;
    const uint32_t now = millis();
    if (now - last_check < 1000) return;
    last_check = now;

    const bool advertise = ha_controls_enabled();
    if (advertise != ha_service_advertised) {
        const esp_err_t result = advertise ? mdns_service_add(nullptr, "_bmcu-esp", "_tcp", 80, nullptr, 0)
                                           : mdns_service_remove("_bmcu-esp", "_tcp");
        if (result == ESP_OK) ha_service_advertised = advertise;
    }

    char actual[MDNS_NAME_BUF_LEN] = {};
    if (mdns_hostname_get(actual) != ESP_OK || !actual[0]) return;
    if (strcmp(actual, mdns_active_name) == 0) return;
    copy_str(mdns_active_name, sizeof(mdns_active_name), actual);
    if (strcmp(actual, mdns_name) != 0)
        LOG_MAIN("[web] mDNS 重名自动避让：%s -> %s", mdns_name, actual);
    LOG_MAIN("[web] mDNS 当前访问地址：http://%s.local/", actual);
}

/**
 * 启动或重载 mDNS 服务
 * @return true 表示 mDNS 已启动，名称冲突检测由协议栈异步完成
 * 使用配置名称发布 HTTP 服务；重名时由协议栈自动编号，主循环同步实际名称供网页和日志显示
 */
static bool start_mdns() {
    mdns_started = false;
    ha_service_advertised = false;
    mdns_active_name[0] = 0;
    MDNS.end();
    if (!MDNS.begin(mdns_name)) {
        MDNS.end();
        LOG_MAIN("[web] mDNS 启动失败：%s.local", mdns_name);
        return false;
    }
    MDNS.addService("http", "tcp", 80);
    mdns_started = true;
    LOG_MAIN("[web] mDNS 正在检测名称：%s.local，重名时自动递增编号", mdns_name);
    return true;
}

/**
 * 处理 STA 连接成功
 * 退出连接状态、停止强制门户 DNS 和配网热点，切回 STA 并按当前配置启动 mDNS 服务
 */
static void on_sta_connected() {
    wifi_state = ST_IDLE;
    if (ap_mode) dns.stop();
    wifi_set_mode(WIFI_STA);
    ap_mode = false;
    start_mdns();
    LOG_MAIN("[web] 已连接 WiFi '%s'：http://%s/", sta_ssid, WiFi.localIP().toString().c_str());
}

// ============================================================
// REST API
// ============================================================
/**
 * 检查是否允许维护操作
 * @return true 表示当前可以执行配置、重启或升级
 * 调用 BMCU_can_maintain()；不满足时直接向客户端发送 409 响应
 */
static bool maintenance_required() {
    if (BMCU_can_maintain()) return true;
    server.send(409, "application/json",
                "{\"ok\":false,\"msg\":\"请先停止所有电机运动并结束局域网换料\"}");
    return false;
}

/**
 * 读取当前平台的项目列表与整套接线预览
 * 根据候选接线校验兼容性，预览包含进退料、微动和霍尔，不受当前项目的引脚占用影响
 */
static void handle_projects_get() {
    String j = R"({"current":")" + String(project_preset_match(settings_snapshot())) + R"(","projects":[)";
    bool first = true;
    for (const auto &preset : PROJECT_PRESETS) {
        if (strcmp(preset.target, hal_project_target()) != 0) continue;
        String error;
        const bool available = project_preset_validate(preset, &error);
        if (!first) j += ',';
        first = false;
        j += R"({"id":")" + Json::escape(preset.id) + R"(","name":")" + Json::escape(preset.name) +
             R"(","available":)" + String(available ? "true" : "false") +
             R"(,"reason":")" + Json::escape(error.c_str()) + R"(","ams":)" + String(preset.ams) +
             ",\"rs485\":" + String(preset.bus.mode) + ",\"rx\":" + String(preset.bus.rx) +
             ",\"tx\":" + String(preset.bus.tx) + ",\"de\":" + String(preset.bus.de) +
             ",\"sda\":" + String(preset.bus.sda) + ",\"scl\":" + String(preset.bus.scl) +
             ",\"switch_pin\":" + String(preset.switch_pin) + ",\"pins\":[";
        IoConfig io{};
        project_preset_io(preset, io);
        const int channels = preset.ams >= 1 && preset.ams <= IO_CONFIG_MAX_CHANNELS / 4 ? preset.ams * 4 : 0;
        for (int ch = 0; ch < channels; ++ch) {
            if (ch) j += ',';
            j += "[" + String(io.motor[ch][0]) + "," + String(io.motor[ch][1]) +
                 "," + String(io.microswitch[ch]) + "," + String(io.hall[ch]) + "]";
        }
        j += "]}";
    }
    j += "]}";
    server.sendHeader("Cache-Control", "no-store");
    server.send(200, "application/json", j);
}

/**
 * 保存整套项目配置并重启
 * 只接收项目 ID，统一验证后保存候选快照；回调不改变当前接线或驱动电机
 */
static void handle_project_save() {
    const ProjectPreset *preset = server.hasArg("id") && server.args() == 1
        ? project_preset_find(server.arg("id").c_str()) : nullptr;
    if (!preset) {
        server.send(400, "application/json", "{\"ok\":false,\"msg\":\"请选择有效的项目\"}");
        return;
    }
    String error;
    if (!project_preset_validate(*preset, &error)) {
        server.send(400, "application/json", Json::message(false, error.c_str()));
        return;
    }
    if (!maintenance_required()) return;
    SettingsData candidate = settings_snapshot();
    project_preset_apply(*preset, candidate);
    if (!settings_save_snapshot(candidate)) {
        server.send(500, "application/json",
                    "{\"ok\":false,\"msg\":\"保存或回读校验失败，当前接线未切换，请重试\"}");
        return;
    }
    LOG_MAIN("[web] 项目配置已保存：%s，AMS=%d，重启生效", preset->name, preset->ams);
    server.send(200, "application/json", "{\"ok\":true,\"msg\":\"项目已保存，设备正在重启\"}");
    delay(300);
    ESP.restart();
}

/**
 * 处理首页请求
 * 禁用浏览器缓存并发送构建时压缩的 HTML 管理页面，由浏览器解压
 */
static void handle_root() {
    // 页面必须和固件配套：禁掉缓存，避免升级后浏览器仍用旧页面（旧页面不认新字段）
    server.sendHeader("Cache-Control", "no-store");
    server.sendHeader("Content-Encoding", "gzip");
    server.send_P(200, "text/html; charset=utf-8", (PGM_P)PAGE_HTML_GZ, sizeof(PAGE_HTML_GZ));
}

/**
 * 发送构建时打包的 PWA 静态资源
 * @param asset 包含路径、类型、字节长度和压缩标记的只读资源
 * 每次使用前重新校验缓存，文本按 gzip 发送，PNG 图标保持原始字节
 */
static void handle_web_asset(const WebAsset &asset) {
    server.sendHeader("Cache-Control", "no-cache");
    if (asset.gzip) server.sendHeader("Content-Encoding", "gzip");
    server.send_P(200, asset.mime, (PGM_P)asset.data, asset.length);
}

/**
 * 把传感器读数格式化为 JSON 数字
 * @param valid 读数是否有效
 * @param value 读数值
 * @param digits 小数位数
 * @return JSON 数字文本；无效时返回 null
 * 网页据此区分“未检测到/读取失败”和真实的 0 值
 */
static String sensor_json_value(const bool valid,const float value,const int digits) {
    return valid ? String(value, digits) : String("null");
}

/**
 * 生成运行状态快照，供 HTTP 与 WebSocket 共用
 * @return 完整状态 JSON；预留内存失败时为空
 * 汇总 WiFi、项目、总线、局域网换料、HA 阶段与暂停测试、有效当前通道、电机、耗材和传感器；五通压下状态只读取主循环缓存
 * mdns 保留配置名称，mdns_active 返回重名避让后的实际名称，离线或尚未读取时为空
 * filament_colors 按启用的全局通道顺序返回记录中的 #RRGGBB，供网页以不透明圆点显示
 * filament_alpha 按相同顺序返回 0-255 的 A 分量，0 为完全透明，255 为不透明
 */
static String status_json() {
    IPAddress ip = ap_mode ? WiFi.softAPIP() : WiFi.localIP();

    String j;
    if (!j.reserve(3200)) return String();
    j  = R"({"mode":")" + String(ap_mode ? "AP" : "STA") + "\"";
    j += R"(,"ip":")" + ip.toString() + "\"";
    j += R"(,"ssid":")" + Json::escape(ap_mode ? "" : sta_ssid) + "\"";
    j += R"(,"mdns":")" + Json::escape(mdns_name) + "\"";
    j += R"(,"mdns_active":")" + Json::escape(mdns_active_name) + "\"";
    j += ",\"rssi\":" + String(ap_mode ? 0 : WiFi.RSSI());
    j += R"(,"mac":")" + WiFi.macAddress() + "\"";
    j += ",\"uptime\":" + String(millis() / 1000);
    j += ",\"heap\":" + String(ESP.getFreeHeap());
    j += ",\"heap_total\":" + String(ESP.getHeapSize());
    j += R"(,"chip":")" + String(ESP.getChipModel()) + "\"";
    j += R"(,"project":")" + String(project_preset_match(settings_snapshot())) + "\"";
    j += ",\"rs485\":" + String(RS485_MODE);
    j += ",\"buslog\":" + String(BUS_LOG_ENABLE);
    j += ",\"ams\":" + String(AMS_COUNT);
    j += ",\"channels\":" + String(BMCU_motor_count());
    j += ",\"cm\":[";
    for (int i = 0; i < BMCU_motor_count(); i++) { if (i) j += ','; j += String(CHANNEL_MODE[i]); }
    j += ']';
    j += ",\"maxams\":" + String(BMCU_max_ams_count());
    j += ",\"printer\":" + String(BambuBus_printer_status());
    append_ha_status(j);
    const auto [connected, paired, busy, fault, current_channel, target_channel, age_ms, state, error] = lan_ams_status();
    j += R"(,"lan_ams":{"enabled":)" + String(lan_ams_enabled() ? "true" : "false") +
         ",\"connected\":" + String(connected ? "true" : "false") +
         ",\"paired\":" + String(paired ? "true" : "false") +
         ",\"busy\":" + String(busy ? "true" : "false") +
         ",\"fault\":" + String(fault ? "true" : "false") +
         ",\"current_channel\":" + String(current_channel) +
         ",\"target_channel\":" + String(target_channel) +
         ",\"age_ms\":" + String(age_ms == UINT32_MAX ? -1 : static_cast<int32_t>(age_ms)) +
         R"(,"state":")" + Json::escape(state) + R"(","error":")" + Json::escape(error) + "\"}";
    const auto current = BambuBus_current_channel_status();
    j += ",\"current_channel\":" + String(current.channel) +
         R"(,"current_channel_state":")" + String(current.state) + "\"";
    const auto pause = BambuBus_pause_test_status();
    j += ",\"pause_test\":{\"available\":" + String(pause.available ? "true" : "false") +
         ",\"active\":" + String(pause.active ? "true" : "false") +
         ",\"pending\":" + String(pause.pending) + ",\"channel\":" + String(pause.channel) +
         ",\"candidate_channel\":" + String(pause.candidate_channel) +
         ",\"frames\":" + String(pause.frames) +
         ",\"stop_seen\":" + String(pause.stop_seen ? "true" : "false") +
         ",\"result\":\"" + String(pause.result) + "\"}";
    j += ",\"bus_fault\":" + String(BMCU_printer_bus_fault() ? "true" : "false");
    j += ",\"bus_rx_errors\":" + String(BambuBus_rx_error_count());
    j += ",\"bus_rx_dropped\":" + String(BambuBus_rx_dropped_count());
    const uint32_t packet_age = BambuBus_last_valid_packet_age_ms();
    j += ",\"bus_last_valid_age_ms\":" +
         String(packet_age == UINT32_MAX ? -1 : static_cast<int32_t>(packet_age));
    j += ",\"motor_limit\":" + String(BMCU_motor_limit());
    j += ",\"clutch_enabled\":" + String(IO_CONFIG.clutch_enabled ? "true" : "false");
    j += ",\"motor_running\":" + String(BMCU_running_count());
    j += R"(,"total_switch":{"pin":)" + String(IO_CONFIG.switch_pin) +
         ",\"active_level\":" + String(TOTAL_SWITCH_ACTIVE_LEVEL) +
         ",\"pressed\":" + String(BMCU_total_switch_state()) + "}";
    j += ",\"motors\":[";
    for (int i = 0; i < BMCU_motor_count(); i++) {
        if (i) j += ',';
        auto [direction, source, remaining_ms, queue_position, queued_direction, queued_source] = BMCU_motor_status(i);
        j += "{\"dir\":" + String(direction) + ",\"source\":" + String(static_cast<int>(source)) +
             ",\"remaining_ms\":" + String(remaining_ms) + ",\"queue_position\":" + String(queue_position) +
             ",\"queued_dir\":" + String(queued_direction) + ",\"queued_source\":" + String(queued_source) + "}";
    }
    j += ']';
    // 每个通道的微动开关状态：1=有料 0=无料 -1=该通道没有微动开关
    j += ",\"fl\":[";
    for (int i = 0; i < BMCU_motor_count(); i++) {
        if (i) j += ',';
        j += String(BMCU_channel_filament_state(i));
    }
    j += "]";
    j += ",\"filament_colors\":[";
    for (int i = 0; i < active_channel_count(); i++) {
        if (i) j += ',';
        char color[8];
        snprintf(color, sizeof(color), "#%02X%02X%02X",
                 get_filament_color_R(i), get_filament_color_G(i), get_filament_color_B(i));
        j += '"'; j += color; j += '"';
    }
    j += ']';
    j += ",\"filament_alpha\":[";
    for (int i = 0; i < active_channel_count(); i++) {
        if (i) j += ',';
        j += String(get_filament_color_A(i));
    }
    j += ']';
    j += ",\"maintenance\":" + String(BMCU_can_maintain() ? "true" : "false");
    j += ",\"manual\":" + String(BMCU_manual_active() ? "true" : "false");
    j += ",\"cli\":" + String(WiFi.softAPgetStationNum());
    j += R"(,"version":")" + String(FW_VERSION) + "\"";
    j += R"(,"target":")" + String(FW_TARGET) + "\"";
    j += R"(,"build":")" + String(FW_BUILD) + "\"";
    j += R"(,"otaurl":")" + Json::escape(OTA_URL) + "\"";
    j += ",\"otanotify\":" + String(OTA_NOTIFY);
    j += ",\"otasize\":" + String(ESP.getSketchSize());
    const auto &[aht20_online, aht20_valid, bmp280_online, bmp280_valid, temperature_c, humidity_pct, bmp_temperature_c, pressure_hpa] = env_sensor_status();
    j += R"(,"i2c":{"configured":)" + String(i2c_bus_configured() ? "true" : "false") +
         ",\"pcf\":" + String(PCF8575_is_online() ? "true" : "false") + ",\"pcf_devices\":[";
    for (uint8_t device = 0; device < PCF8575_DEVICE_COUNT; ++device) {
        if (device) j += ',';
        j += "{\"address\":" + String(PCF8575_ADDR + device) +
             ",\"online\":" + String(PCF8575_is_online(device) ? "true" : "false") + "}";
    }
    j += "],\"aht20\":" + String(aht20_online ? "true" : "false") +
         ",\"bmp280\":" + String(bmp280_online ? "true" : "false") + "}";
    j += R"(,"env":{"aht_temp":)" + sensor_json_value(aht20_online && aht20_valid, temperature_c, 2) +
         ",\"humidity\":" + sensor_json_value(aht20_online && aht20_valid, humidity_pct, 2) +
         ",\"bmp_temp\":" + sensor_json_value(bmp280_online && bmp280_valid, bmp_temperature_c, 2) +
         ",\"pressure\":" + sensor_json_value(bmp280_online && bmp280_valid, pressure_hpa, 2) + "}";
    j += "}";
    return j;
}

/**
 * 处理 HTTP 运行状态查询
 * 返回主循环生成的状态快照，内存不足时提示稍后重试
 */
static void handle_status() {
    const String json = status_json();
    server.send(json.isEmpty() ? 503 : 200, "application/json",
                json.isEmpty() ? "{\"ok\":false,\"msg\":\"可用内存不足，请稍后重试\"}" : json);
}

/**
 * 严格解析霍尔接口中的无符号整数
 * @param key 查询或表单参数名
 * @param value 输出的 32 位无符号整数
 * @return true 表示参数存在且完整落在 0..UINT32_MAX
 * 拒绝负数、空串、尾随字符和溢出，供历史游标与检测设置共用
 */
static bool parse_hall_uint(const char *key, uint32_t &value) {
    if (!server.hasArg(key)) return false;
    const String raw = server.arg(key);
    if (raw.length() == 0) return false;
    uint32_t parsed = 0;
    for (size_t i = 0; i < raw.length(); ++i) {
        const char c = raw[i];
        if (c < '0' || c > '9') return false;
        const auto digit = static_cast<uint32_t>(c - '0');
        if (parsed > (UINT32_MAX - digit) / 10u) return false;
        parsed = parsed * 10u + digit;
    }
    value = parsed;
    return true;
}

/**
 * 向霍尔响应追加有效读数或空值
 * @param json 待追加的 JSON 文本
 * @param valid 是否存在有效数据
 * @param value ADC 读数或统计值
 * 无效值使用 null，不能把缺失样本画成零点
 */
static void append_hall_value(String &json,const bool valid,const uint16_t value) {
    json += valid ? String(value) : String("null");
}

/**
 * 追加霍尔检测配置的 JSON 字段
 * @param json 待追加的 JSON 对象文本，不包含花括号
 * @param config 已保存的通道检测参数
 */
static void append_hall_detection_config(String &json, const HallDetectionConfig &config) {
    json += "\"lower\":" + String(config.lower) + ",\"upper\":" + String(config.upper) +
            ",\"timeout_s\":" + String(config.timeout_s);
}

/**
 * 保存单个通道的霍尔双阈值与超时设置
 * 校验通道和打开弹框时的引脚，写入并回读成功后才应用；失败时保留原运行配置
 * 主循环在下一次检测时重新计时，HTTP 回调不执行检测或电机动作
 */
static void handle_hall_detection_save() {
    uint32_t channel = 0, pin = 0, lower = 0, upper = 0, timeout = 0;
    if (!parse_hall_uint("channel", channel) || channel >= static_cast<uint32_t>(active_channel_count()) ||
        channel >= IO_CONFIG_MAX_CHANNELS || !parse_hall_uint("pin", pin) ||
        !parse_hall_uint("lower", lower) || !parse_hall_uint("upper", upper) ||
        !parse_hall_uint("timeout_s", timeout) || lower >= upper || upper > 4095u ||
        timeout > MAX_HALL_TIMEOUT_S) {
        server.send(400, "application/json",
                    "{\"ok\":false,\"msg\":\"请提供有效通道、引脚及整数参数：0 ≤ 下限 < 上限 ≤ 4095，超时 0～36 秒\"}");
        return;
    }
    if (pin != static_cast<uint32_t>(IO_CONFIG.hall[channel]) ||
        !io_config_hall_pin_available(IO_CONFIG.hall[channel])) {
        server.send(409, "application/json", "{\"ok\":false,\"msg\":\"霍尔接线已变更或未配置，请刷新页面后重试\"}");
        return;
    }
    const HallDetectionConfig config = {static_cast<uint16_t>(lower), static_cast<uint16_t>(upper), timeout};
    const auto &previous = HALL_DETECTION[channel];
    if (previous.lower != config.lower || previous.upper != config.upper || previous.timeout_s != config.timeout_s) {
        SettingsData candidate = settings_snapshot();
        candidate.hall_detection[channel] = config;
        if (!settings_save_snapshot(candidate)) {
            server.send(500, "application/json", "{\"ok\":false,\"msg\":\"保存或回读校验失败，原检测设置仍生效，请重新保存\"}");
            return;
        }
        HALL_DETECTION[channel] = config;
        LOG_MAIN("[hall] 通道 %u 检测设置已保存：下限=%u，上限=%u，超时=%lu 秒（0=关闭）",
                 static_cast<unsigned>(channel + 1), static_cast<unsigned>(config.lower), static_cast<unsigned>(config.upper),
                 static_cast<unsigned long>(config.timeout_s));
    }
    String j = R"({"ok":true,"channel":)" + String(channel) + ",\"pin\":" + String(pin) + ",\"detection\":{";
    append_hall_detection_config(j, config);
    j += "}}";
    server.send(200, "application/json", j);
}

/**
 * 生成霍尔读数、统计、检测状态和增量历史快照
 * @param has_cursor 是否提供历史游标
 * @param requested_epoch 浏览器上次收到的历史纪元
 * @param requested_cursor 浏览器上次收到的历史游标
 * @param epoch 返回快照的纪元
 * @param delivered 返回本次最后一个历史游标
 * @param more 返回是否仍有历史待补齐
 * @return 完整霍尔 JSON；预留内存失败时为空
 * 每次最多返回 50 个时间桶，more 提示继续读取；纪元变化或游标失效时从现存最早数据重新分页
 * 只读取主循环缓存，不在 HTTP 回调中采样；传输年龄相对 now_ms，毫秒和游标回绕均按无符号差值处理
 */
static String hall_json(bool has_cursor, uint32_t requested_epoch, uint32_t requested_cursor,
                        uint32_t &epoch, uint32_t &delivered, bool &more) {
    const HallHistoryInfo info = hall_sensor_history_info();
    const HallDetectionStatus detection = hall_sensor_detection_status();
    const HallDetectionEvent event = hall_sensor_last_event();
    const auto current = BambuBus_current_channel_status();
    const uint32_t distance = info.cursor - requested_cursor;
    const bool reset = !has_cursor || requested_epoch != info.epoch || distance > info.count;
    const uint32_t remaining = reset ? info.count : distance;
    const uint32_t count = remaining > 50u ? 50u : remaining;
    const uint32_t first = reset ? info.cursor - info.count + 1u : requested_cursor + 1u;
    epoch = info.epoch;
    delivered = count ? first + count - 1u : info.cursor;
    more = remaining > count;
    HallChannelStatus channels[IO_CONFIG_MAX_CHANNELS];
    unsigned configured = 0;
    for (int channel = 0; channel < info.channels; ++channel) {
        channels[channel] = hall_sensor_status(channel);
        if (channels[channel].configured) ++configured;
    }
    String j;
    if (!j.reserve(384u + info.channels * 320u + configured * count * 28u)) return String();
    j += "{\"epoch\":" + String(info.epoch) + ",\"cursor\":" + String(delivered) +
        ",\"reset\":" + String(reset ? "true" : "false") +
        ",\"more\":" + String(remaining > count ? "true" : "false") +
        ",\"interval_ms\":" + String(HALL_HISTORY_INTERVAL_MS) +
        ",\"now_ms\":" + String(info.now_ms) +
        R"(,"event":{"sequence":)" + String(event.sequence) + ",\"channel\":" + String(event.channel) +
        R"(,"type":")" + String(event.type) + R"("},"channels":[)";
    for (int channel = 0; channel < info.channels; ++channel) {
        if (channel) j += ',';
        const auto &status = channels[channel];
        j += "{\"channel\":" + String(channel) + ",\"pin\":" + String(status.pin) + ",\"raw\":";
        append_hall_value(j, status.valid, status.raw);
        j += ",\"min\":"; append_hall_value(j, status.statistics_valid, status.minimum);
        j += ",\"max\":"; append_hall_value(j, status.statistics_valid, status.maximum);
        j += ",\"avg\":"; append_hall_value(j, status.statistics_valid, status.average);
        j += ",\"avg_1s\":"; append_hall_value(j, status.average_1s != HALL_INVALID_VALUE, status.average_1s);
        j += ",\"avg_5s\":"; append_hall_value(j, status.average_5s != HALL_INVALID_VALUE, status.average_5s);
        j += ",\"detection\":{";
        append_hall_detection_config(j, HALL_DETECTION[channel]);
        const bool monitored = detection.channel == channel;
        const char *state = monitored ? detection.state :
            current.channel == channel && strcmp(current.state, "pending") == 0 ? "pending" : "inactive";
        j += R"(,"state":")" + String(state) + R"(","idle_ms":)" + String(monitored ? detection.idle_ms : 0u) + "}";
        j += ",\"points\":[";
        if (status.configured) {
            for (uint32_t offset = 0; offset < count; ++offset) {
                if (offset) j += ',';
                const uint32_t cursor = first + offset;
                HallHistoryPoint point = {};
                const bool valid = hall_sensor_history_point(channel, cursor, point) &&
                                   point.average != HALL_INVALID_VALUE;
                const uint32_t age = info.now_ms - info.end_ms +
                                     (info.cursor - cursor) * HALL_HISTORY_INTERVAL_MS;
                j += '['; j += String(age); j += ',';
                append_hall_value(j, valid, point.average); j += ',';
                append_hall_value(j, valid, point.minimum); j += ',';
                append_hall_value(j, valid, point.maximum); j += ']';
            }
        }
        j += "]}";
    }
    j += "]}";
    return j;
}

/**
 * 处理 HTTP 霍尔查询
 * 严格校验成对的纪元与游标参数，按最多 50 个时间桶返回缓存快照
 */
static void handle_hall() {
    const bool has_cursor = server.hasArg("cursor");
    uint32_t cursor = 0, epoch = 0;
    if (has_cursor != server.hasArg("epoch") ||
        (has_cursor && (!parse_hall_uint("cursor", cursor) || !parse_hall_uint("epoch", epoch)))) {
        server.send(400, "application/json", "{\"ok\":false,\"msg\":\"霍尔游标与纪元须同时提供有效的无符号整数\"}");
        return;
    }
    uint32_t delivered = 0, response_epoch = 0;
    bool more = false;
    const String json = hall_json(has_cursor, epoch, cursor, response_epoch, delivered, more);
    server.sendHeader("Cache-Control", "no-store");
    server.send(json.isEmpty() ? 503 : 200, "application/json",
                json.isEmpty() ? "{\"ok\":false,\"msg\":\"可用内存不足，请稍后重试\"}" : json);
}

// WebSocket 任务只处理网络和订阅消息，业务快照仍由主循环生成
static constexpr uint8_t WS_CLIENTS = 4;
static httpd_handle_t ws_server = nullptr;
static char ws_token[33] = {};
static QueueHandle_t ws_events = nullptr;
static portMUX_TYPE ws_lock = portMUX_INITIALIZER_UNLOCKED;
static bool ws_worker_busy = false;
namespace {
    struct WsSession {
        int fd = -1;
        uint32_t generation = 0;
        bool closing = false;
        bool authenticated = false;
    };
    enum class WsEventType : uint8_t { Status, Hall, Ack };
    struct WsEvent {
        WsEventType type;
        uint8_t slot;
        uint32_t generation, id, epoch, cursor;
        bool has_cursor;
    };
    struct WsClient {
        uint32_t generation = 0, seen_ms = 0;
        uint32_t status_id = 0, status_started = 0, status_due = 0;
        uint32_t hall_id = 0, epoch = 0, cursor = 0, hall_due = 0;
        uint32_t pending_seq = 0, pending_id = 0, pending_since = 0;
        uint32_t pending_epoch = 0, pending_cursor = 0;
        bool has_cursor = false, pending_hall = false;
    };
    struct WsSend {
        uint8_t slot{};
        WsSession session;
        String payload;
    };
}
static WsSession ws_sessions[WS_CLIENTS];
static WsClient ws_clients[WS_CLIENTS];
static String ws_status_cache;
static uint32_t ws_status_ms = 0;
static bool ws_status_dirty = true;

/**
 * 复制网络任务维护的连接身份
 * @param slot 固定客户端槽号
 * @return 包含描述符和连接代次的快照
 */
static WsSession ws_session_snapshot(const uint8_t slot) {
    portENTER_CRITICAL(&ws_lock);
    const WsSession session = ws_sessions[slot];
    portEXIT_CRITICAL(&ws_lock);
    return session;
}

/**
 * 释放断开的固定会话槽
 * @param context HTTPD 保存的会话槽指针
 * 由网络任务调用，不依赖事件队列容量，也不释放静态内存
 */
static void ws_session_free(void *context) {
    auto *session = static_cast<WsSession *>(context);
    portENTER_CRITICAL(&ws_lock);
    session->fd = -1;
    portEXIT_CRITICAL(&ws_lock);
}

/**
 * 在网络任务中补齐短写并限制单次发送时间
 * @param handle HTTPD 服务句柄
 * @param fd 当前连接描述符
 * @param data 待发送数据
 * @param length 数据长度
 * @param flags socket 发送标志
 * @return 完整发送的字节数；超时或失败返回负值，调用方关闭连接
 */
static int ws_socket_send(const httpd_handle_t handle,const int fd, const char *data,const size_t length,const int flags) {
    (void)handle;
    size_t sent = 0;
    const uint32_t started = millis();
    while (sent < length) {
        const int count = send(fd, data + sent, length - sent, flags);
        if (count <= 0) return HTTPD_SOCK_ERR_FAIL;
        sent += static_cast<size_t>(count);
        if (sent < length && millis() - started >= 100u) return HTTPD_SOCK_ERR_TIMEOUT;
    }
    return static_cast<int>(sent);
}

/**
 * 读取 WebSocket 控制消息中的无符号整数
 * @param json 控制消息对象
 * @param name 字段名称
 * @param value 返回完整的 32 位整数
 * @return 字段为 0..UINT32_MAX 整数时返回 true
 */
static bool ws_read_uint(const cJSON *json, const char *name, uint32_t &value) {
    return Json::read_uint(json, name, value);
}

/**
 * 返回本次启动的 WebSocket 验证凭据
 * 浏览器通过现有 HTTP 同源及 localhost CORS 规则读取，服务未启动时返回 503
 */
static void handle_ws_token() {
    server.sendHeader("Cache-Control", "no-store");
    server.send(ws_server ? 200 : 503, "application/json", ws_server
                ? String(R"({"token":")") + ws_token + "\"}"
                : String("{\"ok\":false,\"msg\":\"WebSocket 尚未启动\"}"));
}

/**
 * 在 TCP 连接建立时初始化 WebSocket 会话
 * @param handle HTTPD 服务句柄
 * @param fd 新连接描述符
 * @return 会话和发送回调准备成功时返回 ESP_OK，否则关闭新连接
 * 不依赖新版 SDK 已移除的 GET 握手回调，未验证的连接不能订阅状态
 */
static esp_err_t ws_session_open(const httpd_handle_t handle,const  int fd) {
    static uint32_t generation = 0;
    for (auto & ws_session : ws_sessions) {
        if (ws_session.fd >= 0) continue;
        if (++generation == 0) ++generation;
        constexpr timeval timeout = {.tv_sec = 0, .tv_usec = 100000};
        if (setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) != 0 ||
            httpd_sess_set_send_override(handle, fd, ws_socket_send) != ESP_OK) return ESP_FAIL;
        portENTER_CRITICAL(&ws_lock);
        ws_session = {fd, generation, false, false};
        portEXIT_CRITICAL(&ws_lock);
        httpd_sess_set_ctx(handle, fd, &ws_session, ws_session_free);
        return ESP_OK;
    }
    return ESP_FAIL;
}

/**
 * 接收 WebSocket 验证、订阅和逐帧确认
 * @param request 当前 HTTPD 请求
 * @return 成功入队返回 ESP_OK；非法消息或队列已满时关闭连接
 * 接收上限为 192 字节，回调不读取业务快照、不执行电机动作
 */
static esp_err_t handle_ws(httpd_req_t *request) {
    if (request->method == HTTP_GET) return ESP_OK;
    auto *session = static_cast<WsSession *>(request->sess_ctx);
    if (!session || session->closing) return ESP_FAIL;
    httpd_ws_frame_t frame = {};
    if (httpd_ws_recv_frame(request, &frame, 0) != ESP_OK ||
        frame.type != HTTPD_WS_TYPE_TEXT || !frame.final || frame.len == 0 || frame.len > 192) return ESP_FAIL;
    char payload[193] = {};
    frame.payload = reinterpret_cast<uint8_t *>(payload);
    if (httpd_ws_recv_frame(request, &frame, sizeof(payload) - 1) != ESP_OK) return ESP_FAIL;
    auto json = Json::parse(payload, frame.len);
    String type;
    bool valid = Json::read_string(json.get(), "type", type);
    if (!session->authenticated) {
        String token;
        valid = valid && type == "hello" && Json::read_string(json.get(), "token", token) && token == ws_token;
        if (!valid) return ESP_FAIL;
        portENTER_CRITICAL(&ws_lock);
        session->authenticated = true;
        portEXIT_CRITICAL(&ws_lock);
        return ESP_OK;
    }
    WsEvent event = {};
    event.slot = static_cast<uint8_t>(session - ws_sessions);
    event.generation = session->generation;
    if (valid && type == "ack") {
        event.type = WsEventType::Ack;
        valid = ws_read_uint(json.get(), "seq", event.id) && event.id != 0;
    } else if (valid && type == "status") {
        event.type = WsEventType::Status;
        valid = ws_read_uint(json.get(), "id", event.id) && event.id != 0;
    } else if (valid && type == "hall") {
        event.type = WsEventType::Hall;
        event.has_cursor = Json::item(json.get(), "cursor") != nullptr;
        valid = ws_read_uint(json.get(), "id", event.id) &&
                event.has_cursor == (Json::item(json.get(), "epoch") != nullptr) &&
                (!event.has_cursor || (ws_read_uint(json.get(), "epoch", event.epoch) && ws_read_uint(json.get(), "cursor", event.cursor)));
    } else valid = false;
    json.reset();
    return valid && xQueueSend(ws_events, &event, 0) == pdTRUE ? ESP_OK : ESP_FAIL;
}

/**
 * 在 HTTPD 任务中发送快照或关闭指定代次的连接
 * @param argument 独占的发送工作及缓冲区
 * 发送前核对会话身份，描述符复用时丢弃旧工作；空正文表示关闭
 */
static void ws_send_work(void *argument) {
    auto *work = static_cast<WsSend *>(argument);
    auto &session = ws_sessions[work->slot];
    if (session.fd == work->session.fd && session.generation == work->session.generation &&
        !session.closing && httpd_sess_get_ctx(ws_server, session.fd) == &session) {
        httpd_ws_frame_t frame = {};
        frame.type = HTTPD_WS_TYPE_TEXT;
        frame.payload = reinterpret_cast<uint8_t *>(const_cast<char *>(work->payload.c_str()));
        frame.len = work->payload.length();
        if (frame.len == 0 || httpd_ws_send_frame_async(ws_server, session.fd, &frame) != ESP_OK) {
            portENTER_CRITICAL(&ws_lock);
            session.closing = true;
            portEXIT_CRITICAL(&ws_lock);
            shutdown(session.fd, SHUT_RDWR);
        }
    }
    delete work;
    portENTER_CRITICAL(&ws_lock);
    ws_worker_busy = false;
    portEXIT_CRITICAL(&ws_lock);
}

/**
 * 向网络任务转交唯一待发送缓冲区
 * @param slot 客户端槽号
 * @param payload 发送后转移所有权的正文；空正文表示关闭
 * @return 成功入队返回 true，失败后由主循环重试
 */
static bool ws_queue_send(const uint8_t slot, String &payload) {
    const WsSession session = ws_session_snapshot(slot);
    if (session.fd < 0 || session.closing || session.generation != ws_clients[slot].generation) return false;
    auto *work = new (std::nothrow) WsSend;
    if (!work) return false;
    work->slot = slot;
    work->session = session;
    work->payload = std::move(payload);
    portENTER_CRITICAL(&ws_lock);
    ws_worker_busy = true;
    portEXIT_CRITICAL(&ws_lock);
    if (httpd_queue_work(ws_server, ws_send_work, work) == ESP_OK) return true;
    delete work;
    portENTER_CRITICAL(&ws_lock);
    ws_worker_busy = false;
    portEXIT_CRITICAL(&ws_lock);
    return false;
}

/**
 * 使主循环订阅状态与网络连接代次保持一致
 * @param slot 客户端槽号
 * @param now 本轮主循环的毫秒时间
 * @return 当前槽仍连接且未开始关闭时返回 true
 */
static bool ws_sync_client(const uint8_t slot,const uint32_t now) {
    const WsSession session = ws_session_snapshot(slot);
    auto &client = ws_clients[slot];
    if (session.fd < 0 || session.closing) {
        client = {};
        return false;
    }
    if (client.generation != session.generation) {
        client = {};
        client.generation = session.generation;
        client.seen_ms = now;
    }
    return true;
}

/**
 * 在主循环中处理订阅并生成有界的实时快照
 * 每轮最多读取 16 个控制事件、安排一个发送；每客户端最多一帧等待确认
 * 状态每秒刷新且共用快照，霍尔每页最多 50 桶，确认后才推进历史游标
 */
static void websocket_run() {
    if (!ws_server) return;
    const uint32_t now = millis();
    WsEvent event{};
    for (uint8_t received = 0; received < 16 && xQueueReceive(ws_events, &event, 0) == pdTRUE; ++received) {
        if (!ws_sync_client(event.slot, now)) continue;
        auto &client = ws_clients[event.slot];
        if (client.generation != event.generation) continue;
        client.seen_ms = now;
        if (event.type == WsEventType::Status) {
            client.status_id = event.id;
            client.status_started = now;
            client.status_due = now;
            ws_status_dirty = true;
        } else if (event.type == WsEventType::Hall) {
            client.hall_id = event.id;
            client.has_cursor = event.has_cursor;
            client.epoch = event.epoch;
            client.cursor = event.cursor;
            client.hall_due = now;
        } else if (client.pending_seq == event.id) {
            if (client.pending_hall && client.hall_id == client.pending_id) {
                client.has_cursor = true;
                client.epoch = client.pending_epoch;
                client.cursor = client.pending_cursor;
            }
            client.pending_seq = 0;
        }
    }
    portENTER_CRITICAL(&ws_lock);
    const bool busy = ws_worker_busy;
    portEXIT_CRITICAL(&ws_lock);
    if (busy) return;
    static uint8_t next_slot = 0;
    static uint32_t sequence = 0;
    for (uint8_t checked = 0; checked < WS_CLIENTS; ++checked) {
        const uint8_t slot = next_slot;
        next_slot = (next_slot + 1) % WS_CLIENTS;
        if (!ws_sync_client(slot, now)) continue;
        auto &client = ws_clients[slot];
        String payload;
        if ((client.pending_seq && now - client.pending_since >= 5000u) || now - client.seen_ms >= 10000u) {
            ws_queue_send(slot, payload);
            return;
        }
        if (client.pending_seq) continue;
        const bool hall = client.hall_id && static_cast<int32_t>(now - client.hall_due) >= 0;
        const bool status = client.status_id && static_cast<int32_t>(now - client.status_due) >= 0 &&
                            (!hall || static_cast<int32_t>(client.status_due - client.hall_due) <= 0);
        if (!status && !hall) continue;
        String hall_data;
        const String *data;
        uint32_t epoch = 0, cursor = 0;
        bool more = false;
        if (status) {
            if (ws_status_dirty || now - ws_status_ms >= 1000u) {
                ws_status_ms = now;
                ws_status_cache = status_json();
                ws_status_dirty = false;
            }
            data = &ws_status_cache;
        } else {
            hall_data = hall_json(client.has_cursor, client.epoch, client.cursor, epoch, cursor, more);
            data = &hall_data;
        }
        if (data->isEmpty() || !payload.reserve(data->length() + 128u)) {
            ws_queue_send(slot, payload);
            return;
        }
        if (++sequence == 0) ++sequence;
        const uint32_t id = status ? client.status_id : client.hall_id;
        payload = R"({"type":")" + String(status ? "status" : "hall") + R"(","id":)" + String(id) +
                  ",\"seq\":" + String(sequence);
        if (status) payload += ",\"elapsed_ms\":" + String(ws_status_ms - client.status_started);
        payload += ",\"data\":";
        payload += *data;
        payload += '}';
        if (!ws_queue_send(slot, payload)) return;
        client.pending_seq = sequence;
        client.pending_id = id;
        client.pending_since = now;
        client.pending_hall = !status;
        client.pending_epoch = epoch;
        client.pending_cursor = cursor;
        if (status) client.status_due = now + 1000u;
        else client.hall_due = now + (more ? 50u : 1000u);
        return;
    }
}

/**
 * 启动独立端口上的 WebSocket 服务
 * 使用 SDK 自带 HTTPD 任务，连接、接收及发送资源有界，启动失败仍保留 HTTP 管理
 */
static void websocket_init() {
    uint8_t token_bytes[16];
    esp_fill_random(token_bytes, sizeof(token_bytes));
    for (size_t i = 0; i < sizeof(token_bytes); ++i)
        snprintf(ws_token + i * 2, 3, "%02x", static_cast<unsigned>(token_bytes[i]));
    ws_events = xQueueCreate(16, sizeof(WsEvent));
    if (!ws_events) return;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 81;
    config.max_open_sockets = WS_CLIENTS;
    config.max_uri_handlers = 1;
    config.recv_wait_timeout = 1;
    config.send_wait_timeout = 1;
    config.open_fn = ws_session_open;
    httpd_uri_t route = {};
    route.uri = "/api/ws";
    route.method = HTTP_GET;
    route.handler = handle_ws;
    route.is_websocket = true;
    if (httpd_start(&ws_server, &config) == ESP_OK && httpd_register_uri_handler(ws_server, &route) == ESP_OK) {
        LOG_MAIN("[web] WebSocket 服务已启动，端口 81");
        return;
    }
    if (ws_server) httpd_stop(ws_server);
    ws_server = nullptr;
    vQueueDelete(ws_events);
    ws_events = nullptr;
    LOG_MAIN("[web] WebSocket 启动失败，请使用 HTTP 轮询");
}

/**
 * 处理设置读取请求
 * 输出时长、五通压下电平、AMS 身份、通道模式、霍尔检测、局域网参数、两种电机接线及可选引脚；返回已保存的访问码供设置页直接显示
 */
static void handle_settings_get() {
    String j = R"({"rs485":)" + String(RS485_MODE) + ",\"buslog\":" + String(BUS_LOG_ENABLE) +
               ",\"ams\":" + String(AMS_COUNT) + ",\"manual_ms\":" + String(MANUAL_PULSE_MS) +
               ",\"auto_ms\":" + String(AUTO_LOAD_MS) +
               ",\"feed_stop_delay_ms\":" + String(FEED_STOP_DELAY_MS) +
               ",\"total_switch_active_level\":" + String(TOTAL_SWITCH_ACTIVE_LEVEL) +
               ",\"total_switch_feed_ms\":" + String(TOTAL_SWITCH_FEED_MS) +
               ",\"motor_limit\":" + String(MOTOR_LIMIT) +
               ",\"effective_motor_limit\":" + String(BMCU_motor_limit()) +
               ",\"channels\":" + String(BMCU_motor_count()) +
               ",\"maxams\":" + String(BMCU_max_ams_count()) + ",\"ams_identity\":[";
    for (int ams = 0; ams < AMS_IDENTITY_COUNT; ++ams) {
        if (ams) j += ',';
        char serial[AMS_SERIAL_LENGTH + 1] = {};
        settings_get_ams_serial(ams, serial);
        j += "{\"index\":" + String(ams) + R"(,"serial":")" + Json::escape(CUSTOM_SERIAL[ams]) +
             R"(","effective_serial":")" + String(serial) + R"(","ams_version":")" +
             String(AMS_VERSION[ams][0]) + "." + String(AMS_VERSION[ams][1]) + "." +
             String(AMS_VERSION[ams][2]) + "." + String(AMS_VERSION[ams][3]) +
             R"(","enabled":)" + String(ams < AMS_COUNT ? "true" : "false") + "}";
    }
    j += "],\"to\":[";
    for (int i = 0; i < BMCU_motor_count(); i++) { if (i) j += ','; j += String(PULLBACK_TIMEOUT[i]); }
    j += "],\"cm\":[";
    for (int i = 0; i < BMCU_motor_count(); i++) { if (i) j += ','; j += String(CHANNEL_MODE[i]); }
    j += "],\"hall_detection\":[";
    for (int i = 0; i < active_channel_count(); ++i) {
        if (i) j += ',';
        j += '{';
        append_hall_detection_config(j, HALL_DETECTION[i]);
        j += '}';
    }
    j += R"(],"otaurl":")" + Json::escape(OTA_URL) + R"(","otanotify":)" + String(OTA_NOTIFY);
    j += R"(,"lan_ams":{"printer_ip":")" + Json::escape(LAN_AMS_CONFIG.printer_ip) +
         R"(","serial":")" + Json::escape(LAN_AMS_CONFIG.serial) +
         R"(","access_code":")" + Json::escape(LAN_AMS_CONFIG.access_code) +
         R"(","feed_ms":)" + String(LAN_AMS_CONFIG.feed_ms) +
         ",\"nozzle_temp\":" + String(LAN_AMS_CONFIG.nozzle_temp) + "}";
    j += R"(,"io":{"motor":[)";
    for (int i = 0; i < IO_CONFIG_MAX_CHANNELS; i++) {
        if (i) j += ',';
        j += "[" + String(IO_CONFIG.motor[i][0]) + "," + String(IO_CONFIG.motor[i][1]) + "]";
    }
    j += "],\"clutch_enabled\":" + String(IO_CONFIG.clutch_enabled) +
         ",\"shared_motor\":[" + String(IO_CONFIG.shared_motor[0]) + "," + String(IO_CONFIG.shared_motor[1]) +
         "],\"clutch_active_level\":" + String(IO_CONFIG.clutch_active_level) +
         ",\"clutch_engage_ms\":" + String(IO_CONFIG.clutch_engage_ms) +
         ",\"clutch_release_ms\":" + String(IO_CONFIG.clutch_release_ms) + ",\"clutch\":[";
    for (int i = 0; i < IO_CONFIG_MAX_CHANNELS; i++) {
        if (i) j += ',';
        j += String(IO_CONFIG.clutch[i]);
    }
    j += "],\"microswitch\":[";
    for (int i = 0; i < IO_CONFIG_MAX_CHANNELS; i++) {
        if (i) j += ',';
        j += String(IO_CONFIG.microswitch[i]);
    }
    j += "],\"hall\":[";
    for (int i = 0; i < IO_CONFIG_MAX_CHANNELS; i++) {
        if (i) j += ',';
        j += String(IO_CONFIG.hall[i]);
    }
    j += "],\"hall_pins\":";
    io_config_append_hall_pin_options(j);
    j += ",\"switch_pin\":" + String(IO_CONFIG.switch_pin) + ",\"pins\":";
    io_config_append_pin_options(j);
    j += R"(},"pcf":{"configured":)" + String(i2c_bus_configured() ? "true" : "false") +
         ",\"sda\":" + String(PCF_SDA) + ",\"scl\":" + String(PCF_SCL) + ",\"pins\":";
    io_config_append_pin_options(j, true);
    j += R"(},"bus":{"rx":)" + String(BUS_RX) + ",\"tx\":" + String(BUS_TX) +
         ",\"de\":" + String(BUS_DE) + ",\"pins\":";
    io_config_append_pin_options(j, true);
    j += "}}";
    server.sendHeader("Cache-Control", "no-store");
    server.send(200, "application/json", j);
}

// toInt() 会把非法字符串当作 0；修改参数前先完整校验，避免部分保存
/**
 * 校验请求参数是否为范围内整数
 * @param key 参数名
 * @param low 允许的最小值
 * @param high 允许的最大值
 * @return true 表示参数缺失或数值合法
 * 先否定 toInt() 的容错行为，避免非法字符串被当作 0 保存
 */
static bool valid_number(const String &key,const  int low,const  int high) {
    if (!server.hasArg(key)) return true;
    const String raw = server.arg(key);
    char *end;
    const long value = strtol(raw.c_str(), &end, 10);
    return end != raw.c_str() && !*end && value >= low && value <= high;
}

/**
 * 解析并校验引脚参数
 * @param key 参数名
 * @param value 输出的引脚编号
 * @param allow_none 是否允许 PIN_NONE
 * @return true 表示参数存在且引脚可用
 * 把请求中的十进制引脚号解析为整数并调用 io_config_pin_available()
 */
static bool parse_io_pin(const String &key, int &value,const  bool allow_none) {
    if (!server.hasArg(key)) return false;
    const String raw = server.arg(key);
    char *end = nullptr;
    const long parsed = strtol(raw.c_str(), &end, 10);
    if (end == raw.c_str() || *end || parsed < 0 || parsed > 255) return false;
    value = static_cast<int>(parsed);
    return io_config_pin_available(value, allow_none);
}

/**
 * 解析候选电机接线，保留未启用模式的引脚草稿
 * @param key 参数名称
 * @param value 输出引脚编号
 * @param active 是否属于当前候选模式的生效接线
 * @return true 表示整数格式合法且生效引脚可用
 * 未启用模式只校验存储范围，切换模式时再检查 GPIO 能力和当前总线占用
 */
static bool parse_motor_io_pin(const String &key, int &value,const  bool active) {
    if (active) return parse_io_pin(key, value, true);
    if (!server.hasArg(key) || !valid_number(key, 0, 255)) return false;
    value = server.arg(key).toInt();
    return true;
}

// I2C 总线配置：SDA/SCL 同时有效才启用，同时留空表示关闭；保存后重启生效
/**
 * 解析 I2C 引脚参数
 * @param key 请求参数名
 * @param value 输出引脚号，PIN_NONE 表示“无”
 * @return true 表示参数存在且格式合法
 * 接受 0-255 的数字或 none/255（表示未配置）
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
 * 校验 SDA/SCL 必须同时配置或同时留空，清理冲突通道引脚，写入设置并重启
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
    // 其余接线保留，用户已有接线不会被整套冲掉
    const int cleared = io_config_clear_invalid_pins(IO_CONFIG, configured, PCF_SDA, PCF_SCL);
    String error;
    bool wiring_ok = !configured ||
        (io_config_i2c_validate(PCF_SDA, PCF_SCL, IO_CONFIG, active_channel_count(), &error) &&
         io_config_bus_validate(BUS_RX, BUS_TX, BUS_DE, RS485_MODE, PCF_SDA, PCF_SCL,
                                IO_CONFIG, active_channel_count(), &error));
    if (wiring_ok) wiring_ok = io_config_validate(IO_CONFIG, active_channel_count(), &error);
    if (!wiring_ok) {
        settings_restore_snapshot(previous);
        server.send(400, "application/json", Json::message(false, error.c_str()));
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
    server.send(200, "application/json", Json::message(true, msg.c_str()));
    delay(300);
    ESP.restart();
}

/**
 * 保存打印机接线与局域网配对配置
 * 双空 RX/TX 启用局域网；候选配置校验及写入回读成功后重启，运行期仍保留原接线和凭据
 * 未提交的凭据保持原值，显式清空时按三个字段全空校验
 */
static void handle_bus_save() {
    int rx = PIN_NONE, tx = PIN_NONE;
    if (!parse_i2c_pin("rx", rx) || !parse_i2c_pin("tx", tx) || !server.hasArg("mode") ||
        !valid_number("mode", 1, 2) || !valid_number("pair", 0, 1)) {
        server.send(400, "application/json", "{\"ok\":false,\"msg\":\"请完整填写接线，RX/TX 必须同时配置或同时为无\"}");
        return;
    }
    const bool lan = rx == PIN_NONE && tx == PIN_NONE;
    const bool pair = server.arg("pair") == "1";
    const int mode = server.arg("mode").toInt();
    if ((!lan && mode == RS485_MODE_DE && (!server.hasArg("de") || !valid_number("de", 0, 48))) ||
        (pair && !lan)) {
        server.send(400, "application/json", "{\"ok\":false,\"msg\":\"模式 2 必须指定 DE，局域网配对需要 RX/TX 同时为无\"}");
        return;
    }
    const int de = !lan && mode == RS485_MODE_DE ? server.arg("de").toInt() : BUS_DE;
    String error;
    if (!io_config_bus_validate(rx, tx, de, mode, PCF_SDA, PCF_SCL, IO_CONFIG, active_channel_count(), &error)) {
        server.send(400, "application/json", Json::message(false, error.c_str()));
        return;
    }
    SettingsData candidate = settings_snapshot();
    candidate.bus_rx = rx; candidate.bus_tx = tx; candidate.bus_de = de; candidate.rs485_mode = mode;
    if (lan) {
        String ip = server.hasArg("lan_ip") ? server.arg("lan_ip") : candidate.lan_ams.printer_ip;
        String serial = server.hasArg("lan_serial") ? server.arg("lan_serial") : candidate.lan_ams.serial;
        String code = server.hasArg("lan_access_code") ? server.arg("lan_access_code") : candidate.lan_ams.access_code;
        ip.trim(); serial.trim(); code.trim();
        if (ip.length() >= sizeof(candidate.lan_ams.printer_ip) ||
            serial.length() >= sizeof(candidate.lan_ams.serial) || code.length() >= sizeof(candidate.lan_ams.access_code) ||
            !valid_number("lan_feed_ms", 100, MAX_PULSE_MS - 1) || !valid_number("lan_nozzle_temp", 170, 300)) {
            server.send(400, "application/json", "{\"ok\":false,\"msg\":\"局域网参数无效，进料时间为 100～29999 毫秒，温度为 170～300 ℃\"}");
            return;
        }
        copy_str(candidate.lan_ams.printer_ip, sizeof(candidate.lan_ams.printer_ip), ip.c_str());
        copy_str(candidate.lan_ams.serial, sizeof(candidate.lan_ams.serial), serial.c_str());
        copy_str(candidate.lan_ams.access_code, sizeof(candidate.lan_ams.access_code), code.c_str());
        if (server.hasArg("lan_feed_ms")) candidate.lan_ams.feed_ms = server.arg("lan_feed_ms").toInt();
        if (server.hasArg("lan_nozzle_temp")) candidate.lan_ams.nozzle_temp = server.arg("lan_nozzle_temp").toInt();
        if (!settings_lan_config_valid(candidate.lan_ams, pair)) {
            server.send(400, "application/json", "{\"ok\":false,\"msg\":\"请输入有效 IPv4 地址、8 位字母数字访问码和打印机序列号；未配对时三个字段可全部留空\"}");
            return;
        }
    }
    if (!maintenance_required()) return;
    if (!settings_save_snapshot(candidate)) {
        server.send(500, "application/json", "{\"ok\":false,\"msg\":\"保存或回读校验失败，未切换通信配置，请重试\"}");
        return;
    }
    LOG_MAIN("[web] 打印机通信配置已保存：%s，重启生效", lan ? "局域网" : "BambuBus");
    server.send(200, "application/json", lan
        ? "{\"ok\":true,\"msg\":\"局域网配置已保存，重启后连接打印机，请等待配对状态\"}"
        : "{\"ok\":true,\"msg\":\"BambuBus 配置已保存，设备正在重启；请断电后按新引脚接线\"}");
    delay(300);
    ESP.restart();
}

/**
 * 提交局域网当前装料通道确认
 * 仅在已配对且允许维护时接受有效通道或 -1；实际状态更新交给主循环，不启动电机
 */
static void handle_lan_channel() {
    if (!server.hasArg("channel") || !valid_number("channel", -1, active_channel_count() - 1)) {
        server.send(400, "application/json", "{\"ok\":false,\"msg\":\"当前装料通道无效\"}");
        return;
    }
    if (!lan_ams_enabled() || !lan_ams_status().paired) {
        server.send(409, "application/json", "{\"ok\":false,\"msg\":\"请先完成局域网配对\"}");
        return;
    }
    if (!maintenance_required()) return;
    const int channel = server.arg("channel").toInt();
    if ((channel >= 0 && !BMCU_channel_motor_ready(channel)) || !lan_ams_request_channel(channel)) {
        server.send(409, "application/json", "{\"ok\":false,\"msg\":\"通道未配置或局域网换料正在处理，请稍后重试\"}");
        return;
    }
    server.send(202, "application/json", "{\"ok\":true,\"msg\":\"通道确认请求已提交，不会启动电机\"}");
}

/**
 * 提交局域网换料停止请求
 * 不在 Web 回调中停车，由主循环取消换料并清除当前通道确认
 */
static void handle_lan_cancel() {
    if (!lan_ams_enabled()) {
        server.send(409, "application/json", "{\"ok\":false,\"msg\":\"当前未启用局域网换料\"}");
        return;
    }
    lan_ams_request_cancel();
    server.send(202, "application/json", "{\"ok\":true,\"msg\":\"停止请求已提交，请检查耗材后重新确认当前通道\"}");
}

/**
 * 处理常规设置保存
 * 身份字段必须指定 AMS 编号；校验时长、五通压下电平、数量、并发、身份及生效模式接线，保存失败时回滚完整快照
 * 拒绝未知或重复字段；接线字段出现时须完整提交生效模式的驱动、微动和霍尔字段，保留另一模式接线，配置由主循环应用
 */
static void handle_settings_save() {
    if (server.hasArg("rs485")) {
        server.send(400, "application/json", "{\"ok\":false,\"msg\":\"请刷新页面，在 BambuBus 接线中同时保存模式与引脚并重启\"}");
        return;
    }
    const bool identity_requested = server.hasArg("serial") || server.hasArg("ams_version");
    if (identity_requested != server.hasArg("ams_index") ||
        !valid_number("ams_index", 0, AMS_IDENTITY_COUNT - 1)) {
        server.send(400, "application/json",
                    "{\"ok\":false,\"msg\":\"请刷新页面；序列号或版本须同时指定 AMS 编号 0..3，编号不能单独提交\"}");
        return;
    }
    const int identity_index = identity_requested ? server.arg("ams_index").toInt() : 0;
    const SettingsData previous = settings_snapshot();
    bool timeout_changed[IO_CONFIG_MAX_CHANNELS] = {};
    uint32_t timeout_previous_ms[IO_CONFIG_MAX_CHANNELS] = {};
    const bool ams_valid = valid_number("ams", 1, BMCU_max_ams_count());
    const int candidate_ams = ams_valid && server.hasArg("ams") ? server.arg("ams").toInt() : AMS_COUNT;
    const int candidate_channels = candidate_ams * 4;
    const bool count_changed = candidate_ams != AMS_COUNT;
    for (int arg = 0; arg < server.args(); ++arg) {
        const String key = server.argName(arg);
        bool known = key == "ams" || key == "ams_index" || key == "serial" || key == "ams_version" ||
                     key == "manual_ms" || key == "auto_ms" || key == "feed_stop_delay_ms" ||
                     key == "total_switch_active_level" || key == "total_switch_feed_ms" ||
                     key == "motor_limit" || key == "buslog" || key == "otaurl" || key == "otanotify" ||
                     key.startsWith("io_") || key.startsWith("io-");
        for (int ch = 0; ch < candidate_channels; ++ch)
            if (key == "to" + String(ch) || key == "cm" + String(ch)) known = true;
        for (int other = 0; other < arg; ++other)
            if (server.argName(other) == key) known = false;
        if (!known) {
            server.send(400, "application/json", "{\"ok\":false,\"msg\":\"设置字段未知、重复或通道已停用，请刷新页面后重试\"}");
            return;
        }
    }
    bool valid = ams_valid &&
                 valid_number("manual_ms", 1, MAX_PULSE_MS - 1) &&
                 valid_number("auto_ms", 0, MAX_PULSE_MS - 1) &&
                 valid_number("feed_stop_delay_ms", 0, MAX_PULSE_MS - 1) &&
                 valid_number("total_switch_active_level", LOW, HIGH) &&
                 valid_number("total_switch_feed_ms", 1, MAX_PULSE_MS - 1) &&
                 valid_number("io_clutch_enabled", 0, 1) &&
                 valid_number("io_clutch_active_level", LOW, HIGH) &&
                 valid_number("io_clutch_engage_ms", 0, MAX_CLUTCH_DELAY_MS) &&
                 valid_number("io_clutch_release_ms", 0, MAX_CLUTCH_DELAY_MS) &&
                 valid_number("motor_limit", 1, candidate_channels);
    bool serial_valid = true;
    bool version_valid = true;
    int candidate_version[4] = {};
    memcpy(candidate_version, AMS_VERSION[identity_index], sizeof(candidate_version));
    if (server.hasArg("serial"))
        serial_valid = settings_custom_serial_valid(server.arg("serial").c_str());
    if (server.hasArg("ams_version"))
        version_valid = settings_parse_ams_version(server.arg("ams_version").c_str(), candidate_version);
    valid = valid && serial_valid && version_valid;
    for (int i = 0; i < candidate_channels; i++)
        valid = valid && valid_number("to" + String(i), 3, 30) && valid_number("cm" + String(i), 0, 2);

    bool io_requested = false;
    bool io_fields_valid = true;
    IoConfig candidate_io = IO_CONFIG;
    if (valid) {
        if (server.hasArg("io_clutch_enabled")) candidate_io.clutch_enabled = server.arg("io_clutch_enabled").toInt();
        if (server.hasArg("io_clutch_active_level")) candidate_io.clutch_active_level = server.arg("io_clutch_active_level").toInt();
        if (server.hasArg("io_clutch_engage_ms")) candidate_io.clutch_engage_ms = server.arg("io_clutch_engage_ms").toInt();
        if (server.hasArg("io_clutch_release_ms")) candidate_io.clutch_release_ms = server.arg("io_clutch_release_ms").toInt();
    }
    // 先识别完整请求，不能在逐通道解析过程中才发现 IO 字段
    // 同时拒绝旧页面的 io-* 拼写和未知字段，避免忽略修改却返回保存成功
    for (int arg = 0; arg < server.args(); arg++) {
        const String key = server.argName(arg);
        if (!key.startsWith("io_") && !key.startsWith("io-")) continue;
        io_requested = true;
        bool known = key == "io_switch" || key == "io_clutch_enabled" || key == "io_clutch_active_level" ||
                     key == "io_clutch_engage_ms" || key == "io_clutch_release_ms" ||
                     key == "io_shared_mf" || key == "io_shared_mb";
        for (int ch = 0; ch < candidate_channels; ch++) {
            if (key == "io_sw" + String(ch) || key == "io_h" + String(ch) ||
                key == "io_clutch" + String(ch) || key == "io_mf" + String(ch) || key == "io_mb" + String(ch)) known = true;
        }
        if (!known) io_fields_valid = false;
    }
    if (io_requested) {
        for (const char *key : {"io_switch", "io_clutch_enabled", "io_clutch_active_level",
                                "io_clutch_engage_ms", "io_clutch_release_ms"})
            if (!server.hasArg(key)) io_fields_valid = false;
        for (int direction = 0; direction < 2; ++direction) {
            const char *key = direction ? "io_shared_mb" : "io_shared_mf";
            int pin = PIN_NONE;
            if (server.hasArg(key) && parse_motor_io_pin(key, pin, candidate_io.clutch_enabled))
                candidate_io.shared_motor[direction] = pin;
            else if (server.hasArg(key)) valid = false;
            if (candidate_io.clutch_enabled && !server.hasArg(key)) io_fields_valid = false;
        }
    }
    if (server.hasArg("io_switch")) {
        if (int pin = PIN_NONE; parse_io_pin("io_switch", pin, true)) candidate_io.switch_pin = pin;
        else valid = false;
    }
    for (int i = 0; i < candidate_channels; i++) {
        const String k_forward = "io_mf" + String(i);
        const String k_reverse = "io_mb" + String(i);
        const String k_switch = "io_sw" + String(i);
        const String k_hall = "io_h" + String(i);
        const String k_clutch = "io_clutch" + String(i);
        int pin = 0;
        // 电机引脚也允许选「无」：切换 PCF8575 后被清空的通道可以先留空，
        // 该通道只是暂时不能进退料，接好线再来补
        if (server.hasArg(k_forward) && parse_motor_io_pin(k_forward, pin, !candidate_io.clutch_enabled)) candidate_io.motor[i][0] = pin;
        else if (server.hasArg(k_forward)) valid = false;
        if (server.hasArg(k_reverse) && parse_motor_io_pin(k_reverse, pin, !candidate_io.clutch_enabled)) candidate_io.motor[i][1] = pin;
        else if (server.hasArg(k_reverse)) valid = false;
        if (server.hasArg(k_clutch) && parse_motor_io_pin(k_clutch, pin, candidate_io.clutch_enabled)) candidate_io.clutch[i] = pin;
        else if (server.hasArg(k_clutch)) valid = false;
        if (server.hasArg(k_switch) && parse_io_pin(k_switch, pin, true)) candidate_io.microswitch[i] = pin;
        else if (server.hasArg(k_switch)) valid = false;
        if (server.hasArg(k_hall) && parse_io_pin(k_hall, pin, true) &&
            io_config_hall_pin_available(pin, true)) candidate_io.hall[i] = pin;
        else if (server.hasArg(k_hall)) valid = false;
        if (io_requested && (!server.hasArg(k_switch) || !server.hasArg(k_hall) ||
            (candidate_io.clutch_enabled ? !server.hasArg(k_clutch) :
             !server.hasArg(k_forward) || !server.hasArg(k_reverse)))) io_fields_valid = false;
    }
    if (!io_fields_valid) {
        server.send(400, "application/json",
                    "{\"ok\":false,\"msg\":\"IO 配置字段错误或不完整，请刷新页面后重新提交全部通道引脚\"}");
        return;
    }
    String io_error;
    if ((io_requested || count_changed) && valid && !io_config_validate(candidate_io, candidate_channels, &io_error)) valid = false;
    if (!valid) {
        String error_message = io_error.length() ? io_error : String("参数无效，请检查设置项");
        if (!version_valid) error_message = "AMS 版本必须为 A.B.C.D，四段均在 0..255";
        else if (!serial_valid) error_message = "序列号后缀必须为空或 12 位大写十六进制字符（0-9、A-F）";
        server.send(400, "application/json", Json::message(false, error_message.c_str()));
        return;
    }
    // 减少 AMS 时保留剩余通道的运动，停用通道由应用配置时停止并移出队列
    const bool io_changed = memcmp(&candidate_io, &IO_CONFIG, sizeof(IO_CONFIG)) != 0;
    if ((io_changed || candidate_ams > AMS_COUNT) && !BMCU_can_maintain()) {
        server.send(409, "application/json", "{\"ok\":false,\"msg\":\"请先停止所有电机运动和当前通道动作\"}");
        return;
    }
    const int candidate_limit = candidate_io.clutch_enabled ? 1 : server.hasArg("motor_limit")
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
        serial.toCharArray(CUSTOM_SERIAL[identity_index], sizeof(CUSTOM_SERIAL[identity_index]));
    }
    if (server.hasArg("ams_version"))
        memcpy(AMS_VERSION[identity_index], candidate_version, sizeof(candidate_version));
    AMS_COUNT = candidate_ams;
    MOTOR_LIMIT = min(MOTOR_LIMIT, candidate_channels);
    if (server.hasArg("manual_ms")) MANUAL_PULSE_MS = server.arg("manual_ms").toInt();
    if (server.hasArg("auto_ms")) AUTO_LOAD_MS = server.arg("auto_ms").toInt();
    if (server.hasArg("feed_stop_delay_ms")) FEED_STOP_DELAY_MS = server.arg("feed_stop_delay_ms").toInt();
    if (server.hasArg("total_switch_active_level")) TOTAL_SWITCH_ACTIVE_LEVEL = server.arg("total_switch_active_level").toInt();
    if (server.hasArg("total_switch_feed_ms")) TOTAL_SWITCH_FEED_MS = server.arg("total_switch_feed_ms").toInt();
    if (io_requested) IO_CONFIG = candidate_io;
    for (int i = 0; i < candidate_channels; i++) {
        String kt = "to" + String(i), kc = "cm" + String(i);
        if (server.hasArg(kt)) {
            if (int v = server.arg(kt).toInt(); v >= 3 && v <= 30) {
                if (v != PULLBACK_TIMEOUT[i]) {
                    timeout_changed[i] = true;
                    timeout_previous_ms[i] = static_cast<uint32_t>(PULLBACK_TIMEOUT[i]) * 1000u;
                }
                PULLBACK_TIMEOUT[i] = v;
            }
        }
        if (server.hasArg(kc)) {
            if (int v = server.arg(kc).toInt(); v >= 0 && v <= 2) CHANNEL_MODE[i] = v;
        }
    }
    if (server.hasArg("otaurl")) {
        String u = server.arg("otaurl");
        u.trim();
        u.toCharArray(OTA_URL, OTA_URL_MAX);
    }
    if (server.hasArg("otanotify")) OTA_NOTIFY = server.arg("otanotify").toInt() ? 1 : 0;
    if (!settings_save_to_flash()) {
        // 硬件尚未应用候选接线；回退完整内存快照，保持数量、接线和运行状态一致
        settings_restore_snapshot(previous);
        BMCU_apply_channel_modes();
        server.send(500, "application/json", "{\"ok\":false,\"msg\":\"保存失败，请重试\"}");
        return;
    }
    if (io_changed || count_changed || previous.total_switch_active_level != TOTAL_SWITCH_ACTIVE_LEVEL) BMCU_apply_io_config();
    BMCU_apply_channel_modes();
    for (int i = 0; i < candidate_channels; i++) {
        if (timeout_changed[i]) BMCU_apply_pullback_timeout(i, timeout_previous_ms[i]);
    }
    LOG_MAIN("[web] 设置已保存");
    int bus_log_before = BUS_LOG_ENABLE;
    if (BUS_LOG_ENABLE != bus_log_before) {
        LOG_MAIN("[web] BambuBus 原始报文日志已%s", BUS_LOG_ENABLE ? "启用" : "关闭");
    }
    settings_log_all();
    server.send(200, "application/json", "{\"ok\":true,\"msg\":\"参数已保存\"}");
}

/**
 * 处理手动电机控制请求
 * 方向为 0 时登记停止请求；否则校验通道和时长后提交点动，全部由主循环驱动电机
 * 停止保留打印机协议动作和回复，重复进料不重启电机；收到有效非进料动作及新的进料请求后才恢复，手动点动仍可用
 * 驱动未就绪时按当前模式区分共享电机、通道离合接线缺失和所属扩展芯片离线
 */
static void handle_manual_motor() {
    if (!server.hasArg("motor") || !server.hasArg("dir") ||
        !valid_number("motor", 0, BMCU_motor_count() - 1) || !valid_number("dir", -1, 1)) {
        server.send(400, "application/json", "{\"ok\":false,\"msg\":\"电机编号或方向无效\"}");
        return;
    }
    int motor = server.arg("motor").toInt(), dir = server.arg("dir").toInt();
    if (!dir) {
        BMCU_request_stop_motor(motor);
        LOG_MAIN("[web] 已请求停止通道 %d", motor + 1);
        server.send(202, "application/json", "{\"ok\":true,\"msg\":\"停止请求已提交\"}");
        return;
    }
    if (!valid_number("duration", 1, MAX_PULSE_MS - 1)) {
        server.send(400, "application/json", "{\"ok\":false,\"msg\":\"点动时间无效\"}");
        return;
    }
    int duration = server.hasArg("duration") ? server.arg("duration").toInt() : MANUAL_PULSE_MS;
    if (!BMCU_channel_motor_ready(motor)) {
        const bool clutch = IO_CONFIG.clutch_enabled != 0;
        const int32_t *pins = clutch ? IO_CONFIG.shared_motor : IO_CONFIG.motor[motor];
        String error;
        if (pins[0] == PIN_NONE || pins[1] == PIN_NONE)
            error = clutch ? "共享电机的进料或退料 IO 未配置，请先到电机引脚设置中补全" :
                             "该通道的进料或退料 IO 未配置，请先到电机引脚设置中补全";
        else if (clutch && IO_CONFIG.clutch[motor] == PIN_NONE)
            error = "该通道的离合 IO 未配置，请先到电机引脚设置中补全";
        for (int direction = 0; !error.length() && direction < 2; ++direction)
            if (IS_PCF8575_VIRTUAL_PIN(pins[direction]) && !PCF8575_pin_is_online(pins[direction]))
                error = String(clutch ? "共享电机" : "该通道电机") + "的 " + io_config_pin_label(pins[direction]) +
                        " 所属 PCF8575 已离线，请检查扩展模块接线和供电";
        for (int channel = 0; clutch && !error.length() && channel < active_channel_count(); ++channel)
            if (IS_PCF8575_VIRTUAL_PIN(IO_CONFIG.clutch[channel]) && !PCF8575_pin_is_online(IO_CONFIG.clutch[channel]))
                error = "通道 " + String(channel + 1) + " 离合的 " + io_config_pin_label(IO_CONFIG.clutch[channel]) +
                        " 所属 PCF8575 已离线，无法确认离合释放，请检查扩展模块接线和供电";
        if (!error.length()) error = "该通道暂不可用，请重新读取设置和设备状态后重试";
        server.send(409, "application/json", Json::message(false, error.c_str()));
        return;
    }
    if (!BMCU_manual_pulse(motor, dir, duration)) {
        server.send(409, "application/json", "{\"ok\":false,\"msg\":\"通道正在运行、停止或已排队\"}");
        return;
    }
    LOG_MAIN("[web] 通道 %d 排队%s，%dms", motor + 1, dir > 0 ? "进料" : "退料", duration);
    server.send(202, "application/json", "{\"ok\":true,\"msg\":\"已排队\"}");
}

/**
 * 检查 HA 通道接口开关，关闭时已添加的集成也不能操作设备
 * @return 可继续处理请求时返回 true，否则已回复错误
 */
static bool require_ha_controls() {
    if (ha_controls_enabled()) return true;
    server.send(403, "application/json", "{\"ok\":false,\"msg\":\"未启用 HA 通道控制\"}");
    return false;
}

/**
 * 向 HA 返回有效通道的退料时间与电机就绪状态，不暴露访问码或令牌
 */
static void handle_ha_channels() {
    if (!require_ha_controls()) return;
    String body = R"({"id":")" + ha_device_id() + R"(","channels":[)";
    for (int ch = 0; ch < active_channel_count(); ++ch) {
        if (ch) body += ',';
        body += "{\"timeout\":" + String(PULLBACK_TIMEOUT[ch]) + ",\"ready\":" +
                (BMCU_channel_motor_ready(ch) ? "true}" : "false}");
    }
    server.send(200, "application/json", body + "]}");
}

/**
 * 接收单通道进料或退料按钮，沿用网页点动时长、接线保护及主循环队列
 */
static void handle_ha_motor() {
    if (!require_ha_controls()) return;
    if (server.args() != 2 || !server.hasArg("motor") || (server.arg("dir") != "1" && server.arg("dir") != "-1")) {
        server.send(400, "application/json", "{\"ok\":false,\"msg\":\"请指定通道和点动方向\"}");
        return;
    }
    handle_manual_motor();
}

/**
 * 仅允许更新单个启用通道的退料时间，复用网页设置校验、保存回滚与运行时更新
 */
static void handle_ha_timeout() {
    if (!require_ha_controls()) return;
    if (server.args() == 1) {
        for (int ch = 0; ch < active_channel_count(); ++ch) {
            if (server.argName(0) == "to" + String(ch)) {
                handle_settings_save();
                return;
            }
        }
    }
    server.send(400, "application/json", "{\"ok\":false,\"msg\":\"请指定一个启用通道的退料时间\"}");
}

/**
 * 登记暂停打印实验或解除请求
 * 只接受 enable=0/1；回调不发送总线帧、不驱动电机，实际反馈和解除交由主循环
 */
static void handle_pause_test() {
    if (!server.hasArg("enable")) {
        server.send(400, "application/json", "{\"ok\":false,\"msg\":\"缺少测试操作\"}");
        return;
    }
    const String enable = server.arg("enable");
    if (enable != "0" && enable != "1") {
        server.send(400, "application/json", "{\"ok\":false,\"msg\":\"测试操作必须为 0 或 1\"}");
        return;
    }
    if (!BambuBus_request_pause_test(enable == "1")) {
        server.send(409, "application/json", "{\"ok\":false,\"msg\":\"需要有效的正在使用通道，且不能重复触发测试\"}");
        return;
    }
    server.send(202, "application/json", enable == "1"
        ? "{\"ok\":true,\"msg\":\"测试请求已提交，请观察打印机\"}"
        : "{\"ok\":true,\"msg\":\"解除请求已提交\"}");
}

/**
 * 处理 WiFi 配网请求
 * 保留 AP 的同时尝试连接目标路由并刷新连接快闪；成功后常亮、保存凭据并重启，失败则恢复原网络或提示重试
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
    wifi_failure_led_active = false;
    WiFi.begin(ssid.c_str(), pass.c_str());
    hal_wifi_led_run(HalWifiLedState::connecting);

    const uint32_t t0 = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - t0 < STA_TIMEOUT_MS) {
        hal_watchdog_feed();
        hal_wifi_led_run(HalWifiLedState::connecting);
        delay(100);
    }

    if (WiFi.status() == WL_CONNECTED) {
        hal_wifi_led_run(HalWifiLedState::connected);
        save_wifi_creds(ssid.c_str(), pass.c_str());
        String ip = WiFi.localIP().toString();
        LOG_MAIN("[web] WiFi 连接成功，IP = %s，重启切换为 STA 模式", ip.c_str());
        // 页面拿到 ip 后会去轮询这个地址，能连上才跳转，所以这里必须把 ip 回给前端
        server.send(200, "application/json",
                    R"({"ok":true,"ip":")" + ip + "\",\"msg\":\"配网成功，设备即将重启\"}");
        delay(1000);
        ESP.restart();
        return;
    }

    // 失败：STA 模式立即恢复原网络，AP 模式继续按间隔重试已保存的网络
    if (!ap_mode && sta_ssid[0]) {
        WiFi.disconnect();
        start_sta_connect();
    } else {
        schedule_sta_retry();
        if (!sta_ssid[0]) {
            wifi_failure_led_active = true;
            wifi_failure_led_deadline = millis() + 6000;
        }
    }

    wifi_indicator_run();

    LOG_MAIN("[web] WiFi 连接失败: %s", ssid.c_str());
    server.send(200, "application/json",
                "{\"ok\":false,\"msg\":\"连接失败，请确认 SSID / 密码，且路由器为 2.4GHz\"}");
}

/**
 * 处理 mDNS 名称保存请求
 * 校验并写入 WiFi NVS；STA 在线时重载 mDNS 并异步检测重名，实际地址由状态接口更新，AP 模式下联网后生效
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
    const String url = mdns_active_name[0] ? "http://" + String(mdns_active_name) + ".local/" : "";
    const String msg = active
        ? (started ? "mDNS 名称已保存，正在检测重名，请查看下方当前访问地址" : "mDNS 名称已保存，但服务启动失败，请重启设备")
        : "mDNS 名称已保存，联网后生效";
    server.send(200, "application/json",
                R"({"ok":true,"mdns":")" + normalized + R"(","mdns_active":")" + Json::escape(mdns_active_name) + R"(","url":")" + url +
                R"(","msg":")" + msg + "\"}");
}

/**
 * 处理忘记 WiFi 请求
 * 清除凭据并重启，使设备进入配网模式
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
 * 异步启动扫描；扫描未完成时返回 scanning，完成后返回 SSID、信号强度和加密状态列表
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
        j += R"({"ssid":")" + Json::escape(WiFi.SSID(i).c_str()) + "\"";
        j += ",\"rssi\":" + String(WiFi.RSSI(i));
        j += ",\"enc\":" + String(WiFi.encryptionType(i) == WIFI_AUTH_OPEN ? 0 : 1) + "}";
    }
    j += "]";
    WiFi.scanDelete();
    server.send(200, "application/json", j);
}

static bool ota_check_running();

/**
 * 处理日志读取请求
 * both=1 时一次读取两路日志，以主板字节块、总线字节块顺序返回，头部携带长度和独立游标
 * 发送缓冲按请求分配，后台检查期间缩小分块以给 TLS 握手留出内存；日志仍按游标连续读取
 * 合并读取支持启动标识、清空位图和抓取边界；旧单通道接口仍供已有客户端使用
 * 单通道请求未指定或传入无效通道时默认读取主板日志
 */
static void handle_log() {
    const size_t capacity = ota_check_running() ? 4096 : LOG_PULL_MAX;
    const std::unique_ptr<char[]> storage(new (std::nothrow) char[capacity + 64]);
    if (!storage) {
        server.send(503, "application/json", "{\"ok\":false,\"msg\":\"内存不足，请稍后重试读取日志\"}");
        return;
    }
    char *buf = storage.get();
    static const String boot_id = String(esp_random(), HEX) + String(esp_random(), HEX);
    if (server.arg("both") == "1") {
        const size_t chunk = capacity / 2;
        const bool reset = server.hasArg("boot") && server.arg("boot") != boot_id;
        const unsigned clear_mask = server.hasArg("clear_mask") ? server.arg("clear_mask").toInt() & 3 : 0;
        uint32_t seq[2] = {
            reset ? 0U : strtoul(server.arg("main_seq").c_str(), nullptr, 10),
            reset ? 0U : strtoul(server.arg("bus_seq").c_str(), nullptr, 10)
        };
        for (int channel = 0; channel < LOG_CH_COUNT; channel++) {
            if (clear_mask & (1U << channel)) {
                log_bus_clear(channel);
                seq[channel] = log_bus_total(channel);
            }
        }
        const uint32_t boundary = log_bus_total(LOG_CH_BUS);
        const uint32_t uptime = millis();
        uint32_t dropped[2] = {};
        const size_t main_len = log_bus_read(LOG_CH_MAIN, &seq[0], buf, chunk + 1, &dropped[0]);
        const size_t bus_len = log_bus_read(LOG_CH_BUS, &seq[1], buf + chunk + 1, chunk + 1, &dropped[1]);
        server.sendHeader("X-Log-Boot", boot_id);
        server.sendHeader("X-Uptime-Ms", String(uptime));
        server.sendHeader("X-Log-Main-Length", String(main_len));
        server.sendHeader("X-Log-Bus-Length", String(bus_len));
        server.sendHeader("X-Log-Main-Seq", String(seq[0]));
        server.sendHeader("X-Log-Bus-Seq", String(seq[1]));
        server.sendHeader("X-Log-Main-Total", String(log_bus_total(LOG_CH_MAIN)));
        server.sendHeader("X-Log-Bus-Total", String(log_bus_total(LOG_CH_BUS)));
        server.sendHeader("X-Log-Main-Dropped", String(dropped[0]));
        server.sendHeader("X-Log-Bus-Dropped", String(dropped[1]));
        server.sendHeader("X-Log-Boundary", String(boundary));
        server.sendHeader("Cache-Control", "no-store");
        server.setContentLength(main_len + bus_len);
        server.send(200, "application/octet-stream", "");
        if (main_len) server.sendContent(buf, main_len);
        if (bus_len) server.sendContent(buf + chunk + 1, bus_len);
        return;
    }
    int ch = server.hasArg("ch") ? server.arg("ch").toInt() : static_cast<long>(LOG_CH_MAIN);
    if (ch < 0 || ch >= LOG_CH_COUNT) ch = LOG_CH_MAIN;

    if (server.hasArg("clear")) {
        log_bus_clear(ch);
        server.sendHeader("X-Log-Seq", String(log_bus_total(ch)));
        server.sendHeader("X-Uptime-Ms", String(millis()));
        server.sendHeader("Cache-Control", "no-store");
        server.send(200, "text/plain; charset=utf-8", "");
        return;
    }

    uint32_t seq = server.hasArg("seq") ? strtoul(server.arg("seq").c_str(), nullptr, 10) : 0;

    const size_t length = log_bus_read(ch, &seq, buf, capacity + 64);

    server.sendHeader("X-Log-Seq", String(seq));
    server.sendHeader("X-Uptime-Ms", String(millis()));
    server.sendHeader("Cache-Control", "no-store");
    if (server.hasArg("dl")) {
        server.sendHeader("Content-Disposition",
                          ch == LOG_CH_BUS ? "attachment; filename=bambubus.log"
                                           : "attachment; filename=main.log");
    }
    server.setContentLength(length);
    server.send(200, "text/plain; charset=utf-8", "");
    if (length) server.sendContent(buf, length);
}

/**
 * 处理设备重启请求
 * 在维护状态下回复成功并重启设备
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
 * 恢复默认参数、清除 HA 与 WiFi 凭据并重启；HA 清除失败时保留运行配置并报告错误
 */
static void handle_factory_reset() {
    if (!maintenance_required()) return;
    constexpr HaConfig defaults;
    if (!ha_save_config(defaults)) {
        server.send(500, "application/json", "{\"ok\":false,\"msg\":\"HA 配置清除失败，请重试\"}");
        return;
    }
    portENTER_CRITICAL(&ha_lock);
    ha_config = defaults;
    if (++ha_generation == 0) ++ha_generation;
    if (++ha_connection_generation == 0) ++ha_connection_generation;
    ha_status = HaStatus();
    portEXIT_CRITICAL(&ha_lock);
    settings_reset_to_defaults();
    settings_save_to_flash();
    forget_wifi_creds();
    LOG_MAIN("[web] 已恢复出厂设置（含 WiFi 与 HA 凭据），即将重启");
    server.send(200, "application/json", "{\"ok\":true,\"msg\":\"已恢复默认，设备重启中\"}");
    delay(300);
    ESP.restart();
}

/**
 * 恢复默认配置并保留 WiFi 设置
 * 维护状态下生成默认快照，写入并回读校验成功后重启；保留独立存放的 WiFi、mDNS 与 HA 网络配置
 */
static void handle_config_reset() {
    if (!maintenance_required()) return;
    const SettingsData previous = settings_snapshot();
    settings_reset_to_defaults();
    const SettingsData defaults = settings_snapshot();
    settings_restore_snapshot(previous);
    if (!settings_save_snapshot(defaults)) {
        server.send(500, "application/json",
                    "{\"ok\":false,\"msg\":\"保存或回读校验失败，当前运行配置未改变，请重试\"}");
        return;
    }
    LOG_MAIN("[web] 已恢复默认配置（保留 WiFi），即将重启");
    server.send(200, "application/json",
                "{\"ok\":true,\"msg\":\"已恢复默认配置，WiFi 已保留，设备重启中\"}");
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
// 自动模式按顺序尝试；代理只转发 GitHub Release 下载地址
static constexpr char *const OTA_SOURCE_NAMES[] = {"GitHub Pages", "GitHub 直连", "ghfast.top", "gh-proxy.com", "gh.llkk.cc"};
static constexpr char *const OTA_PROXY_PREFIXES[] = {"", "", "https://ghfast.top/", "https://gh-proxy.com/", "https://gh.llkk.cc/"};
static constexpr int OTA_SOURCE_COUNT = 5;
static int g_ota_source = -1;
// 仅纯文件名允许在官方渠道间回退；版本、大小和 MD5 始终保持不变
static String g_ota_file;
// 仅 progress=1 请求使用逐行 JSON 回传；普通 API 请求仍返回单个 JSON
static bool g_ota_streaming = false;
static String g_ota_progress_source;

namespace {
    // 版本检查独占 OTA 清单缓存；运行期间拒绝安装请求，进度与结果通过互斥锁交给主循环
    struct OtaCheckState {
        uint32_t id = 0;
        bool running = false;
        String base, progress, result;
    };
}
static OtaCheckState ota_check;
static SemaphoreHandle_t ota_check_mutex = nullptr;

/**
 * 查询后台版本检查是否仍占用 OTA 清单缓存
 * @return 正在检查或状态暂时被锁定时返回 true
 */
static bool ota_check_running() {
    if (!ota_check_mutex) return false;
    if (xSemaphoreTake(ota_check_mutex, 0) != pdTRUE) return true;
    const bool running = ota_check.running;
    xSemaphoreGive(ota_check_mutex);
    return running;
}

/**
 * 按请求参数开启 OTA 进度响应
 * 通过当前连接分块回传，避免同步更新期间另开轮询请求
 */
static void ota_progress_begin() {
    g_ota_streaming = server.arg("progress") == "1";
    g_ota_progress_source = "";
    if (!g_ota_streaming) return;
    server.sendHeader("Cache-Control", "no-store");
    server.sendHeader("X-Accel-Buffering", "no");
    server.setContentLength(CONTENT_LENGTH_UNKNOWN);
    server.send(200, "application/x-ndjson; charset=utf-8", "");
    server.client().setNoDelay(true);
}

/**
 * 回传当前 OTA 阶段与实际传输字节数
 * @param stage 阶段标识
 * @param msg 当前操作说明
 * @param loaded 已处理字节数
 * @param total 总字节数；0 表示当前阶段没有可计算的百分比
 */
static void ota_progress(const char *stage, const String &msg, const size_t loaded = 0, const size_t total = 0) {
    if (!g_ota_streaming || !server.client().connected()) return;
    String j = R"({"event":"progress","stage":")" + String(stage) + R"(","msg":")" + Json::escape(msg.c_str());
    j += R"(","source":")" + Json::escape(g_ota_progress_source.c_str()) + "\"";
    j += ",\"loaded\":" + String(static_cast<unsigned long>(loaded));
    j += ",\"total\":" + String(static_cast<unsigned long>(total)) + "}\n";
    server.sendContent(j);
    hal_watchdog_feed();
}

/**
 * 发布清单检查阶段，后台检查只更新快照，安装流程沿用当前 HTTP 进度响应
 * @param background 是否由版本检查任务调用
 * @param stage 阶段标识
 * @param msg 当前阶段说明
 */
static void ota_manifest_progress(const bool background, const char *stage, const String &msg) {
    if (!background) {
        ota_progress(stage, msg);
        return;
    }
    String progress = R"({"stage":")" + String(stage) + R"(","msg":")" + Json::escape(msg.c_str());
    progress += R"(","source":")" + Json::escape(g_ota_progress_source.c_str()) + "\"}";
    xSemaphoreTake(ota_check_mutex, portMAX_DELAY);
    ota_check.progress = std::move(progress);
    xSemaphoreGive(ota_check_mutex);
}

/**
 * 结束 OTA 请求并返回最终结果
 * @param status 普通 JSON 响应的 HTTP 状态码
 * @param data 最终结果 JSON
 * 流式响应以 result 事件及空块结束，已发送响应头后的失败由 data.ok 表达
 */
static void ota_send_result(const int status, const String &data) {
    if (g_ota_streaming) {
        server.sendContent(R"({"event":"result","data":)" + data + "}\n");
        server.sendContent("");
    } else {
        server.send(status, "application/json", data);
    }
    g_ota_streaming = false;
    g_ota_progress_source = "";
}

/**
 * 获取 OTA 服务基础地址
 * @return 去掉首尾空白和末尾斜杠的地址
 * 用于拼接 manifest.json 和相对固件路径
 */
static String ota_base() {
    String b = OTA_URL;
    b.trim();
    while (b.endsWith("/")) b.remove(b.length() - 1);
    return b;
}

/**
 * 生成内置渠道的清单或固件目录
 * @param source 内置渠道编号
 * @param version 固件版本；空串表示查询最新清单
 * @return 对应渠道的下载目录
 * Release 固件固定到版本标签，避免 latest 在下载期间变化
 */
static String ota_source_base(const int source, const String &version = "") {
    if (source == 0) return DEFAULT_OTA_URL;
    const String base = String(OTA_PROXY_PREFIXES[source]) + "https://github.com/tsimplez/bmcu-esp/releases";
    return base + (version.length() ? "/download/v" + version : String("/latest/download"));
}

/**
 * 识别已保存地址对应的内置渠道
 * @param base 去掉末尾斜杠的更新源目录
 * @return 内置编号，-1 表示自定义源
 * 默认 Pages 地址代表自动模式，其余地址固定使用用户选择的渠道
 */
static int ota_source_index(const String &base) {
    for (int i = 0; i < OTA_SOURCE_COUNT; ++i)
        if (base == ota_source_base(i)) return i;
    return -1;
}

// a 比 b 新返回 true（按 . 分段比数字，1.0.10 > 1.0.9）
/**
 * 比较两个版本号的新旧
 * @param a 待比较版本
 * @param b 基准版本
 * @return true 表示 a 比 b 新
 * 按点分段比较数字，可正确处理 1.0.10 > 1.0.9
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
 * 要求 YY.MM.DD 或 YY.MM.DD.修订号格式
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
 * 记录 OTA 可用于 TLS 的内部堆状态
 * @param stage 当前检查阶段
 * 输出内部 RAM 空闲总量和最大连续空闲块，便于区分总量不足与堆碎片
 */
static void ota_log_tls_heap(const char *stage) {
    const size_t free_bytes = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    const size_t largest_block = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    LOG_MAIN("[ota] TLS 内存（%s）: free=%lu, largest=%lu 字节", stage,
             static_cast<unsigned long>(free_bytes), static_cast<unsigned long>(largest_block));
}

/**
 * 在创建 HTTPS 客户端前检查内部堆余量
 * @param url 请求地址
 * @param err 检查失败原因
 * @param resource 当前请求资源名称
 * HTTPS 握手前要求至少 48 KiB 内部堆空闲且最大连续块不少于 24 KiB；HTTP 请求不受此门槛限制
 * @return 内存满足要求或请求使用 HTTP 时返回 true
 */
static bool ota_tls_memory_preflight(const String &url, String &err, const char *resource) {
    if (!url.startsWith("https://")) return true;

    constexpr size_t MIN_FREE_BYTES = 48u * 1024u;
    constexpr size_t MIN_LARGEST_BLOCK = 24u * 1024u;
    const size_t free_bytes = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    const size_t largest_block = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    LOG_MAIN("[ota] TLS 内存预检（%s）: free=%lu, largest=%lu 字节", resource,
             static_cast<unsigned long>(free_bytes), static_cast<unsigned long>(largest_block));
    if (free_bytes >= MIN_FREE_BYTES && largest_block >= MIN_LARGEST_BLOCK) return true;

    err = String("内存预检未通过（空闲 ") + String(static_cast<unsigned long>(free_bytes)) +
          " 字节，最大连续块 " + String(static_cast<unsigned long>(largest_block)) + " 字节），暂不启动 HTTPS";
    return false;
}

/**
 * 创建 OTA HTTP 客户端
 * @param url 固件或清单地址
 * @return 新建的 WiFiClient 指针
 * HTTPS 使用 WiFiClientSecure 并限制握手等待，HTTP 使用普通客户端
 */
static WiFiClient *ota_make_client(const String &url) {
    if (url.startsWith("https")) {
        auto *c = new WiFiClientSecure();
        c->setInsecure();  // 不校验证书；省掉塞证书要用的几十 KB 内存
        c->setHandshakeTimeout(3);
        return c;
    }
    return new WiFiClient();
}

namespace {
    // client 是 new 出来的，但 HTTPClient 析构时还会再碰一次 _client->stop()，
    // 而 end() 只在 socket 还连着的时候才把 _client 置空（连接已断开时它不置空）
    // 所以 client 必须活得比 HTTPClient 久：用这个壳子持有，声明在 HTTPClient 前面，
    // 块内对象按声明逆序析构，正好保证「先 ~HTTPClient，再 delete client」
    class OtaClientHolder {
    public:
        /**
         * 构造 OTA 客户端持有器
         * @param url 请求地址
         * 按地址创建 HTTP 或 HTTPS 客户端，并保证其生命周期长于 HTTPClient
         */
        explicit OtaClientHolder(const String &url) : _client(ota_make_client(url)) {}
        /**
         * 析构 OTA 客户端持有器
         * 在 HTTPClient 析构之后释放底层网络客户端
         */
        ~OtaClientHolder() { delete _client; }
        /**
         * 获取底层网络客户端引用
         * @return 底层 WiFiClient 引用
         * 供 HTTPClient.begin() 使用
         */
        [[nodiscard]] WiFiClient &ref() const { return *_client; }

    private:
        WiFiClient *_client;
    };
}

/**
 * 打开 OTA 下载请求并分步处理重定向
 * @param http HTTP 客户端
 * @param client 底层网络客户端
 * @param url 清单或固件地址
 * @param err 失败原因
 * @param resource 当前下载的资源名称
 * @param background 是否仅在后台读取清单；此时不访问运动状态或主循环看门狗
 * @return true 表示服务器返回 HTTP 200
 * 回传连接与跳转阶段，安装时每次跳转前检查设备状态并喂狗，连接及读取各等待至多 2 秒；失败保留 TLS 诊断
 */
static bool ota_http_open(HTTPClient &http, WiFiClient &client, const String &url, String &err, const char *resource,
                          bool background = false) {
    http.setConnectTimeout(2000);
    http.setTimeout(2000);
    http.setFollowRedirects(HTTPC_DISABLE_FOLLOW_REDIRECTS);
    if (!http.begin(client, url)) { err = "地址无法解析"; return false; }
    for (int redirects = 0; redirects <= 4; ++redirects) {
        if (!background && !BMCU_can_maintain()) { err = "请先停止所有电机运动"; return false; }
        ota_manifest_progress(background, redirects ? "redirect" : "connect",
                     String(redirects ? "正在连接跳转后的服务器，获取" : "正在连接更新服务器，获取") + resource);
        if (!background) hal_watchdog_feed();
        const int code = http.GET();
        if (!background) hal_watchdog_feed();
        if (code == HTTP_CODE_OK) return true;
        if (code == 301 || code == 302 || code == 303 || code == 307 || code == 308) {
            const String location = http.getLocation();
            if (redirects < 4 && location.length() && http.setURL(location)) continue;
            err = "重定向过多或跳转地址不受支持";
        } else if (code < 0) {
            err = "连接失败（HTTP " + String(code) + ": " + HTTPClient::errorToString(code) + "）";
            if (url.startsWith("https://")) {
                ota_log_tls_heap("HTTPS 请求失败后");
                char tls_error[128] = {};
                if (const int tls_code = static_cast<WiFiClientSecure &>(client).lastError(tls_error, sizeof(tls_error))) err += "，TLS " + String(tls_code) + ": " + tls_error;
            }
        } else err = "服务器返回 HTTP " + String(code);
        return false;
    }
    return false;
}

/**
 * 执行 OTA HTTP GET 请求
 * @param url 请求地址
 * @param body 响应正文
 * @param err 错误信息
 * @param background 是否由后台版本检查任务调用
 * @return true 表示请求成功
 * 回传清单下载阶段；清单必须有长度且不超过 8 KB，避免代理错误页占满内存
 */
static bool ota_http_get(const String &url, String &body, String &err,const bool background = false) {
    if (!ota_tls_memory_preflight(url, err, "版本清单")) return false;
    const OtaClientHolder client(url);
    HTTPClient http;
    if (!ota_http_open(http, client.ref(), url, err, "版本清单", background)) {
        http.end();
        return false;
    }
    if (!background && !BMCU_can_maintain()) {
        err = "下载前检测到打印机通信或电机动作";
        http.end();
        return false;
    }
    const int length = http.getSize();
    if (length <= 0 || length > 8192) {
        err = "清单长度无效或超过 8 KB";
        http.end();
        return false;
    }
    ota_manifest_progress(background, "manifest", "正在下载版本清单");
    body = http.getString();
    if (!background) hal_watchdog_feed();
    http.end();
    if (body.length() != static_cast<unsigned int>(length)) { err = "清单下载不完整"; return false; }
    return true;
}

/**
 * 获取并解析 OTA 清单
 * @param base 当前尝试的更新源目录
 * @param source 内置渠道编号；-1 为自定义
 * @param err 错误信息
 * @param background 是否由后台版本检查任务调用
 * @return true 表示成功取得当前板型固件信息
 * 读取 manifest.json，回传解析阶段，提取目标平台的 url、md5、version、notes 和 size，并校验版本与 MD5 格式
 */
static bool ota_fetch_manifest_from(const String &base, const int source, String &err, const bool background = false) {
    g_ota_url = g_ota_md5 = g_ota_ver = g_ota_notes = g_ota_file = "";
    g_ota_size = 0;
    g_ota_source = -1;
    String body;
    if (!ota_http_get(base + "/manifest.json", body, err, background)) return false;
    ota_manifest_progress(background, "manifest", "正在解析版本清单并核对板型");

    const auto manifest = Json::parse(body.c_str(), body.length());
    const cJSON *target = Json::item(manifest.get(), FW_TARGET);
    if (!cJSON_IsObject(target)) {
        err = "清单里没有 " + String(FW_TARGET) + " 对应的固件";
        return false;
    }

    Json::read_string(target, "url", g_ota_url);
    Json::read_string(target, "md5", g_ota_md5);
    Json::read_string(target, "version", g_ota_ver);
    const cJSON *notes = Json::item(target, "notes");
    if (notes && !Json::read_string(target, "notes", g_ota_notes)) {
        err = "当前板型清单中的 notes 无效";
        return false;
    }
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

    uint32_t size = 0;
    if (Json::item(target, "size") && !Json::read_uint(target, "size", size)) {
        err = "当前板型清单中的 size 无效";
        return false;
    }
    g_ota_size = size;

    g_ota_url.trim();
    if (source >= 0 && g_ota_url.length() && g_ota_url.indexOf('/') < 0 &&
        g_ota_url.indexOf('\\') < 0 && g_ota_url.indexOf(':') < 0 &&
        g_ota_url.indexOf('?') < 0 && g_ota_url.indexOf('#') < 0 && g_ota_url.indexOf("..") < 0) {
        g_ota_file = g_ota_url;
        g_ota_url = ota_source_base(source, g_ota_ver) + "/" + g_ota_file;
    }
    if (g_ota_url.length() && !g_ota_url.startsWith("http")) {
        g_ota_url = base + "/" + g_ota_url;  // 清单里允许只写文件名
    }
    if (!g_ota_url.length()) {
        err = "清单里没有写 url";
        return false;
    }
    g_ota_source = source;
    return true;
}

/**
 * 按用户设置获取第一个有效的 OTA 清单
 * @param err 全部失败时的最后渠道及错误原因
 * @param background 是否在后台使用启动时保存的更新源快照
 * @return true 表示已取得有效清单
 * 回传当前渠道和失败原因；自动模式顺序回退，固定或自定义源只尝试一次，失败不保留旧下载信息
 */
static bool ota_fetch_manifest(String &err, const bool background = false) {
    const String base = background ? ota_check.base : ota_base();
    const int selected = ota_source_index(base);
    const int count = selected == 0 ? OTA_SOURCE_COUNT : 1;
    for (int i = 0; i < count; ++i) {
        const int source = selected == 0 ? i : selected;
        const String name = source >= 0 ? OTA_SOURCE_NAMES[source] : "自定义";
        const String candidate = selected == 0 ? ota_source_base(source) : base;
        g_ota_progress_source = name;
        err = "";
        if (!candidate.length()) err = "还没有配置更新源地址";
        else if (ota_fetch_manifest_from(candidate, source, err, background)) {
            LOG_MAIN("[ota] 已选渠道: %s", name.c_str());
            return true;
        }
        LOG_MAIN("[ota] 渠道 %s 检查失败: %s", name.c_str(), err.c_str());
        err = name + "：" + err;
        ota_manifest_progress(background, "retry", "渠道检查失败：" + err + (i + 1 < count ? "，准备切换渠道" : ""));
        if (!background && !BMCU_can_maintain()) break;
    }
    g_ota_url = g_ota_md5 = g_ota_ver = g_ota_notes = g_ota_file = "";
    g_ota_size = 0;
    g_ota_source = -1;
    if (selected == 0) err = "自动渠道检查失败，最后尝试 " + err;
    return false;
}

/**
 * 生成 OTA 错误 JSON
 * @param msg 错误文本
 * @return {"ok":false,"msg":"..."} 格式字符串
 * 统一网页接口的错误响应格式
 */
static String ota_json_err(const String &msg) {
    return Json::message(false, msg.c_str());
}

/**
 * 检查安装固件前设备是否空闲
 * @return true 表示允许继续
 * 拒绝与后台版本检查并发安装，再复用 maintenance_required() 的检查和 409 响应
 */
static bool ota_require_stopped() {
    if (ota_check_running()) {
        server.send(409, "application/json", ota_json_err("正在检查更新，请等待检查结束后再安装"));
        return false;
    }
    return maintenance_required();
}

// ---- 检查更新 ----
/**
 * 在后台读取版本清单并发布最终结果
 * @param arg 未使用的任务参数
 * 独占清单缓存，不访问 WebServer 或运动状态，网络对象析构后释放任务
 */
static void ota_check_worker(void *arg) {
    (void)arg;
    {
        String err, result;
        if (!ota_fetch_manifest(err, true)) {
            LOG_MAIN("[ota] 检查更新失败: %s", err.c_str());
            result = ota_json_err(err);
        } else {
            const bool has = version_newer(g_ota_ver, String(FW_VERSION));
            result.reserve(320);
            result = "{\"ok\":true";
            result += R"(,"current":")" + String(FW_VERSION) + "\"";
            result += R"(,"target":")" + String(FW_TARGET) + "\"";
            result += R"(,"source":")" + String(g_ota_source >= 0 ? OTA_SOURCE_NAMES[g_ota_source] : "自定义") + "\"";
            result += R"(,"latest":")" + Json::escape(g_ota_ver.c_str()) + "\"";
            result += ",\"has_update\":" + String(has ? "true" : "false");
            result += R"(,"notes":")" + Json::escape(g_ota_notes.c_str()) + "\"";
            result += ",\"size\":" + String(static_cast<unsigned long>(g_ota_size));
            result += R"(,"md5":")" + Json::escape(g_ota_md5.c_str()) + "\"";
            result += R"(,"url":")" + Json::escape(g_ota_url.c_str()) + "\"}";
            LOG_MAIN("[ota] 检查更新: 本机 %s, 服务器 %s, %s", FW_VERSION, g_ota_ver.c_str(),
                     has ? "有新版" : "已是最新");
        }
        xSemaphoreTake(ota_check_mutex, portMAX_DELAY);
        ota_check.result = std::move(result);
        ota_check.running = false;
        xSemaphoreGive(ota_check_mutex);
    }
    vTaskDelete(nullptr);
}

/**
 * 返回后台版本检查的进度或结果快照
 * GET 必须携带当前任务 id，过期或无效 id 返回 409，锁外发送响应以免阻塞后台任务
 */
static void ota_check_send_status() {
    if (!ota_check_mutex) {
        server.send(409, "application/json", ota_json_err("没有可查询的版本检查，请重新检查"));
        return;
    }
    xSemaphoreTake(ota_check_mutex, portMAX_DELAY);
    const bool valid = ota_check.id &&
                       (server.method() != HTTP_GET || server.arg("id") == String(ota_check.id));
    String response;
    if (valid) {
        response = R"({"ok":true,"id":)" + String(ota_check.id);
        response += ",\"running\":" + String(ota_check.running ? "true" : "false");
        response += ",\"progress\":" + ota_check.progress;
        response += ",\"result\":" + (ota_check.result.length() ? ota_check.result : String("null")) + "}";
    }
    xSemaphoreGive(ota_check_mutex);
    server.send(valid ? 200 : 409, "application/json",
                valid ? response : ota_json_err("版本检查已失效，请重新检查"));
}

/**
 * 启动或查询后台版本检查，无需进入维护状态
 * POST 创建任务或加入正在执行的检查，GET 按 id 轮询；关闭自动提示时仅 force=1 启动检查
 */
static void handle_ota_check() {
    server.sendHeader("Cache-Control", "no-store");
    if (server.method() == HTTP_GET) {
        ota_check_send_status();
        return;
    }
    if (OTA_NOTIFY == 0 && server.arg("force") != "1") {
        server.send(200, "application/json", "{\"ok\":true,\"skipped\":true}");
        return;
    }
    if (!ota_check_mutex) ota_check_mutex = xSemaphoreCreateMutex();
    if (!ota_check_mutex) {
        server.send(503, "application/json", ota_json_err("内存不足，无法启动版本检查"));
        return;
    }
    xSemaphoreTake(ota_check_mutex, portMAX_DELAY);
    const bool start = !ota_check.running;
    if (start) {
        if (++ota_check.id == 0) ++ota_check.id;
        ota_check.base = ota_base();
        ota_check.progress = "{\"stage\":\"check\",\"msg\":\"正在检查可用更新\"}";
        ota_check.result = "";
        ota_check.running = true;
    }
    xSemaphoreGive(ota_check_mutex);
    if (start && xTaskCreate(ota_check_worker, "ota_check", 8192, nullptr, 1, nullptr) != pdPASS) {
        xSemaphoreTake(ota_check_mutex, portMAX_DELAY);
        ota_check.result = ota_json_err("内存不足，无法启动版本检查");
        ota_check.running = false;
        xSemaphoreGive(ota_check_mutex);
        server.send(503, "application/json", ota_json_err("内存不足，无法启动版本检查"));
        return;
    }
    ota_check_send_status();
}

// ---- 在线更新：下载 → 写 flash → 重启 ----
/**
 * 从 URL 下载并写入固件
 * @param url 本次尝试的固件地址
 * @param err 错误信息
 * @param retryable 是否允许换渠道重试
 * @return true 表示下载、校验和写入全部成功
 * 按实际写入字节数回传进度，并显示准备、等待数据和校验阶段；传输失败会中止本次写入，设备忙或 Flash 写入失败不换渠道重试
 */
static bool ota_flash_attempt(const String &url, String &err, bool &retryable) {
    retryable = false;
    if (!ota_tls_memory_preflight(url, err, "固件下载")) return false;
    const OtaClientHolder client(url);
    HTTPClient http;
    if (!ota_http_open(http, client.ref(), url, err, "固件文件")) {
        retryable = BMCU_can_maintain();
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
        retryable = true;
        err = "服务器必须提供 Content-Length，不支持分块传输";
        http.end();
        return false;
    }
    if (g_ota_size && g_ota_size != static_cast<size_t>(len)) {
        retryable = true;
        err = "固件大小与清单不一致";
        http.end();
        return false;
    }
    ota_progress("prepare", "正在准备固件写入区域");
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
    uint32_t last_progress = last_rx;
    bool timeout = false;
    ota_progress("download", "正在下载并写入固件", 0, static_cast<size_t>(len));

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
            if (!http.connected() || millis() - last_rx > 15000) { timeout = true; break; }
            if (millis() - last_progress >= 1000) {
                ota_progress("download", "正在等待服务器发送固件数据", written, static_cast<size_t>(len));
                last_progress = millis();
            }
            hal_watchdog_feed();
            delay(1);  // 等待数据时让出 CPU，避免空闲任务看门狗超时
            continue;
        }
        last_rx = millis();
        if (Update.write(buf, static_cast<size_t>(n)) != static_cast<size_t>(n)) {
            err = String("写入 Flash 失败: ") + Update.errorString();
            break;
        }
        written += static_cast<size_t>(n);
        if (millis() - last_progress >= 250 || written == static_cast<size_t>(len)) {
            ota_progress("download", "正在下载并写入固件", written, static_cast<size_t>(len));
            last_progress = millis();
        }
        hal_watchdog_feed();
    }

    http.end();

    if (err.length()) { Update.abort(); return false; }
    if (timeout) { Update.abort(); err = "下载超时或连接中断"; retryable = true; return false; }
    if (written != static_cast<size_t>(len)) {
        Update.abort();
        err = "下载不完整（" + String(static_cast<unsigned long>(written)) + "/" + String(len) + " 字节）";
        retryable = true;
        return false;
    }
    if (!BMCU_can_maintain()) {
        Update.abort();
        err = "写入结束前检测到打印机通信或电机动作";
        return false;
    }
    ota_progress("verify", "下载完成，正在校验固件完整性并完成写入");
    if (!Update.end()) {
        retryable = Update.getError() == UPDATE_ERROR_MD5;
        err = String("写入结束失败: ") + Update.errorString();
        Update.abort();
        return false;
    }
    return true;
}

/**
 * 下载固件并在自动模式下回退渠道
 * @param err 最后一次失败原因
 * @return true 表示固件完整写入且校验通过
 * 回传每次使用的渠道和失败原因；优先使用取得清单的渠道，重试固定同一版本文件、大小和 MD5，自定义源不改写
 */
static bool ota_flash_from_url(String &err) {
    const bool automatic = ota_source_index(ota_base()) == 0 && g_ota_file.length();
    const int first = g_ota_source;
    for (int attempt = -1; attempt < (automatic ? OTA_SOURCE_COUNT : 0); ++attempt) {
        if (attempt >= 0 && attempt == first) continue;
        const int source = attempt < 0 ? first : attempt;
        const String url = attempt < 0 ? g_ota_url : ota_source_base(source, g_ota_ver) + "/" + g_ota_file;
        const String name = source >= 0 ? OTA_SOURCE_NAMES[source] : "自定义";
        if (!BMCU_can_maintain()) { err = "请先停止所有电机运动"; return false; }
        bool retryable = false;
        err = "";
        LOG_MAIN("[ota] 下载渠道: %s", name.c_str());
        g_ota_progress_source = name;
        if (ota_flash_attempt(url, err, retryable)) {
            g_ota_url = url;
            g_ota_source = source;
            return true;
        }
        LOG_MAIN("[ota] 渠道 %s 下载失败: %s", name.c_str(), err.c_str());
        err = name + "：" + err;
        ota_progress("retry", "下载失败：" + err);
        if (!retryable) break;
    }
    return false;
}

/**
 * 处理在线升级请求
 * 检查清单和版本，仅当服务器版本更新时下载写入；progress=1 时回传阶段、字节进度和最终结果，成功后重启
 */
static void handle_ota_pull() {
    if (!ota_require_stopped()) return;
    ota_progress_begin();
    ota_progress("check", "正在重新检查版本与下载信息");
    String err;
    if (!ota_fetch_manifest(err)) {
        ota_send_result(200, ota_json_err(err));
        return;
    }
    if (!BMCU_can_maintain()) {
        ota_send_result(409, ota_json_err("请先停止打印、断开打印机总线并停止所有电机运动"));
        return;
    }
    if (!version_newer(g_ota_ver, String(FW_VERSION))) {
        ota_send_result(200, ota_json_err("服务器上的版本不高于本机（" + g_ota_ver + "），已跳过"));
        return;
    }

    LOG_MAIN("[ota] 开始在线更新到 %s：%s", g_ota_ver.c_str(), g_ota_url.c_str());
    uint32_t t0 = millis();
    bool ok = ota_flash_from_url(err);

    if (!ok) {
        LOG_MAIN("[ota] 更新失败: %s", err.c_str());
        ota_send_result(200, ota_json_err("更新失败：" + err));
        return;
    }

    LOG_MAIN("[ota] 更新成功（%lu ms），重启", static_cast<unsigned long>(millis() - t0));
    ota_progress("reboot", "固件校验通过，准备重启设备");
    ota_send_result(200,
                R"({"ok":true,"version":")" + Json::escape(g_ota_ver.c_str()) + "\",\"msg\":\"已更新到 " + Json::escape(g_ota_ver.c_str()) +
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
 * 拒绝与版本检查并发安装，按开始、写入、结束、中断四个阶段驱动 Update，并喂狗和检查设备状态
 */
static void handle_ota_upload() {
    HTTPUpload &up = server.upload();

    if (up.status == UPLOAD_FILE_START) {
        ota_up_failed = false;
        ota_up_err = "";
        if (ota_check_running()) {
            ota_up_failed = true;
            ota_up_err = "正在检查更新，请等待检查结束后再安装";
            return;
        }
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
 * 根据上传阶段记录的结果返回成功或失败，成功后重启设备
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
 * 配网模式下跳转到强制门户首页，STA 模式下返回 404
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
 * 为 localhost 预览的 API 请求添加跨域许可并处理预检
 * @param request 当前 HTTP 请求
 * @param next 后续路由处理回调
 * @return 预检已处理或后续路由的处理结果
 * 仅允许 HTTP localhost 来源，暴露日志响应头；预检不执行实际接口
 */
static bool handle_local_preview_cors(WebServer &request, Middleware::Callback next) {
    if (!request.uri().startsWith("/api/")) return next();
    const String origin = request.header("Origin");
    if (const String local_origin = "http://localhost"; origin != local_origin) {
        if (!origin.startsWith(local_origin + ":")) return next();
        const String port = origin.substring(local_origin.length() + 1);
        if (port.isEmpty() || port.length() > 5) return next();
        for (size_t i = 0; i < port.length(); ++i) {
            if (port[i] < '0' || port[i] > '9') return next();
        }
        if (port.toInt() > 65535) return next();
    }
    request.sendHeader("Access-Control-Allow-Origin", origin);
    request.sendHeader("Vary", "Origin");
    request.sendHeader("Access-Control-Expose-Headers", "*");
    if (request.method() == HTTP_OPTIONS) {
        request.sendHeader("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
        request.sendHeader("Access-Control-Allow-Headers", "Content-Type");
        request.send(204, "text/plain", "");
        return true;
    }
    return next();
}

/**
 * 注册 Web 路由与 localhost 预览跨域处理
 * 把首页、PWA 静态资源、状态、设置、接线、局域网换料、HA、电机、WiFi、日志、重启、恢复默认配置、恢复出厂和 OTA 绑定到处理函数
 */
static void setup_routes() {
    const char *headers[] = {"Origin"};
    server.collectHeaders(headers, 1);
    server.addMiddleware(handle_local_preview_cors);
    server.on("/", HTTP_GET, handle_root);
    server.on("/index.html", HTTP_GET, handle_root);
    for (const auto & i : WEB_ASSETS) {
        const WebAsset *asset = &i;
        server.on(asset->path, HTTP_GET, [asset]() { handle_web_asset(*asset); });
    }
    server.on("/api/status", HTTP_GET, handle_status);
    server.on("/api/home-assistant", HTTP_GET, handle_ha_get);
    server.on("/api/home-assistant", HTTP_POST, handle_ha_save);
    server.on("/api/home-assistant/selection", HTTP_POST, handle_ha_selection);
    server.on("/api/home-assistant/entities", HTTP_GET, handle_ha_entities);
    server.on("/api/home-assistant/channels", HTTP_GET, handle_ha_channels);
    const String ha_base = "/api/home-assistant/channels/" + ha_device_id();
    server.on((ha_base + "/motor").c_str(), HTTP_POST, handle_ha_motor);
    server.on((ha_base + "/timeout").c_str(), HTTP_POST, handle_ha_timeout);
    server.on("/api/ws-token", HTTP_GET, handle_ws_token);
    server.on("/api/hall", HTTP_GET, handle_hall);
    server.on("/api/hall/detection", HTTP_POST, handle_hall_detection_save);
    server.on("/api/settings", HTTP_GET, handle_settings_get);
    server.on("/api/settings", HTTP_POST, handle_settings_save);
    server.on("/api/projects", HTTP_GET, handle_projects_get);
    server.on("/api/projects", HTTP_POST, handle_project_save);
    server.on("/api/bus", HTTP_POST, handle_bus_save);
    server.on("/api/lan/channel", HTTP_POST, handle_lan_channel);
    server.on("/api/lan/cancel", HTTP_POST, handle_lan_cancel);
    server.on("/api/pcf", HTTP_POST, handle_pcf_save);
    server.on("/api/motor", HTTP_POST, handle_manual_motor);
    server.on("/api/printer/pause-test", HTTP_POST, handle_pause_test);
    server.on("/api/scan", HTTP_GET, handle_scan);
    server.on("/api/wifi", HTTP_POST, handle_wifi_save);
    server.on("/api/mdns", HTTP_POST, handle_mdns_save);
    server.on("/api/forget", HTTP_POST, handle_wifi_forget);
    server.on("/api/log", HTTP_GET, handle_log);
    server.on("/api/reboot", HTTP_GET, handle_reboot);
    server.on("/api/settings/reset", HTTP_POST, handle_config_reset);
    server.on("/api/factory", HTTP_POST, handle_factory_reset);
    server.on("/api/ota/check", HTTP_POST, handle_ota_check);
    server.on("/api/ota/check", HTTP_GET, handle_ota_check);
    server.on("/api/ota/pull", HTTP_POST, handle_ota_pull);
    server.on("/api/ota/upload", HTTP_POST, handle_ota_upload_done, handle_ota_upload);
    server.onNotFound(handle_not_found);
}

/**
 * 初始化 Web 管理服务
 * 初始化 WiFi 状态灯，读取 WiFi 和 HA 配置并异步联网；未配置 WiFi 时启动热点并显示慢闪，再启动 Web 与 HA 服务
 */
void web_config_init() {
    hal_wifi_led_init();
    load_wifi_creds();
    ha_load_config();
    WiFi.persistent(false);
    WiFi.setAutoReconnect(false);

    if (sta_ssid[0]) {
        start_sta_connect();
    } else {
        start_config_ap();
    }
    wifi_indicator_run();

    setup_routes();
    server.begin();
    websocket_init();
    ha_start_worker();
    LOG_MAIN("[web] HTTP 管理服务已启动");
}

/**
 * 轮询 Web 与 WiFi 状态
 * 处理 STA 重连并按配网、连接、重试和在线状态刷新指示灯，再轮询 mDNS、HTTP、强制门户 DNS 与 WebSocket
 */
void web_config_run() {
    if (sta_ssid[0]) {
        const uint32_t now = millis();
        if (WiFi.status() == WL_CONNECTED) {
            if (wifi_state != ST_IDLE || ap_mode) on_sta_connected();
        } else if (wifi_state == ST_CONNECTING) {
            if (static_cast<int32_t>(now - connect_deadline) >= 0) {
                LOG_MAIN("[web] 连接 WiFi 超时，开启或保留配网热点");
                if (!ap_mode) start_config_ap();
                schedule_sta_retry();
            }
        } else if (wifi_state == ST_RETRY_WAIT) {
            if (static_cast<int32_t>(now - retry_deadline) >= 0) start_sta_connect();
        } else if (!ap_mode) {
            LOG_MAIN("[web] WiFi 已断开，尝试重新连接");
            start_sta_connect();
        }
    }

    wifi_indicator_run();
    sync_mdns_name();
    server.handleClient();
    websocket_run();
    if (ap_mode) dns.processNextRequest();
}

#endif  // ARDUINO_ARCH_ESP32
