#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <type_traits>
#include <vector>

#define LOG_LOCK() do {} while (0)
#define LOG_UNLOCK() do {} while (0)
#define LOG_PULL_MAX 16384
#define HEX 16
constexpr uint32_t HAL_LOG_MAIN_CAPACITY = 16384;
constexpr uint32_t HAL_LOG_BUS_CAPACITY = 32768;

class String {
    std::string value;
public:
    /** 创建空字符串。 */
    String() = default;
    /** 从测试文本创建 Arduino 字符串替身。 @param text 文本 */
    String(const char *text) : value(text) {}
    /** 从标准字符串创建替身。 @param text 文本 */
    String(std::string text) : value(std::move(text)) {}
    /** 将整数转换为指定进制。 @param number 整数 @param base 进制 */
    template <class T, typename = std::enable_if_t<std::is_integral_v<T>>>
    String(T number, int base = 10) {
        char buffer[32];
        snprintf(buffer, sizeof(buffer), base == 16 ? "%llx" : "%llu", static_cast<unsigned long long>(number));
        value = buffer;
    }
    /** 读取底层文本。 @return NUL 结尾的文本 */
    const char *c_str() const { return value.c_str(); }
    /** 解析整数参数。 @return 十进制整数 */
    long toInt() const { return strtol(value.c_str(), nullptr, 10); }
    /** 比较字符串。 @param other 目标字符串 @return 是否相等 */
    bool operator==(const String &other) const { return value == other.value; }
    /** 比较字符串。 @param other 目标字符串 @return 是否不同 */
    bool operator!=(const String &other) const { return value != other.value; }
    /** 拼接字符串。 @param other 后缀 @return 拼接结果 */
    String operator+(const String &other) const { return value + other.value; }
};

struct FakeServer {
    std::map<std::string, String> args, headers;
    std::string body, content_type;
    size_t content_length = 0;
    int status = 0;
    /** 清理上一次请求和响应。 */
    void reset() { args.clear(); headers.clear(); body.clear(); content_type.clear(); content_length = 0; status = 0; }
    /** 检查参数是否存在。 @param key 参数名 @return 是否存在 */
    bool hasArg(const char *key) const { return args.count(key); }
    /** 读取参数。 @param key 参数名 @return 参数文本或空串 */
    String arg(const char *key) const { auto it = args.find(key); return it == args.end() ? String("") : it->second; }
    /** 记录响应头。 @param key 名称 @param value 值 */
    void sendHeader(const char *key, const String &value) { headers[key] = value; }
    /** 记录响应长度。 @param length 字节数 */
    void setContentLength(size_t length) { content_length = length; }
    /** 记录普通响应。 @param code 状态码 @param type 类型 @param text 初始文本 */
    void send(int code, const char *type, const char *text) { status = code; content_type = type; body = text; }
    /** 按精确长度追加响应。 @param data 数据 @param length 字节数 */
    void sendContent(const char *data, size_t length) { body.append(data, length); }
};
static FakeServer server;
/** 提供固定运行时间。 @return 开机毫秒数 */
static uint32_t millis() { return 12345; }
/** 提供可重复的启动标识。 @return 伪随机测试整数 */
static uint32_t esp_random() { return 0x1234abcd; }
