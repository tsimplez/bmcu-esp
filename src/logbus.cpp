#include "logbus.h"
#include "platform.h"

#include <stdio.h>
#include <string.h>
#include <stdarg.h>

// ============================================================
// 环形缓冲实现
// ============================================================

#if defined(ARDUINO_ARCH_ESP32)

#define LOG_SIZE_MAIN 4096
#define LOG_SIZE_BUS  65536
static portMUX_TYPE log_mux = portMUX_INITIALIZER_UNLOCKED;
#define LOG_LOCK()   portENTER_CRITICAL(&log_mux)
#define LOG_UNLOCK() portEXIT_CRITICAL(&log_mux)

#else

#define LOG_SIZE_MAIN 2048
#define LOG_SIZE_BUS  8192
#include <hardware/sync.h>
#define LOG_LOCK()   uint32_t _log_irq = save_and_disable_interrupts()
#define LOG_UNLOCK() restore_interrupts(_log_irq)

#endif

namespace {
    template <uint32_t N>
    struct LogRing {
        char buf[N];
        volatile uint32_t total;  // 累计写入字节数（单调递增，即读游标）
        volatile uint32_t floor;  // 清空后允许读到的最小游标
    };
}

static LogRing<LOG_SIZE_MAIN> ring_main;
static LogRing<LOG_SIZE_BUS>  ring_bus;

namespace {
    struct RingRef {
        char *buf;
        uint32_t cap;
        volatile uint32_t *total;
        volatile uint32_t *floor;
    };
}

static bool ring_ref(const int ch, RingRef &r) {
    if (ch == LOG_CH_MAIN) {
        r = {.buf = ring_main.buf, .cap = LOG_SIZE_MAIN, .total = &ring_main.total, .floor = &ring_main.floor};
        return true;
    }
    if (ch == LOG_CH_BUS) {
        r = {.buf = ring_bus.buf, .cap = LOG_SIZE_BUS, .total = &ring_bus.total, .floor = &ring_bus.floor};
        return true;
    }
    return false;
}

void logbus_init() {
    memset(ring_main.buf, 0, sizeof(ring_main.buf));
    memset(ring_bus.buf, 0, sizeof(ring_bus.buf));
    ring_main.total = ring_main.floor = 0;
    ring_bus.total = ring_bus.floor = 0;
}

void logbus_write(const int ch, const char *data, const size_t len) {
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

void logbus_clear(const int ch) {
    RingRef r{};
    if (!ring_ref(ch, r)) return;

    LOG_LOCK();
    memset(r.buf, 0, r.cap);
    *r.floor = *r.total;
    LOG_UNLOCK();
}

uint32_t logbus_total(const int ch) {
    RingRef r{};
    if (!ring_ref(ch, r)) return 0;
    uint32_t t;
    LOG_LOCK();
    t = *r.total;
    LOG_UNLOCK();
    return t;
}

size_t logbus_read(const int ch, uint32_t *seq, char *out, size_t maxlen) {
    if (!seq || !out || maxlen == 0) return 0;
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
        static constexpr char mark[] = "[... 较早日志已滚动丢弃 ...]\n";
        size_t ml = sizeof(mark) - 1;
        if (ml > maxlen - 1) ml = maxlen - 1;
        memcpy(out, mark, ml);
        n = ml;
        s = oldest;
    }

    const size_t room = maxlen - 1 - n;
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

static void console_echo(const char *s, size_t len) {
#if defined(ARDUINO_ARCH_ESP32)
    Serial.write(reinterpret_cast<const uint8_t *>(s), len);
#else
    fwrite(s, 1, len, stdout);
#endif
}

void logbus_printf(int ch, const char *fmt, ...) {
    char body[224];
    char line[256];

    va_list ap;
    va_start(ap, fmt);
    vsnprintf(body, sizeof(body), fmt, ap);
    va_end(ap);

    const int n = snprintf(line, sizeof(line), "[%lu] %s\n", static_cast<unsigned long>(millis()), body);
    if (n <= 0) return;
    const size_t len = static_cast<size_t>(n) < sizeof(line) ? static_cast<size_t>(n) : sizeof(line) - 1;

    logbus_write(ch, line, len);
    if (ch == LOG_CH_MAIN) console_echo(line, len);
}

// 手写十进制转换：hex 转储可能在中断 / 事件回调里调用，
// 这里刻意不用 snprintf，避免在 ISR 上下文里碰到不可重入的 newlib 分配器
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

static char *fmt_i32(char *p, int32_t v) {
    if (v < 0) {
        *p++ = '-';
        return fmt_u32(p, static_cast<uint32_t>(-static_cast<int64_t>(v)));
    }
    return fmt_u32(p, static_cast<uint32_t>(v));
}

void logbus_hex_typed(const int ch, const char tag, const int type, const uint8_t *data, const uint16_t len) {
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
        logbus_write(ch, line, static_cast<size_t>(p - line));
    }
}

void logbus_hex(const int ch, const char tag, const uint8_t *data, const uint16_t len) {
    logbus_hex_typed(ch, tag, -1, data, len);
}
