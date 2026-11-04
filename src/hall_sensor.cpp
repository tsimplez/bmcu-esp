#include "hall_sensor.h"
#include "settings_storage.h"
#include "platform.h"
#include "PCF8575_PinMap.h"
#include <cstring>

namespace {

struct HallSlot {
    HallHistoryPoint history[HALL_HISTORY_CAPACITY];
    uint32_t last_sample_ms;
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
            if (bucket == 0 && elapsed <= HALL_HISTORY_CAPACITY && slot.samples) {
                point.average = static_cast<uint16_t>((slot.sum + slot.samples / 2u) / slot.samples);
                point.minimum = slot.minimum;
                point.maximum = slot.maximum;
            }
            slot.history[next_history_index] = point;
        }
        next_history_index = (next_history_index + 1u) % HALL_HISTORY_CAPACITY;
        if (history_count < HALL_HISTORY_CAPACITY) ++history_count;
        ++history_cursor;
    }
    history_end_ms += elapsed * HALL_HISTORY_INTERVAL_MS;
    for (uint8_t index = 0; index < slot_count; ++index) clear_accumulator(slots[index]);
}

} // namespace

/**
 * 初始化霍尔采集与历史记录
 * 创建本次启动的非零历史标识，在主循环启动前应用已加载的 ADC 接线
 */
void hall_sensor_init() {
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
    const uint8_t count = requested_count < 0 ? 0 :
        requested_count > IO_CONFIG_MAX_CHANNELS ? IO_CONFIG_MAX_CHANNELS : requested_count;
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
 */
void hall_sensor_run() {
    hall_sensor_sync_config();
    const uint32_t now = millis();
    complete_buckets(now);
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
        if (slot.last_read_ok) {
            slot.has_raw = true;
            slot.raw = raw;
            slot.raw_ms = now;
            slot.sum += raw;
            ++slot.samples;
            if (raw < slot.minimum) slot.minimum = raw;
            if (raw > slot.maximum) slot.maximum = raw;
        }
        break;
    }
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
 * 读取通道最新值和最近一分钟统计
 * @param channel 全局通道编号
 * @return 运行期引脚、配置状态、当前值及分钟极值和平均值，缺数据时对应有效标志为 false
 * 最新转换超过五百毫秒即过期；分钟平均值按仍在六十秒内的有效完整桶等权计算
 */
HallChannelStatus hall_sensor_status(const int channel) {
    HallChannelStatus status = {PIN_NONE, false, false, false, HALL_INVALID_VALUE,
        HALL_INVALID_VALUE, HALL_INVALID_VALUE, HALL_INVALID_VALUE, UINT32_MAX};
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
    }
    status.statistics_valid = valid_count > 0;
    if (valid_count) status.average = static_cast<uint16_t>((sum + valid_count / 2u) / valid_count);
    return status;
}
