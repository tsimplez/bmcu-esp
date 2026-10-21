// 主机测试的硬件边界；实际队列、计时、协议、保存校验均来自 src/BambuBus.cpp
#include <cassert>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <string>
#include <vector>
#include <deque>
#ifndef BMCU_TEST_WASI
#include <thread>
#include <atomic>
#endif
#include <algorithm>
#include "crc_bus.h"

using String = std::string;
static uint32_t fake_ms = 0;
static uint32_t clock_step = 0;
static int AMS_COUNT = 1;
static int RS485_MODE = 0;
static int BUS_LOG_ENABLE = 0;
static constexpr int RS485_MODE_DE = 1;
static constexpr int BUS_DE = 255;
static constexpr int LOW = 0, OUTPUT = 1;
static constexpr int HAL_REGION_FILAMENT = 0;
static constexpr int CUSTOM_SERIAL_LENGTH = 12;
static char CUSTOM_SERIAL[CUSTOM_SERIAL_LENGTH + 1] = {};
static int AMS_VERSION[4] = {90, 0, 0, 0};
static std::deque<uint8_t> uart_rx;
static std::vector<std::vector<uint8_t>> uart_tx;
static std::vector<uint8_t> flash_data;
static bool fail_write = false, corrupt_read = false;
static int write_count = 0, read_count = 0;
static unsigned idle_notifications[16] = {};
static void (*during_tx)() = nullptr;
static void (*during_read)() = nullptr;
static void (*during_available)() = nullptr;
static void (*during_log)(char) = nullptr;
struct TestBusLog {
    char direction;
    int type;
    std::vector<uint8_t> data;
};
static std::vector<TestBusLog> bus_logs;

/** 获取模拟毫秒时间；可选步进用于验证生产代码的忙等间隔 */
static uint32_t millis() { const auto value = fake_ms; fake_ms += clock_step; return value; }
/** 返回测试配置的有效通道数 */
static int active_channel_count() { return AMS_COUNT * 4; }
/** 检查模拟 UART 是否有数据，可注入一次完整回调以复现空闲快照竞态 */
static bool hal_bus_available() {
    if (during_available) {
        const auto callback = during_available;
        during_available = nullptr;
        callback();
    }
    return !uart_rx.empty();
}
/** 读取模拟 UART 字节，可注入读取期间的空闲检查 */
static int hal_bus_read() {
    if (during_read) during_read();
    if (uart_rx.empty()) return -1;
    const auto value = uart_rx.front(); uart_rx.pop_front(); return value;
}
/**
 * 捕获完整同步回复
 * @param data 发送缓冲
 * @param length 帧长
 * 先执行在途回调再读取缓冲，以检查发送完成之前内容没有被覆盖
 */
static void hal_bus_write(const uint8_t *data, uint16_t length) {
    if (during_tx) during_tx();
    uart_tx.emplace_back(data, data + length);
}
/** 主机测试不初始化硬件 */
static void hal_bus_begin(uint32_t) {}
#define BUS_IRQ_SETUP() do {} while (0)
/** 返回稳定的模拟设备 ID */
static uint64_t hal_get_device_id() { return 0x123456789abcdef0ull; }
/** 主机测试只观察协议，不操作 GPIO */
static void digitalWrite(int, int) {}
/** 主机测试只观察协议，不配置 GPIO */
static void pinMode(int, int) {}
/**
 * 记录空闲通知
 * @param channel 全局通道号
 * 验证内部停车不会意外解除打印机阻断
 */
static void BMCU_printer_idle(int channel) { assert(channel >= 0 && channel < 16); ++idle_notifications[channel]; }
/** 默认传感器离线，验证现有温湿度回退值 */
static bool env_sensor_get_aht20(float *, float *) { return false; }
/** 模拟持久化写入，可注入失败 */
static bool hal_storage_write(int, const void *data, uint32_t length) {
    ++write_count;
    if (fail_write) return false;
    const auto *bytes = static_cast<const uint8_t *>(data);
    flash_data.assign(bytes, bytes + length);
    return true;
}
/** 模拟回读，可注入内容损坏，确保真实 CRC 和整块比较能拒绝错误记录 */
static void hal_storage_read(int, void *out, uint32_t length) {
    ++read_count;
    memset(out, 0xff, length);
    if (!flash_data.empty()) memcpy(out, flash_data.data(), std::min<size_t>(length, flash_data.size()));
    if (corrupt_read && length) static_cast<uint8_t *>(out)[0] ^= 1u;
}
#define LOG_MAIN(...) do {} while (0)
#define LOG_BUS(...) do {} while (0)
static constexpr int LOG_CH_BUS = 1;
/**
 * 捕获总线日志和关联报文类型
 * @param channel 日志通道
 * @param direction 收发方向
 * @param type 报文类型
 * @param data 日志数据
 * @param length 数据长度
 * 在读取数据前注入回调，验证日志期间的缓冲所有权和重入保护
 */
static void log_bus_hex_typed(int channel, char direction, int type, const uint8_t *data, uint16_t length) {
    (void)channel;
    if (during_log) during_log(direction);
    bus_logs.push_back({direction, type, std::vector<uint8_t>(data, data + length)});
}
