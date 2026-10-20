#pragma once
#include <cstdint>
#include <cstddef>

// ============================================================
// 日志总线：两路固定大小的环形缓冲，Web 页面按游标增量拉取
//  - 写入永不阻塞，可在 UART 中断 / 事件回调里安全调用
//  - 单写多读，无锁（读侧对撕裂不敏感，缓冲区绕回自动跳转）
// 缓冲区大小见 log_bus.cpp
// ============================================================

enum {
    LOG_CH_MAIN = 0,   // 主板 / 系统日志
    LOG_CH_BUS  = 1,   // BambuBus (RS485) 原始报文
    LOG_CH_COUNT
};

void log_bus_init();
void log_bus_write(int ch, const char *data, size_t len);
void log_bus_printf(int ch, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

// 十六进制报文转储：每行最多 24 字节，tag 取 'R'(接收) / 'T'(发送)
// type 是 BambuBus 报文类型编号；-1 表示没有可关联的完整报文类型。
// 每一行都会携带 type，网页可以据此把 RX/TX 和类型说明一起过滤。
void log_bus_hex_typed(int ch, char tag, int type, const uint8_t *data, uint16_t len);
void log_bus_hex(int ch, char tag, const uint8_t *data, uint16_t len);

// 清空某一路日志（游标同步推进，客户端不会重复读到旧数据）
void log_bus_clear(int ch);

// 当前写游标（可用于清空后同步客户端序号）
uint32_t log_bus_total(int ch);

// 增量读取：*seq 传入上次拿到的游标、传出新游标；返回写入 out 的字节数
// out 会被补 '\0'，需要 max_len 字节空间
size_t log_bus_read(int ch, uint32_t *seq, char *out, size_t max_len);

// 主板日志：进环形缓冲，同时输出到串口控制台
#define LOG_MAIN(...) log_bus_printf(LOG_CH_MAIN, __VA_ARGS__)
// BambuBus 日志：只进环形缓冲，不占串口带宽
#define LOG_BUS(...)  log_bus_printf(LOG_CH_BUS, __VA_ARGS__)
