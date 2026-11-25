#include "json_utils.h"

#if defined(ARDUINO_ARCH_ESP32)

namespace Json {

/**
 * 严格解析完整 JSON 文本并以 RAII 管理解析树
 * @param text JSON 文本
 * @param length 文本字节数，不含结尾 NUL
 * @return 解析成功时持有 JSON 根节点，失败时为空
 */
Document parse(const char *text, const size_t length) {
    if (!text || length == SIZE_MAX) return {nullptr, &cJSON_Delete};
    return {cJSON_ParseWithLengthOpts(text, length + 1, nullptr, true), &cJSON_Delete};
}

/**
 * 按大小写敏感方式读取 JSON 对象字段
 * @param object JSON 对象
 * @param key 字段名
 * @return 字段节点，不存在或输入不是对象时为空
 */
const cJSON *item(const cJSON *object, const char *key) {
    return cJSON_IsObject(object) && key ? cJSON_GetObjectItemCaseSensitive(object, key) : nullptr;
}

/**
 * 读取 JSON 字符串字段
 * @param object JSON 对象
 * @param key 字段名
 * @param value 接收字符串的输出变量
 * @return 字段存在且类型为字符串时返回 true
 */
bool read_string(const cJSON *object, const char *key, String &value) {
    value = "";
    const cJSON *field = item(object, key);
    if (!cJSON_IsString(field) || !field->valuestring) return false;
    value = field->valuestring;
    return true;
}

/**
 * 读取范围内的无符号整数 JSON 字段
 * @param object JSON 对象
 * @param key 字段名
 * @param value 接收整数的输出变量
 * @param maximum 可接受的最大值
 * @return 字段为非负整数且不超过上限时返回 true
 */
bool read_uint(const cJSON *object, const char *key, uint32_t &value, const uint32_t maximum) {
    const cJSON *field = item(object, key);
    if (!cJSON_IsNumber(field) || !(field->valuedouble >= 0 && field->valuedouble <= maximum) ||
        field->valuedouble != static_cast<uint32_t>(field->valuedouble)) return false;
    value = static_cast<uint32_t>(field->valuedouble);
    return true;
}

/**
 * 读取带范围约束的 JSON 数字字段
 * @param object JSON 对象
 * @param key 字段名
 * @param value 接收数字的输出变量
 * @param minimum 可接受的最小值
 * @param maximum 可接受的最大值
 * @param allow_fractional 是否接受小数
 * @return 字段为指定范围内的数字时返回 true
 */
bool read_number(const cJSON *object, const char *key, double &value, const double minimum, const double maximum,
                 const bool allow_fractional) {
    const cJSON *field = item(object, key);
    if (!cJSON_IsNumber(field) || !(field->valuedouble >= minimum && field->valuedouble <= maximum) ||
        (!allow_fractional && field->valuedouble != static_cast<int32_t>(field->valuedouble))) return false;
    value = field->valuedouble;
    return true;
}

/**
 * 读取并转义 JSON 字符串内容，不含外围引号
 * @param value 待转义的 UTF-8 字符串
 * @return 可直接放入 JSON 字符串字面量的内容
 */
String escape(const char *value) {
    String result;
    for (auto p = reinterpret_cast<const unsigned char *>(value ? value : ""); *p; ++p) {
        switch (*p) {
            case '"': result += "\\\""; break;
            case '\\': result += "\\\\"; break;
            case '\b': result += "\\b"; break;
            case '\f': result += "\\f"; break;
            case '\n': result += "\\n"; break;
            case '\r': result += "\\r"; break;
            case '\t': result += "\\t"; break;
            default:
                if (*p < 0x20) {
                    static constexpr char hex[] = "0123456789abcdef";
                    result += "\\u00";
                    result += hex[*p >> 4];
                    result += hex[*p & 0x0f];
                } else result += static_cast<char>(*p);
        }
    }
    return result;
}

/**
 * 生成包含 ok 和 msg 字段的标准 JSON 响应
 * @param ok 操作是否成功
 * @param message 响应消息
 * @return 完整 JSON 对象
 */
String message(const bool ok, const char *message) {
    String result;
    if (!result.reserve((message ? strlen(message) : 0) + 24)) return String();
    result = "{\"ok\":";
    result += ok ? "true" : "false";
    result += R"(,"msg":")";
    result += escape(message);
    result += "\"}";
    return result;
}

}

#endif
