#include "log_bus.h"
#include "platform.h"

#include <cstdio>
#include <cstring>
#include <cstdarg>

// ============================================================
// 环形缓冲实现
// ============================================================

#if defined(ARDUINO_ARCH_ESP32)

static portMUX_TYPE log_mux = portMUX_INITIALIZER_UNLOCKED;
#define LOG_LOCK()   portENTER_CRITICAL(&log_mux)
#define LOG_UNLOCK() portEXIT_CRITICAL(&log_mux)

#else

#include <hardware/sync.h>
#define LOG_LOCK()   uint32_t _log_irq = save_and_disable_interrupts()
#define LOG_UNLOCK() restore_interrupts(_log_irq)

#endif

namespace {
    template <uint32_t N>
    struct LogRing {
        // 固定容量环形缓冲区，写入到达末尾后从头覆盖最旧数据
        char buf[N];
        volatile uint32_t total;  // 累计写入字节数（单调递增，即读游标）
        volatile uint32_t floor;  // 清空后允许读到的最小游标
    };
}

// 主板日志环形缓冲：记录业务日志，容量由平台内存预算决定
static LogRing<HAL_LOG_MAIN_CAPACITY> ring_main;
// 总线日志环形缓冲：记录 BambuBus 原始收发报文，容量由平台内存预算决定
static LogRing<HAL_LOG_BUS_CAPACITY>  ring_bus;

namespace {
    struct RingRef {
        // 指向目标日志环形缓冲区的数据区
        char *buf;
        // 环形缓冲区容量（字节）
        uint32_t cap;
        // 指向该通道单调递增的累计写游标
        volatile uint32_t *total;
        // 指向清空后允许读取的最小游标
        volatile uint32_t *floor;
    };
}

/**
 * 取得指定日志通道的环形缓冲引用
 * @param ch 日志通道
 * @param r 输出的环形缓冲引用
 * @return 通道是否有效
 * 把主日志或总线日志的缓冲区、平台容量和游标指针封装到 RingRef 中
 */
static bool ring_ref(const int ch, RingRef &r) {
    if (ch == LOG_CH_MAIN) {
        r = {.buf = ring_main.buf, .cap = HAL_LOG_MAIN_CAPACITY, .total = &ring_main.total, .floor = &ring_main.floor};
        return true;
    }
    if (ch == LOG_CH_BUS) {
        r = {.buf = ring_bus.buf, .cap = HAL_LOG_BUS_CAPACITY, .total = &ring_bus.total, .floor = &ring_bus.floor};
        return true;
    }
    return false;
}

/**
 * 初始化日志环形缓冲
 * 清空两路日志缓冲区并把累计写游标和清空下限归零
 */
void log_bus_init() {
    memset(ring_main.buf, 0, sizeof(ring_main.buf));
    memset(ring_bus.buf, 0, sizeof(ring_bus.buf));
    ring_main.total = ring_main.floor = 0;
    ring_bus.total = ring_bus.floor = 0;
}

/**
 * 向日志环形缓冲追加数据
 * @param ch 日志通道
 * @param data 待写入数据
 * @param len 数据长度
 * 在临界区内循环写入并在末尾覆盖旧数据，写入过程不会阻塞
 */
void log_bus_write(const int ch, const char *data, const size_t len) {
    RingRef r{};
    if (!data || !ring_ref(ch, r)) return;

    LOG_LOCK();
    uint32_t idx = *r.total % r.cap;
    uint32_t total = *r.total;
    for (size_t i = 0; i < len; i++) {
        r.buf[idx++] = data[i];
        if (idx >= r.cap) idx = 0;
        total++;
    }
    *r.total = total;
    LOG_UNLOCK();
}

/**
 * 清空指定通道日志
 * @param ch 日志通道
 * 清空缓冲区并把 floor 推进到当前写游标，客户端不会重复读到旧内容
 */
void log_bus_clear(const int ch) {
    RingRef r{};
    if (!ring_ref(ch, r)) return;

    LOG_LOCK();
    memset(r.buf, 0, r.cap);
    *r.floor = *r.total;
    LOG_UNLOCK();
}

/**
 * 读取日志累计写入字节数
 * @param ch 日志通道
 * @return 累计写游标
 * 返回单调递增的写游标，可用于清空后同步客户端序号
 */
uint32_t log_bus_total(const int ch) {
    RingRef r{};
    if (!ring_ref(ch, r)) return 0;
    uint32_t t;
    LOG_LOCK();
    t = *r.total;
    LOG_UNLOCK();
    return t;
}

/**
 * 按游标增量读取日志
 * @param ch 日志通道
 * @param seq 输入上次游标，输出新游标
 * @param out 输出缓冲区
 * @param max_len 输出缓冲区大小
 * @param dropped 可选输出：本次读取前被覆盖或清空的字节数
 * @return 实际写入 out 的字节数
 * 从游标位置读取未消费日志；若中间数据已被覆盖，会先插入丢弃提示
 */
size_t log_bus_read(const int ch, uint32_t *seq, char *out, size_t max_len, uint32_t *dropped) {
    if (dropped) *dropped = 0;
    if (!seq || !out || max_len == 0) return 0;
    out[0] = 0;

    RingRef r{};
    if (!ring_ref(ch, r)) return 0;

    size_t n = 0;

    LOG_LOCK();
    const uint32_t total = *r.total;
    uint32_t oldest = total > r.cap ? total - r.cap : 0;
    if (*r.floor > oldest) oldest = *r.floor;

    uint32_t s = *seq;
    if (s > total) s = total;  // 日志被清空过，游标回退

    if (s < oldest) {  // 用户太久没来取，中间已被覆盖
        if (dropped) *dropped = oldest - s;
        // 游标落后超过缓冲区容量时，返回给网页的提示文本
        static constexpr char mark[] = "[... 较早日志已滚动丢弃 ...]\n";
        size_t ml = sizeof(mark) - 1;
        if (ml > max_len - 1) ml = max_len - 1;
        memcpy(out, mark, ml);
        n = ml;
        s = oldest;
    }

    const size_t room = max_len - 1 - n;
    uint32_t avail = total > s ? total - s : 0;
    if (avail > room) avail = static_cast<uint32_t>(room);

    uint32_t idx = s % r.cap;
    for (uint32_t i = 0; i < avail; i++) {
        out[n++] = r.buf[idx++];
        if (idx >= r.cap) idx = 0;
    }
    *seq = s + avail;
    LOG_UNLOCK();

    out[n] = 0;
    return n;
}

// ============================================================
// 文本输出
// ============================================================

/**
 * 把日志同步输出到控制台
 * @param s 文本内容
 * @param len 文本长度
 * ESP32 使用 Serial.write()，RP2040 使用 stdout 输出日志
 */
static void console_echo(const char *s, size_t len) {
#if defined(ARDUINO_ARCH_ESP32)
    Serial.write(reinterpret_cast<const uint8_t *>(s), len);
#else
    fwrite(s, 1, len, stdout);
#endif
}

/**
 * 格式化写入日志
 * @param ch 日志通道
 * @param fmt printf 格式串
 * @param ... 可变参数
 * 添加毫秒时间戳后写入环形缓冲，主板日志同时回显到控制台
 */
void log_bus_printf(const int ch, const char *fmt, ...) {
    char body[224];
    char line[256];

    va_list ap;
    va_start(ap, fmt);
    vsnprintf(body, sizeof(body), fmt, ap);
    va_end(ap);

    const int n = snprintf(line, sizeof(line), "[%lu] %s\n", static_cast<unsigned long>(millis()), body);
    if (n <= 0) return;
    const size_t len = static_cast<size_t>(n) < sizeof(line) ? static_cast<size_t>(n) : sizeof(line) - 1;

    log_bus_write(ch, line, len);
    if (ch == LOG_CH_MAIN) console_echo(line, len);
}

// 手写十进制转换：hex 转储可能在中断 / 事件回调里调用，
// 这里刻意不用 snprintf，避免在 ISR 上下文里碰到不可重入的 newlib 分配器
/**
 * 把无符号整数格式化为十进制文本
 * @param p 输出位置
 * @param v 待转换数值
 * @return 写入结束后的新位置
 * 手写十进制转换，避免在中断上下文中调用可能不可重入的 snprintf
 */
static char *fmt_u32(char *p, uint32_t v) {
    char tmp[11];
    int n = 0;
    if (v == 0) tmp[n++] = '0';
    while (v) {
        tmp[n++] = static_cast<char>('0' + (v % 10));
        v /= 10;
    }
    while (n) *p++ = tmp[--n];
    return p;
}

/**
 * 把有符号整数格式化为十进制文本
 * @param p 输出位置
 * @param v 待转换数值
 * @return 写入结束后的新位置
 * 先处理负号，再复用无符号转换逻辑
 */
static char *fmt_i32(char *p, const int32_t v) {
    if (v < 0) {
        *p++ = '-';
        return fmt_u32(p, static_cast<uint32_t>(-static_cast<int64_t>(v)));
    }
    return fmt_u32(p, static_cast<uint32_t>(v));
}

/**
 * 带报文类型的十六进制日志转储
 * @param ch 日志通道
 * @param tag R 表示接收，T 表示发送
 * @param type BambuBus 报文类型编号，-1 表示未知
 * @param data 待转储数据
 * @param len 数据长度
 * 每行最多 24 字节，行首携带时间戳、类型和收发标记，便于网页按类型过滤
 */
void log_bus_hex_typed(const int ch, const char tag, const int type, const uint8_t *data, const uint16_t len) {
    // 十六进制字符表，避免每次转换都调用格式化函数
    static constexpr char hexd[] = "0123456789ABCDEF";
    char line[128];
    uint16_t i = 0;

    while (i < len) {
        char *p = line;
        *p++ = '[';
        p = fmt_u32(p, millis());
        *p++ = ']';
        *p++ = ' ';
        *p++ = 't';
        *p++ = 'y';
        *p++ = 'p';
        *p++ = 'e';
        *p++ = '=';
        p = fmt_i32(p, type);
        *p++ = ' ';
        *p++ = tag;
        *p++ = 'X';
        *p++ = ' ';

        uint16_t k = 0;
        while (i < len && k < 24) {
            const uint8_t b = data[i++];
            *p++ = hexd[b >> 4];
            *p++ = hexd[b & 0x0F];
            *p++ = ' ';
            k++;
        }
        if (i < len) *p++ = '>';
        *p++ = '\n';
        log_bus_write(ch, line, static_cast<size_t>(p - line));
    }
}

/**
 * 十六进制日志转储（未知类型）
 * @param ch 日志通道
 * @param tag R 表示接收，T 表示发送
 * @param data 待转储数据
 * @param len 数据长度
 * 以 type=-1 调用 log_bus_hex_typed()，用于没有完整报文类型的原始字节
 */
void log_bus_hex(const int ch, const char tag, const uint8_t *data, const uint16_t len) {
    log_bus_hex_typed(ch, tag, -1, data, len);
}
