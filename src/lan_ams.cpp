#include "lan_ams.h"
#include "platform.h"
#include "settings_storage.h"

#if defined(ARDUINO_ARCH_ESP32)

#include <WiFi.h>
#include <mqtt_client.h>
#include <cJSON.h>
#include <mbedtls/ssl.h>
#include <esp_idf_version.h>
#include "BMCU.h"
#include "log_bus.h"

namespace {
constexpr uint32_t REPORT_TIMEOUT_MS = 15000;
constexpr uint32_t STEP_TIMEOUT_MS = 120000;
constexpr size_t MAX_REPORT_SIZE = 32768;
enum PrintState { UNKNOWN, RUNNING, PAUSED, ENDED };
enum Step { IDLE, UNLOAD, RETRACT, UNLOADED, HEAT, EXTRUDER, FEED, RESUME };

struct PrinterReport {
    bool connected = false, subscribed = false, failed = false, denied = false;
    uint32_t epoch = 0, received = 0, revision = 0, ams_revision = 0;
    uint32_t bed_time = 0, state_time = 0, nozzle_time = 0;
    int bed = -1, nozzle = -1, ams = -1, filament = -1, print_error = 0;
    PrintState state = UNKNOWN;
    const char *network_error = "";
};
portMUX_TYPE report_lock = portMUX_INITIALIZER_UNLOCKED;
PrinterReport shared_report, report;
esp_mqtt_client_handle_t client = nullptr;
char request_topic[64], report_topic[64], broker_uri[48];
char *incoming = nullptr;
size_t incoming_size = 0, incoming_used = 0;
Step step = IDLE;
int current_channel = -1, target_channel = -1, channel_request = -2;
int previous_bed = -1;
uint32_t seen_epoch = 0, attempted_at = 0, next_connect = 0, next_poll = 0;
uint32_t step_deadline = 0, expected_ams_revision = 0;
bool armed = false, cancel_requested = false, poll_sent = false, reset_client_requested = false;
bool motion_fault = false, pair_logged = false;
int wait_state = -1;
const char *last_error = "";

/**
 * 读取网络任务提交的打印机快照
 * @return 一致的状态副本，不包含电机操作
 */
PrinterReport read_report() {
    portENTER_CRITICAL(&report_lock);
    const auto result = shared_report;
    portEXIT_CRITICAL(&report_lock);
    return result;
}

/**
 * 判断当前报文是否仍然有效
 * @return 已订阅且在有效期内收到打印机状态时返回 true
 */
bool paired() {
    return report.connected && report.subscribed && report.received &&
           millis() - report.received < REPORT_TIMEOUT_MS;
}

/**
 * 读取 JSON 中的有界数值，默认要求整数
 * @param object JSON 对象
 * @param name 字段名称
 * @param value 接收整数的变量
 * @param low 最小值
 * @param high 最大值
 * @param fractional 是否接受小数，接受时向零取整
 * @return 字段存在且满足范围和精度要求时返回 true
 */
bool read_number(const cJSON *object, const char *name, int &value, int low, int high, bool fractional = false) {
    const auto *item = cJSON_GetObjectItemCaseSensitive(object, name);
    if (!cJSON_IsNumber(item) || !(item->valuedouble >= low && item->valuedouble <= high) ||
        (!fractional && item->valuedouble != item->valueint)) return false;
    value = item->valueint;
    return true;
}

/**
 * 合并打印机增量状态并提交到主循环
 * @param data 完整且以 NUL 结尾的 JSON 报文
 * 不把缺失字段清零；喷嘴小数温度向下取整，关键状态仅在变化时记录日志
 * 温度、暂停和退料确认分别记录新鲜度
 */
void receive_report(const char *data) {
    auto *json = cJSON_ParseWithOpts(data, nullptr, true);
    const auto *print = cJSON_GetObjectItemCaseSensitive(json, "print");
    if (!cJSON_IsObject(print)) { cJSON_Delete(json); return; }
    auto next = read_report();
    const auto previous = next;
    const uint32_t now = millis();
    bool valid = false;
    if (read_number(print, "bed_target_temper", next.bed, 0, 130)) {
        next.bed_time = now;
        valid = true;
    }
    if (read_number(print, "nozzle_temper", next.nozzle, 0, 400, true)) {
        next.nozzle_time = now;
        valid = true;
    }
    if (read_number(print, "ams_status", next.ams, 0, 65535)) {
        ++next.ams_revision;
        valid = true;
    }
    valid |= read_number(print, "hw_switch_state", next.filament, 0, 1);
    valid |= read_number(print, "print_error", next.print_error, 0, INT32_MAX);
    const auto *state = cJSON_GetObjectItemCaseSensitive(print, "gcode_state");
    if (cJSON_IsString(state)) {
        next.state = !strcmp(state->valuestring, "PAUSE") ? PAUSED :
                     !strcmp(state->valuestring, "RUNNING") ? RUNNING :
                     (!strcmp(state->valuestring, "FINISH") || !strcmp(state->valuestring, "FAILED") ||
                      !strcmp(state->valuestring, "IDLE")) ? ENDED : UNKNOWN;
        next.state_time = now;
        valid = true;
    }
    const auto *result = cJSON_GetObjectItemCaseSensitive(print, "result");
    if (cJSON_IsString(result) && (!strcmp(result->valuestring, "fail") ||
                                  !strcmp(result->valuestring, "failed"))) next.denied = true;
    if (valid) { next.received = now; ++next.revision; }
    cJSON_Delete(json);
    portENTER_CRITICAL(&report_lock);
    shared_report = next;
    portEXIT_CRITICAL(&report_lock);
    if (valid && (!previous.received || next.state != previous.state || next.ams != previous.ams ||
                  next.filament != previous.filament || next.print_error != previous.print_error)) {
        static const char *states[] = {"未知", "打印中", "已暂停", "已结束/空闲"};
        LOG_MAIN("[lan] 打印机状态：%s，AMS=%d，喷嘴有料=%d，错误=%d",
                 states[next.state], next.ams, next.filament, next.print_error);
    }
    if (next.denied && !previous.denied) LOG_MAIN("[lan] 打印机拒绝了局域网命令，请检查控制权限");
}

/**
 * 释放本条 MQTT 报文的接收缓存
 */
void clear_incoming() {
    free(incoming);
    incoming = nullptr;
    incoming_size = incoming_used = 0;
}

/**
 * 接收 MQTT 事件并提交打印机快照
 * @param arg 未使用的注册参数
 * @param base 事件来源
 * @param id MQTT 事件类型
 * @param data MQTT 事件数据
 * 只接收指定序列号的非保留报文，按偏移拼接分片，不执行运动
 * 连接、订阅和错误日志在快照临界区外写入主板日志
 */
void mqtt_event(void *arg, esp_event_base_t base, int32_t id, void *data) {
    (void)arg; (void)base;
    const auto *event = static_cast<esp_mqtt_event_t *>(data);
    if (id == MQTT_EVENT_CONNECTED || id == MQTT_EVENT_DISCONNECTED) {
        clear_incoming();
        portENTER_CRITICAL(&report_lock);
        const uint32_t epoch = shared_report.epoch + 1;
        const char *error = shared_report.network_error;
        shared_report = {};
        shared_report.epoch = epoch;
        shared_report.connected = id == MQTT_EVENT_CONNECTED;
        shared_report.failed = id == MQTT_EVENT_DISCONNECTED;
        if (id == MQTT_EVENT_DISCONNECTED) shared_report.network_error = error;
        portEXIT_CRITICAL(&report_lock);
        if (id == MQTT_EVENT_CONNECTED) {
            LOG_MAIN("[lan] MQTT 已连接，访问码认证通过");
            if (esp_mqtt_client_subscribe(event->client, report_topic, 0) < 0) {
                portENTER_CRITICAL(&report_lock);
                shared_report.failed = true;
                shared_report.network_error = "打印机状态订阅请求发送失败";
                portEXIT_CRITICAL(&report_lock);
                LOG_MAIN("[lan] 打印机状态订阅请求发送失败");
            } else LOG_MAIN("[lan] 正在订阅 %s", report_topic);
        } else LOG_MAIN("[lan] MQTT 连接已断开");
    } else if (id == MQTT_EVENT_SUBSCRIBED || id == MQTT_EVENT_ERROR) {
        const bool subscribed = id == MQTT_EVENT_SUBSCRIBED &&
            (!event->error_handle || event->error_handle->error_type != MQTT_ERROR_TYPE_SUBSCRIBE_FAILED) &&
            !(event->data && event->data_len > 0 && static_cast<unsigned char>(event->data[0]) >= 0x80);
        const char *error = "MQTT 连接失败，请检查打印机 IP 和局域网设置";
        if (id == MQTT_EVENT_SUBSCRIBED) error = "打印机拒绝状态订阅，请检查序列号及局域网权限";
        if (id == MQTT_EVENT_ERROR && event->error_handle &&
            event->error_handle->error_type == MQTT_ERROR_TYPE_CONNECTION_REFUSED &&
            (event->error_handle->connect_return_code == MQTT_CONNECTION_REFUSE_BAD_USERNAME ||
             event->error_handle->connect_return_code == MQTT_CONNECTION_REFUSE_NOT_AUTHORIZED))
            error = "访问码错误或打印机拒绝局域网连接";
        portENTER_CRITICAL(&report_lock);
        if (subscribed) shared_report.subscribed = true;
        else { shared_report.failed = true; shared_report.network_error = error; }
        portEXIT_CRITICAL(&report_lock);
        if (subscribed) LOG_MAIN("[lan] 状态订阅成功，等待打印机报文");
        else {
            LOG_MAIN("[lan] %s", error);
            if (event->error_handle && event->error_handle->error_type == MQTT_ERROR_TYPE_TCP_TRANSPORT)
                LOG_MAIN("[lan] 网络诊断：ESP-TLS=0x%x，TLS=%d，socket=%d",
                         static_cast<unsigned>(event->error_handle->esp_tls_last_esp_err),
                         event->error_handle->esp_tls_stack_err, event->error_handle->esp_transport_sock_errno);
            else if (event->error_handle && event->error_handle->error_type == MQTT_ERROR_TYPE_CONNECTION_REFUSED)
                LOG_MAIN("[lan] MQTT 认证返回码：%d", static_cast<int>(event->error_handle->connect_return_code));
        }
    } else if (id == MQTT_EVENT_DATA) {
        if (!event->current_data_offset) {
            clear_incoming();
            if (event->retain || event->total_data_len <= 0 ||
                static_cast<size_t>(event->total_data_len) > MAX_REPORT_SIZE ||
                event->topic_len != static_cast<int>(strlen(report_topic)) ||
                !event->topic || memcmp(event->topic, report_topic, event->topic_len)) return;
            incoming_size = event->total_data_len;
            incoming = static_cast<char *>(malloc(incoming_size + 1));
        }
        if (!incoming) return;
        if (event->data_len < 0 || event->current_data_offset != static_cast<int>(incoming_used) ||
            event->total_data_len != static_cast<int>(incoming_size) ||
            incoming_used + event->data_len > incoming_size) { clear_incoming(); return; }
        memcpy(incoming + incoming_used, event->data, event->data_len);
        incoming_used += event->data_len;
        if (incoming_used == incoming_size) {
            incoming[incoming_size] = 0;
            receive_report(incoming);
            clear_incoming();
        }
    }
}

/**
 * 配置打印机局域网连接的自签证书兼容模式
 * @param config 当前 MQTT 连接的 TLS 配置
 * @return ESP_OK 表示已应用
 * 与 Top-AMS 一致不验证服务器证书，仅作用于用户指定 IP 的 MQTT 客户端，不改变 OTA TLS 配置
 */
esp_err_t printer_tls(void *config) {
    mbedtls_ssl_conf_authmode(static_cast<mbedtls_ssl_config *>(config), MBEDTLS_SSL_VERIFY_NONE);
    return ESP_OK;
}

/**
 * 异步连接已配置的打印机
 * 记录连接尝试但不输出访问码；不使用自动重连，断线后销毁客户端及发送队列，避免重放旧换料命令
 */
void connect_printer() {
    snprintf(broker_uri, sizeof(broker_uri), "mqtts://%s:8883", LAN_AMS_CONFIG.printer_ip);
    snprintf(request_topic, sizeof(request_topic), "device/%s/request", LAN_AMS_CONFIG.serial);
    snprintf(report_topic, sizeof(report_topic), "device/%s/report", LAN_AMS_CONFIG.serial);
    esp_mqtt_client_config_t config{};
#if ESP_IDF_VERSION_MAJOR >= 5
    config.broker.address.uri = broker_uri;
    config.broker.verification.crt_bundle_attach = printer_tls;
    config.broker.verification.skip_cert_common_name_check = true;
    config.credentials.username = "bblp";
    config.credentials.authentication.password = LAN_AMS_CONFIG.access_code;
    config.network.disable_auto_reconnect = true;
    config.network.timeout_ms = 2000;
    config.buffer.size = 2048;
    config.buffer.out_size = 512;
    config.outbox.limit = 2048;
#else
    config.uri = broker_uri;
    config.crt_bundle_attach = printer_tls;
    config.skip_cert_common_name_check = true;
    config.username = "bblp";
    config.password = LAN_AMS_CONFIG.access_code;
    config.disable_auto_reconnect = true;
    config.network_timeout_ms = 2000;
    config.buffer_size = 2048;
    config.out_buffer_size = 512;
#endif
    portENTER_CRITICAL(&report_lock);
    shared_report.failed = false;
    portEXIT_CRITICAL(&report_lock);
    attempted_at = millis();
    LOG_MAIN("[lan] 正在连接打印机 %s:8883（MQTT/TLS）", LAN_AMS_CONFIG.printer_ip);
    client = esp_mqtt_client_init(&config);
    if (!client || esp_mqtt_client_register_event(client, MQTT_EVENT_ANY, mqtt_event, nullptr) != ESP_OK ||
        esp_mqtt_client_start(client) != ESP_OK) {
        if (client) esp_mqtt_client_destroy(client);
        client = nullptr;
        last_error = "MQTT 初始化失败";
        next_connect = millis() + 5000;
        LOG_MAIN("[lan] %s，5 秒后重试", last_error);
    }
}

/**
 * 向网络任务提交一条控制消息
 * @param payload JSON 消息
 * @return 成功入队时返回 true
 * 使用 QoS 0 和非保留消息，主循环不等待网络发送
 */
bool publish(const char *payload) {
    return client && report.connected && report.subscribed &&
           esp_mqtt_client_enqueue(client, request_topic, payload, 0, 0, 0, true) >= 0;
}

/**
 * 提交一段打印机 G-code
 * @param code 内部生成的 G-code，允许换行
 * @return 成功入队时返回 true
 */
bool send_gcode(const char *code) {
    char escaped[320];
    size_t n = 0;
    for (const char *p = code; *p && n + 2 < sizeof(escaped); ++p) {
        if (*p == '\n') { escaped[n++] = '\\'; escaped[n++] = 'n'; }
        else escaped[n++] = *p;
    }
    escaped[n] = 0;
    char json[432];
    snprintf(json, sizeof(json), "{\"print\":{\"command\":\"gcode_line\",\"sequence_id\":\"0\",\"param\":\"%s\\n\"}}", escaped);
    return publish(json);
}

/**
 * 停止换料并使当前装料通道失效
 * @param reason 显示给用户的错误原因
 * 保持打印机暂停，等待人工检查后重新确认通道
 */
void fail(const char *reason) {
    for (int ch = 0; ch < active_channel_count(); ++ch) {
        const auto motor = BMCU_motor_status(ch);
        if (motor.source == MOTOR_LAN || motor.queued_source == MOTOR_LAN) BMCU_stop_motor(ch);
    }
    step = IDLE;
    current_channel = target_channel = -1;
    armed = false;
    motion_fault = true;
    reset_client_requested = true;
    if (last_error != reason) LOG_MAIN("[lan] %s", reason);
    last_error = reason;
}

/**
 * 切换非阻塞换料步骤并记录阶段与动作时长
 * @param next 下一步骤
 * @param duration 本步骤允许等待的毫秒数
 */
void advance(Step next, uint32_t duration = STEP_TIMEOUT_MS) {
    step = next;
    step_deadline = millis() + duration;
    switch (next) {
        case UNLOAD: LOG_MAIN("[lan] 已请求打印机退料，等待 AMS=260/259"); break;
        case RETRACT:
            LOG_MAIN("[lan] 通道 %d 开始退线，时长 %u 秒", current_channel + 1, PULLBACK_TIMEOUT[current_channel]);
            break;
        case UNLOADED: LOG_MAIN("[lan] 本地退线结束，等待打印机 AMS=0"); break;
        case HEAT: LOG_MAIN("[lan] 等待喷嘴加热至 %u°C，当前 %d°C", LAN_AMS_CONFIG.nozzle_temp, report.nozzle); break;
        case EXTRUDER: LOG_MAIN("[lan] 已请求挤出机进料，3 秒后启动送料电机"); break;
        case FEED:
            LOG_MAIN("[lan] 通道 %d 开始送线，时长 %lu ms", target_channel + 1,
                     static_cast<unsigned long>(LAN_AMS_CONFIG.feed_ms));
            break;
        case RESUME: LOG_MAIN("[lan] 已请求恢复打印，等待 RUNNING 确认"); break;
        default: break;
    }
}

/**
 * 判断电机是否已完成本次限时动作
 * @param channel 待检查通道
 * @return 电机已停止且没有排队动作时返回 true
 */
bool motor_finished(int channel) {
    const auto motor = BMCU_motor_status(channel);
    return !motor.direction && !motor.queue_position;
}

/**
 * 判断喷嘴是否已达到本次送料条件
 * @return 实际喷嘴温度新鲜且达到配置温度附近时返回 true
 */
bool nozzle_ready() {
    return report.nozzle_time && millis() - report.nozzle_time < REPORT_TIMEOUT_MS &&
           report.nozzle >= LAN_AMS_CONFIG.nozzle_temp - 5;
}

/**
 * 接受由热床温度和暂停状态组成的换料请求
 * 只有本会话已观察到正常打印、确认当前通道且本地空闲时才执行，记录目标通道及热床恢复温度
 */
void begin_change() {
    const int next = report.bed - 1;
    armed = false;
    if (next < 0 || next >= active_channel_count() || current_channel < 0 ||
        current_channel >= active_channel_count() || previous_bed < 17) {
        fail("换料通道或原热床温度未知，请检查后重新确认当前通道");
        return;
    }
    if (!BMCU_can_maintain() || !BMCU_channel_motor_ready(current_channel) ||
        !BMCU_channel_motor_ready(next) || CHANNEL_MODE[current_channel] == CHANNEL_MODE_OFFLINE ||
        CHANNEL_MODE[next] == CHANNEL_MODE_OFFLINE || BMCU_channel_filament_state(next) == 0) {
        fail("换料通道无料、电机未配置或本地运动尚未结束");
        return;
    }
    if (report.print_error || report.denied) { fail("打印机存在错误或拒绝了局域网控制"); return; }
    target_channel = next;
    LOG_MAIN("[lan] 开始换料：通道 %d → %d", current_channel + 1, target_channel + 1);
    char code[128];
    snprintf(code, sizeof(code), "M190 S%d", previous_bed);
    if (!send_gcode(code)) { fail("恢复热床温度请求发送失败"); return; }
    LOG_MAIN("[lan] 已请求恢复热床温度至 %d°C", previous_bed);
    if (next == current_channel) {
        advance(RESUME, 15000);
        if (!publish("{\"print\":{\"command\":\"resume\",\"sequence_id\":\"0\"}}")) fail("恢复打印请求发送失败");
        return;
    }
    expected_ams_revision = report.ams_revision;
    snprintf(code, sizeof(code), "M109 S%u\nM620 S255\nT255\nM621 S255", LAN_AMS_CONFIG.nozzle_temp);
    if (!send_gcode(code)) { fail("打印机退料请求发送失败"); return; }
    advance(UNLOAD);
}

/**
 * 推进一次 Top-AMS 定时换料流程
 * 实际喷嘴温度到位后才送线；每一步有期限，退料状态接受项目注明的 A1/P1 两种值
 */
void run_change() {
    const uint32_t now = millis();
    if (step == IDLE) {
        if (report.state == ENDED || report.bed == 0) { armed = false; previous_bed = -1; }
        if (report.state == RUNNING && report.bed >= 17 && current_channel >= 0 &&
            now - report.state_time < REPORT_TIMEOUT_MS && now - report.bed_time < REPORT_TIMEOUT_MS) {
            previous_bed = report.bed;
            armed = true;
        }
        if (armed && report.state == PAUSED && report.bed > 0 && report.bed <= 16 &&
            now - report.bed_time < REPORT_TIMEOUT_MS && now - report.state_time < REPORT_TIMEOUT_MS) begin_change();
        return;
    }
    if (report.denied || report.print_error) { fail("打印机报错或拒绝控制，请检查局域网控制权限"); return; }
    if (now - report.state_time >= REPORT_TIMEOUT_MS ||
        (report.state != PAUSED && !(step == RESUME && report.state == RUNNING))) {
        fail("打印状态已改变，已停止局域网换料"); return;
    }
    if ((step == EXTRUDER || step == FEED) && !nozzle_ready()) {
        fail("喷嘴温度不足或状态过期，已停止送料"); return;
    }
    if (step != EXTRUDER && static_cast<int32_t>(now - step_deadline) >= 0) {
        fail("换料步骤超时，已停止电机，请检查打印机状态"); return;
    }
    switch (step) {
        case UNLOAD:
            if (report.ams_revision != expected_ams_revision && (report.ams == 260 || report.ams == 259)) {
                const uint32_t duration = PULLBACK_TIMEOUT[current_channel] * 1000u;
                advance(RETRACT, duration + 10000);
                if (!motorCmd(current_channel, -1, MOTOR_LAN, duration)) fail("退料电机无法启动");
            }
            break;
        case RETRACT:
            if (motor_finished(current_channel)) advance(UNLOADED);
            break;
        case UNLOADED:
            if (report.ams == 0 && report.ams_revision != expected_ams_revision) {
                char code[32];
                snprintf(code, sizeof(code), "M109 S%u", LAN_AMS_CONFIG.nozzle_temp);
                if (!send_gcode(code)) { fail("加热请求发送失败"); break; }
                advance(HEAT);
            }
            break;
        case HEAT:
            if (nozzle_ready()) {
                if (!send_gcode("G1 E150 F500")) { fail("挤出机进料请求发送失败"); break; }
                advance(EXTRUDER, 3000);
            }
            break;
        case EXTRUDER:
            if (static_cast<int32_t>(now - step_deadline) >= 0) {
                advance(FEED, LAN_AMS_CONFIG.feed_ms + 10000);
                if (!motorCmd(target_channel, 1, MOTOR_LAN, LAN_AMS_CONFIG.feed_ms)) fail("进料电机无法启动");
            }
            break;
        case FEED:
            if (motor_finished(target_channel)) {
                current_channel = target_channel;
                if (!publish("{\"print\":{\"command\":\"resume\",\"sequence_id\":\"0\"}}")) {
                    fail("恢复打印请求发送失败"); break;
                }
                advance(RESUME, 15000);
            }
            break;
        case RESUME:
            if (report.state == RUNNING) {
                LOG_MAIN("[lan] 换料完成，当前通道 %d", current_channel + 1);
                target_channel = -1;
                step = IDLE;
            }
            break;
        default: break;
    }
}
} // namespace

/**
 * 判断是否启用局域网换料
 * @return ESP32 的 RX/TX 均未配置时返回 true
 */
bool lan_ams_enabled() { return BUS_RX == PIN_NONE && BUS_TX == PIN_NONE; }

/**
 * 查询局域网换料是否占用运动调度
 * @return 正在等待打印机或执行电机动作时返回 true
 */
bool lan_ams_busy() { return step != IDLE; }

/**
 * 检查局域网电机请求是否属于当前有效步骤
 * @param channel 全局通道编号
 * @return 在线且对应进退料步骤时返回 true
 */
bool lan_ams_motor_allowed(int channel) {
    return paired() && ((step == RETRACT && channel == current_channel) ||
                        (step == FEED && channel == target_channel));
}

/**
 * 获取已确认且在线的当前装料通道
 * @return 正在打印时的全局通道编号，未知、换料中或状态失效时返回 -1
 */
int lan_ams_active_channel() {
    return paired() && !lan_ams_busy() && report.state == RUNNING &&
           millis() - report.state_time < REPORT_TIMEOUT_MS ? current_channel : -1;
}

/**
 * 查询配对和换料状态
 * @return 供网页与有线兼容接口读取的状态快照
 */
LanAmsStatus lan_ams_status() {
    static const char *steps[] = {"已配对", "等待打印机退料", "正在退线", "等待退料结束", "等待喷嘴加热", "等待挤出机进料", "正在送线", "等待恢复打印"};
    const char *state = !lan_ams_enabled() ? "未启用" : !LAN_AMS_CONFIG.printer_ip[0] ? "未配置" :
                        WiFi.status() != WL_CONNECTED ? "等待 WiFi" : !report.connected ? "等待连接打印机" :
                        !paired() ? "等待打印机状态" : last_error[0] ? "需要检查" :
                        current_channel < 0 ? "已配对，请确认当前通道" : steps[step];
    return {report.connected, paired(), lan_ams_busy(), last_error[0] != 0,
            current_channel, target_channel, report.received ? millis() - report.received : UINT32_MAX,
            state, last_error};
}

/**
 * 提交人工确认的当前装料通道
 * @param channel 全局通道编号，-1 表示未知
 * @return 已配对、空闲且通道可用时返回 true
 */
bool lan_ams_request_channel(int channel) {
    if (!lan_ams_enabled() || !paired() || lan_ams_busy() || !BMCU_can_maintain() ||
        channel < -1 || channel >= active_channel_count() ||
        (channel >= 0 && (!BMCU_channel_motor_ready(channel) || CHANNEL_MODE[channel] == CHANNEL_MODE_OFFLINE))) return false;
    channel_request = channel;
    return true;
}

/**
 * 提交停止局域网换料的请求
 * 网页回调只设置标记，下一次主循环执行停车
 */
void lan_ams_request_cancel() { cancel_requested = true; }

/**
 * 在主循环中取消局域网换料并清空电机队列
 */
void lan_ams_cancel() { fail("已停止局域网换料，请重新确认当前通道"); }

/**
 * 在主循环中处理网络状态、人工请求和换料步骤
 * 断线先停车再销毁客户端，重建连接清空旧请求且要求重新确认通道
 * 等待配置、WiFi、配对与状态超时仅在变化时记录，重试时记录原因
 */
void lan_ams_run() {
    if (!lan_ams_enabled()) return;
    report = read_report();
    const uint32_t now = millis();
    const int waiting = !settings_lan_config_valid(LAN_AMS_CONFIG, true) ? 1 : WiFi.status() != WL_CONNECTED ? 2 : 0;
    if (waiting != wait_state) {
        if (waiting == 1) LOG_MAIN("[lan] 局域网模式已启用，请填写有效的打印机 IP、访问码和序列号");
        if (waiting == 2) LOG_MAIN("[lan] 等待 WiFi 连接后配对打印机");
        wait_state = waiting;
    }
    if (cancel_requested) { cancel_requested = false; channel_request = -2; lan_ams_cancel(); }
    if (report.epoch != seen_epoch) {
        if (seen_epoch && (current_channel >= 0 || lan_ams_busy())) fail("打印机连接已改变，请重新确认当前通道");
        seen_epoch = report.epoch;
        previous_bed = -1;
        channel_request = -2;
        armed = poll_sent = pair_logged = false;
    }
    if (client && (reset_client_requested || WiFi.status() != WL_CONNECTED || report.failed ||
        (!report.connected && now - attempted_at > REPORT_TIMEOUT_MS))) {
        if (lan_ams_busy() || current_channel >= 0) fail("打印机连接中断，已停止电机");
        if (!last_error[0]) last_error = report.network_error[0] ? report.network_error : "无法连接打印机，请检查 IP、访问码及局域网控制权限";
        esp_mqtt_client_stop(client);
        esp_mqtt_client_destroy(client);
        client = nullptr;
        clear_incoming();
        portENTER_CRITICAL(&report_lock);
        const uint32_t epoch = shared_report.epoch + 1;
        shared_report = {};
        shared_report.epoch = epoch;
        portEXIT_CRITICAL(&report_lock);
        report = {};
        next_connect = now + 5000;
        poll_sent = pair_logged = false;
        LOG_MAIN("[lan] %s，5 秒后重试", last_error);
    }
    reset_client_requested = false;
    if (!client && WiFi.status() == WL_CONNECTED && settings_lan_config_valid(LAN_AMS_CONFIG, true) &&
        static_cast<int32_t>(now - next_connect) >= 0) connect_printer();
    if (report.connected && report.subscribed && (!poll_sent || static_cast<int32_t>(now - next_poll) >= 0)) {
        poll_sent = publish("{\"pushing\":{\"command\":\"pushall\",\"sequence_id\":\"0\"}}");
        next_poll = now + 5000;
    }
    if (!motion_fault && report.connected && !report.received && now - attempted_at >= REPORT_TIMEOUT_MS) {
        const char *error = "已连接但未收到打印机状态，请检查序列号及局域网权限";
        if (strcmp(last_error, error)) LOG_MAIN("[lan] %s", error);
        last_error = error;
    }
    if (!paired()) {
        if (pair_logged) LOG_MAIN("[lan] 超过 15 秒未收到有效状态，等待打印机重新上报");
        pair_logged = false;
        if (lan_ams_busy() || current_channel >= 0) fail("打印机状态已过期，已停止电机");
        channel_request = -2;
        return;
    }
    if (!pair_logged) {
        LOG_MAIN("[lan] 配对成功，已收到打印机状态，请确认当前装料通道");
        pair_logged = true;
    }
    if (!motion_fault) last_error = "";
    if (channel_request != -2) {
        current_channel = channel_request;
        channel_request = -2;
        last_error = "";
        motion_fault = false;
        armed = false;
        if (current_channel >= 0) LOG_MAIN("[lan] 人工确认当前通道：%d", current_channel + 1);
        else LOG_MAIN("[lan] 当前装料通道已设为未知");
    }
    run_change();
}

#else

/**
 * 判断非 ESP32 平台是否启用局域网
 * @return 始终返回 false
 */
bool lan_ams_enabled() { return false; }
/** 提供非 ESP32 平台的空轮询入口 */
void lan_ams_run() {}
/**
 * 查询非 ESP32 平台换料状态
 * @return 始终返回 false
 */
bool lan_ams_busy() { return false; }
/**
 * 检查非 ESP32 平台局域网电机请求
 * @param channel 通道编号
 * @return 始终返回 false
 */
bool lan_ams_motor_allowed(int channel) { (void)channel; return false; }
/**
 * 获取非 ESP32 平台局域网通道
 * @return 始终返回 -1
 */
int lan_ams_active_channel() { return -1; }
/**
 * 获取非 ESP32 平台局域网状态
 * @return 未启用状态
 */
LanAmsStatus lan_ams_status() { return {false, false, false, false, -1, -1, UINT32_MAX, "未启用", ""}; }
/**
 * 提交非 ESP32 平台通道请求
 * @param channel 通道编号
 * @return 始终返回 false
 */
bool lan_ams_request_channel(int channel) { (void)channel; return false; }
/** 忽略非 ESP32 平台局域网停止请求 */
void lan_ams_request_cancel() {}
/** 提供非 ESP32 平台局域网取消入口 */
void lan_ams_cancel() {}

#endif
