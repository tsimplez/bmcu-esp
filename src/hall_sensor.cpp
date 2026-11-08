#include "hall_sensor.h"
#include "settings_storage.h"
#include "platform.h"
#include "PCF8575_PinMap.h"
#include "log_bus.h"
#include "BambuBus.h"
#include <cstring>

namespace {
    constexpr uint32_t HALL_LOG_INTERVAL_MS = 1000;
    constexpr uint16_t HALL_AVERAGE_BUCKETS = 5000 / HALL_HISTORY_INTERVAL_MS;
    static_assert(HALL_HISTORY_CAPACITY % HALL_AVERAGE_BUCKETS == 0, "霍尔短期统计槽须与历史环形槽对齐");

    struct HallAverageBucket {
        uint32_t sum;
        uint16_t samples;
    };

    struct HallSlot {
        HallHistoryPoint history[HALL_HISTORY_CAPACITY];
        HallAverageBucket averages[HALL_AVERAGE_BUCKETS];
        uint32_t last_sample_ms;
        uint32_t last_log_ms;
        uint32_t raw_ms;
        uint32_t sum;
        uint16_t samples;
        uint16_t minimum;
        uint16_t maximum;
        uint16_t raw;
        bool has_raw;
        bool last_read_ok;
    };

    // 每个 ADC 最多对应一个通道，C3 只预留五份历史，避免按十六通道浪费常驻内存。
    HallSlot slots[HAL_HALL_ADC_CAPACITY];
    int runtime_pins[IO_CONFIG_MAX_CHANNELS];
    int8_t channel_slots[IO_CONFIG_MAX_CHANNELS];
    uint8_t channel_count = 0;
    uint8_t slot_count = 0;
    uint8_t next_channel = 0;
    uint16_t history_count = 0;
    uint16_t next_history_index = 0;
    uint32_t history_cursor = 0;
    uint32_t history_end_ms = 0;
    uint32_t history_epoch = 0;
    bool initialized = false;

    enum class DetectionState : uint8_t {inactive, waiting, monitoring, timeout, sensor_error};
    struct HallDetector {
        int channel = -1;
        int pin = PIN_NONE;
        uint32_t epoch = 0;
        HallDetectionConfig config = DEFAULT_HALL_DETECTION;
        DetectionState state = DetectionState::inactive;
        uint32_t reference_ms = 0;
        uint32_t sample_ms = 0;
        bool has_sample = false;
        int8_t zone = 0;
        int8_t candidate = 0;
        uint8_t confirmations = 0;
    } detector;
    HallDetectionEvent last_event = {0, -1, "none"};

    /**
     * 发布霍尔状态变化事件
     * @param type 事件类型
     * 只保留最后一个事件及递增序号，供后续主循环调用方去重；不发出暂停或电机命令
     */
    void publish_detection_event(const char *type) {
        if (++last_event.sequence == 0) ++last_event.sequence;
        last_event.channel = detector.channel;
        last_event.type = type;
    }

    /**
     * 将当前检测标记为采样异常
     * 清除阈值确认和旧计时，避免把无数据当作停止转动；异常只记录一次
     */
    void detector_sample_error() {
        if (detector.state != DetectionState::sensor_error) {
            publish_detection_event("sensor_error");
            LOG_MAIN("[hall] 通道 %d 检测暂停：霍尔采样异常", detector.channel + 1);
        }
        detector.state = DetectionState::sensor_error;
        detector.has_sample = false;
        detector.zone = detector.candidate = 0;
        detector.confirmations = 0;
    }

    /**
     * 维护唯一使用通道的双阈值停转检测
     * @param now 当前毫秒时刻
     * @param sampled_channel 本轮实际采样的通道，没有采样时为 -1
     * @param valid 本次转换是否有效
     * @param raw 本次 ADC 原始值
     * 连续三次采样位于同一阈值外才确认区域；首次确认只建立基准，之后高低区交替才刷新移动时间
     * 切换通道、配置或接线重新计时；初始化或读取失败、超过五百毫秒无采样单独报错，恢复后重新建立基准
     */
    void update_detector(const uint32_t now, const int sampled_channel, const bool valid, const uint16_t raw) {
        // const int channel = BambuBus_active_using_channel();
        const int channel = 0;
        if (channel < 0 || channel >= channel_count || runtime_pins[channel] == PIN_NONE ||
            !settings_hall_detection_valid(HALL_DETECTION[channel]) || !HALL_DETECTION[channel].timeout_s) {
            if (detector.channel >= 0)
                LOG_MAIN("[hall] 通道 %d 已退出停转检测", detector.channel + 1);
            detector = HallDetector();
            return;
        }
        const auto &config = HALL_DETECTION[channel];
        if (detector.channel != channel || detector.pin != runtime_pins[channel] || detector.epoch != history_epoch ||
            detector.config.lower != config.lower || detector.config.upper != config.upper ||
            detector.config.timeout_s != config.timeout_s) {
            detector = HallDetector();
            detector.channel = channel;
            detector.pin = runtime_pins[channel];
            detector.epoch = history_epoch;
            detector.config = config;
            detector.state = DetectionState::waiting;
            detector.reference_ms = now;
            LOG_MAIN("[hall] 通道 %d 开始停转检测，下限=%u，上限=%u，超时=%lu 秒", channel + 1,
                     static_cast<unsigned>(config.lower), static_cast<unsigned>(config.upper),
                     static_cast<unsigned long>(config.timeout_s));
        }
        if ((detector.has_sample && now - detector.sample_ms > 500u) ||
            (detector.state == DetectionState::waiting && now - detector.reference_ms > 500u))
            detector_sample_error();
        if (sampled_channel == channel) {
            if (!valid) {
                detector_sample_error();
                return;
            }
            if (!detector.has_sample) {
                if (detector.state == DetectionState::sensor_error) {
                    publish_detection_event("sensor_restored");
                    LOG_MAIN("[hall] 通道 %d 采样恢复，重新开始停转检测", channel + 1);
                }
                detector.state = DetectionState::monitoring;
                detector.reference_ms = now;
                detector.has_sample = true;
            }
            detector.sample_ms = now;
            if (const int8_t zone = raw <= config.lower ? -1 : raw >= config.upper ? 1 : 0; !zone) {
                detector.candidate = 0;
                detector.confirmations = 0;
            } else {
                if (zone != detector.candidate) {
                    detector.candidate = zone;
                    detector.confirmations = 0;
                }
                if (detector.confirmations < 3) ++detector.confirmations;
                if (detector.confirmations == 3 && zone != detector.zone) {
                    if (detector.zone) {
                        detector.reference_ms = now;
                        if (detector.state == DetectionState::timeout) {
                            publish_detection_event("recovered");
                            LOG_MAIN("[hall] 通道 %d 检测到移动，停转告警解除", channel + 1);
                        }
                        detector.state = DetectionState::monitoring;
                    }
                    detector.zone = zone;
                }
            }
        }
        if (detector.state == DetectionState::monitoring &&
            now - detector.reference_ms >= config.timeout_s * 1000u) {
            detector.state = DetectionState::timeout;
            publish_detection_event("timeout");
            LOG_MAIN("[hall] 通道 %d 已连续 %lu 秒未检测到移动，仅告警", channel + 1,
                     static_cast<unsigned long>(config.timeout_s));
        }
    }

    /**
     * 清空当前统计桶的累加值
     * @param slot 待清空的 ADC 槽
     */
    void clear_accumulator(HallSlot &slot) {
        slot.sum = 0;
        slot.samples = 0;
        slot.minimum = HALL_INVALID_VALUE;
        slot.maximum = 0;
    }

    /**
     * 结算已结束的统计桶并补齐采样空缺
     * @param now 当前毫秒时刻
     * 长时间未调度时最多写三百个桶；空缺以无效值保存，不复制上一读数
     * 最近五秒额外保留各桶的原始总和与样本数，供短期均值按有效样本数加权计算
     */
    void complete_buckets(const uint32_t now) {
        const uint32_t elapsed = (now - history_end_ms) / HALL_HISTORY_INTERVAL_MS;
        if (!elapsed) return;
        const uint32_t retained = elapsed > HALL_HISTORY_CAPACITY ? HALL_HISTORY_CAPACITY : elapsed;
        history_cursor += elapsed - retained;
        if (elapsed > HALL_HISTORY_CAPACITY) history_count = 0;
        for (uint32_t bucket = 0; bucket < retained; ++bucket) {
            for (uint8_t index = 0; index < slot_count; ++index) {
                auto &slot = slots[index];
                HallHistoryPoint point = {HALL_INVALID_VALUE, HALL_INVALID_VALUE, HALL_INVALID_VALUE};
                HallAverageBucket average = {};
                if (bucket == 0 && elapsed <= HALL_HISTORY_CAPACITY && slot.samples) {
                    point.average = static_cast<uint16_t>((slot.sum + slot.samples / 2u) / slot.samples);
                    point.minimum = slot.minimum;
                    point.maximum = slot.maximum;
                    average = {slot.sum, slot.samples};
                }
                slot.history[next_history_index] = point;
                slot.averages[next_history_index % HALL_AVERAGE_BUCKETS] = average;
            }
            next_history_index = (next_history_index + 1u) % HALL_HISTORY_CAPACITY;
            if (history_count < HALL_HISTORY_CAPACITY) ++history_count;
            ++history_cursor;
        }
        history_end_ms += elapsed * HALL_HISTORY_INTERVAL_MS;
        for (uint8_t index = 0; index < slot_count; ++index) clear_accumulator(slots[index]);
    }
}

/**
 * 初始化霍尔采集与历史记录
 * 创建本次启动的非零历史标识，清空检测和事件，在主循环启动前应用已加载的 ADC 接线
 */
void hall_sensor_init() {
    detector = HallDetector();
    last_event = {0, -1, "none"};
    history_epoch = hal_hall_epoch();
    if (!history_epoch) history_epoch = 1;
    initialized = false;
    hall_sensor_sync_config();
}

/**
 * 在主循环同步已保存的霍尔接线
 * 仅在启用数量或有效通道的霍尔引脚变化时重建采样槽并清空历史；不写存储
 */
void hall_sensor_sync_config() {
    const int requested_count = active_channel_count();
    const uint8_t count = requested_count < 0
                              ? 0
                              : requested_count > IO_CONFIG_MAX_CHANNELS
                                    ? IO_CONFIG_MAX_CHANNELS
                                    : requested_count;
    bool changed = !initialized || count != channel_count;
    for (uint8_t channel = 0; channel < IO_CONFIG_MAX_CHANNELS; ++channel) {
        const int pin = channel < count ? IO_CONFIG.hall[channel] : PIN_NONE;
        if (runtime_pins[channel] != pin) changed = true;
    }
    if (!changed) return;
    const uint32_t now = millis();
    if (initialized && ++history_epoch == 0) history_epoch = 1;
    if (!history_epoch) {
        history_epoch = hal_hall_epoch();
        if (!history_epoch) history_epoch = 1;
    }
    channel_count = count;
    slot_count = 0;
    next_channel = 0;
    history_count = 0;
    next_history_index = 0;
    history_cursor = 0;
    history_end_ms = now;
    memset(slots, 0, sizeof(slots));
    for (uint8_t channel = 0; channel < IO_CONFIG_MAX_CHANNELS; ++channel) {
        const int pin = channel < count ? IO_CONFIG.hall[channel] : PIN_NONE;
        runtime_pins[channel] = pin;
        channel_slots[channel] = -1;
        if (slot_count >= HAL_HALL_ADC_CAPACITY || !io_config_hall_pin_available(pin)) continue;
        // 正常设置已拒绝重复引脚；运行期仍做保护，防止坏配置重复占用同一 ADC。
        bool duplicate = false;
        for (uint8_t previous = 0; previous < channel; ++previous)
            if (channel_slots[previous] >= 0 && runtime_pins[previous] == pin) duplicate = true;
        if (duplicate) continue;
        pinMode_PCF8575(pin, INPUT);
        if (!hal_adc_prepare(pin)) continue;
        auto &slot = slots[slot_count];
        slot.last_sample_ms = now - HALL_SAMPLE_INTERVAL_MS;
        slot.last_log_ms = now - HALL_LOG_INTERVAL_MS;
        slot.raw = HALL_INVALID_VALUE;
        clear_accumulator(slot);
        channel_slots[channel] = slot_count++;
    }
    initialized = true;
}

/**
 * 按节拍采集霍尔模拟值
 * 每轮最多转换一个到期通道，目标每通道二十次每秒；错过的采样不集中补读
 * 先结算过去的统计桶再读取当前值，使长停顿和毫秒回绕保持正确时间轴
 * 每通道首次采样及此后每秒将当前 ADC 值或读取失败写入主板日志，不补打错过的日志
 * 所有通道保留采样和历史，仅对当前已确认使用的一个通道执行停转检测
 */
void hall_sensor_run() {
    hall_sensor_sync_config();
    const uint32_t now = millis();
    complete_buckets(now);
    int sampled_channel = -1;
    bool sample_valid = false;
    uint16_t sample_raw = HALL_INVALID_VALUE;
    for (uint8_t visited = 0; visited < channel_count; ++visited) {
        const uint8_t channel = next_channel;
        next_channel = (next_channel + 1u) % channel_count;
        const int index = channel_slots[channel];
        if (index < 0) continue;
        auto &slot = slots[index];
        if (now - slot.last_sample_ms < HALL_SAMPLE_INTERVAL_MS) continue;
        slot.last_sample_ms = now;
        uint16_t raw = HALL_INVALID_VALUE;
        slot.last_read_ok = hal_adc_read(runtime_pins[channel], raw) && raw <= 4095;
        sampled_channel = channel;
        sample_valid = slot.last_read_ok;
        sample_raw = raw;
        if (slot.last_read_ok) {
            slot.has_raw = true;
            slot.raw = raw;
            slot.raw_ms = now;
            slot.sum += raw;
            ++slot.samples;
            if (raw < slot.minimum) slot.minimum = raw;
            if (raw > slot.maximum) slot.maximum = raw;
        }
        if (now - slot.last_log_ms >= HALL_LOG_INTERVAL_MS) {
            slot.last_log_ms = now;
            if (slot.last_read_ok)
                LOG_MAIN("[hall] 通道 %d IO%d ADC=%u", channel + 1, runtime_pins[channel],
                         static_cast<unsigned>(raw));
            else
                LOG_MAIN("[hall] 通道 %d IO%d 读取失败", channel + 1, runtime_pins[channel]);
        }
        break;
    }
    update_detector(now, sampled_channel, sample_valid, sample_raw);
}

/**
 * 读取固件停转检测快照
 * @return 当前唯一检测通道、状态、无移动时间和超时阈值
 * 只读取状态；超时锁存不因毫秒计数回绕解除，待采样及异常期间不累计停转时间
 */
HallDetectionStatus hall_sensor_detection_status() {
    const char *state = "inactive";
    uint32_t idle = 0;
    switch (detector.state) {
        case DetectionState::waiting: state = "waiting"; break;
        case DetectionState::monitoring: state = "monitoring"; break;
        case DetectionState::timeout: state = "timeout"; break;
        case DetectionState::sensor_error: state = "sensor_error"; break;
        default: break;
    }
    const uint32_t timeout = detector.config.timeout_s * 1000u;
    if (detector.state == DetectionState::monitoring || detector.state == DetectionState::timeout) {
        idle = millis() - detector.reference_ms;
        if (detector.state == DetectionState::timeout && idle < timeout) idle = timeout;
    }
    return {detector.channel, state, idle, timeout};
}

/**
 * 读取最后一次检测状态变化事件
 * @return 事件序号、所属通道及类型；没有事件时序号为零
 * 查询不会消费事件；后续暂停调用方需按序号去重并再次确认通道和实时超时状态
 */
HallDetectionEvent hall_sensor_last_event() {
    return last_event;
}

/**
 * 读取当前历史时间轴信息
 * @return 本次启动或接线版本、最近完整桶的游标及结束时刻、有效容量和通道数
 * 只读取状态，不在 HTTP 回调中采样或补桶
 */
HallHistoryInfo hall_sensor_history_info() {
    return {history_epoch, history_cursor, millis(), history_end_ms, history_count, channel_count};
}

/**
 * 读取指定通道的一条历史统计
 * @param channel 全局通道编号
 * @param cursor 请求的完整桶游标，允许无符号回绕
 * @param point 输出平均值和极值；没有采样的桶返回无效值
 * @return true 表示通道已配置且该游标仍在缓存中
 */
bool hall_sensor_history_point(const int channel, const uint32_t cursor, HallHistoryPoint &point) {
    if (channel < 0 || channel >= channel_count || channel_slots[channel] < 0) return false;
    const uint32_t distance = history_cursor - cursor;
    if (distance >= history_count) return false;
    const uint16_t index = (next_history_index + HALL_HISTORY_CAPACITY - 1u - distance) % HALL_HISTORY_CAPACITY;
    point = slots[channel_slots[channel]].history[index];
    return true;
}

/**
 * 读取通道最新值、最近一秒和五秒均值及最近一分钟统计
 * @param channel 全局通道编号
 * @return 运行期引脚、配置状态、当前值及统计；短期均值无样本时为 HALL_INVALID_VALUE，其余数据由有效标志判断
 * 最新转换超过五百毫秒即过期；分钟平均值按仍在六十秒内的有效完整桶等权计算
 * 一秒和五秒均值使用各窗口内完整桶的原始总和与样本数，不足窗口时按已有样本计算，缺失不补零
 */
HallChannelStatus hall_sensor_status(const int channel) {
    HallChannelStatus status = {
        PIN_NONE, false, false, false, HALL_INVALID_VALUE,
        HALL_INVALID_VALUE, HALL_INVALID_VALUE, HALL_INVALID_VALUE,
        HALL_INVALID_VALUE, HALL_INVALID_VALUE, UINT32_MAX
    };
    if (channel < 0 || channel >= channel_count) return status;
    status.pin = runtime_pins[channel];
    const int slot_index = channel_slots[channel];
    if (slot_index < 0) return status;
    const uint32_t now = millis();
    const auto &slot = slots[slot_index];
    status.configured = true;
    status.age_ms = slot.has_raw ? now - slot.raw_ms : UINT32_MAX;
    status.valid = slot.has_raw && slot.last_read_ok && status.age_ms <= 500u;
    status.raw = slot.raw;
    uint32_t sum = 0;
    uint16_t valid_count = 0;
    uint32_t sum_1s = 0, sum_5s = 0;
    uint16_t samples_1s = 0, samples_5s = 0;
    for (uint16_t distance = 0; distance < history_count; ++distance) {
        const uint32_t age = now - history_end_ms + distance * HALL_HISTORY_INTERVAL_MS;
        if (age >= HALL_HISTORY_INTERVAL_MS * HALL_HISTORY_CAPACITY) break;
        const uint16_t index = (next_history_index + HALL_HISTORY_CAPACITY - 1u - distance) % HALL_HISTORY_CAPACITY;
        const auto &point = slot.history[index];
        if (point.average == HALL_INVALID_VALUE) continue;
        if (!valid_count || point.minimum < status.minimum) status.minimum = point.minimum;
        if (!valid_count || point.maximum > status.maximum) status.maximum = point.maximum;
        sum += point.average;
        ++valid_count;
        if (age < 5000u && distance < HALL_AVERAGE_BUCKETS) {
            const auto &average = slot.averages[index % HALL_AVERAGE_BUCKETS];
            sum_5s += average.sum;
            samples_5s += average.samples;
            if (age < 1000u) {
                sum_1s += average.sum;
                samples_1s += average.samples;
            }
        }
    }
    status.statistics_valid = valid_count > 0;
    if (valid_count) status.average = static_cast<uint16_t>((sum + valid_count / 2u) / valid_count);
    if (samples_1s) status.average_1s = static_cast<uint16_t>((sum_1s + samples_1s / 2u) / samples_1s);
    if (samples_5s) status.average_5s = static_cast<uint16_t>((sum_5s + samples_5s / 2u) / samples_5s);
    return status;
}
