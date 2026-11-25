#pragma once

#if defined(ARDUINO_ARCH_ESP32)

#include <Arduino.h>
#include <cJSON.h>
#include <memory>

namespace Json {

using Document = std::unique_ptr<cJSON, void (*)(cJSON *)>;

/**
 * 严格解析完整 JSON 文本并以 RAII 管理解析树
 * @param text JSON 文本
 * @param length 文本字节数，不含结尾 NUL
 * @return 解析成功时持有 JSON 根节点，失败时为空
 */
Document parse(const char *text, size_t length);

/**
 * 按大小写敏感方式读取 JSON 对象字段
 * @param object JSON 对象
 * @param key 字段名
 * @return 字段节点，不存在或输入不是对象时为空
 */
const cJSON *item(const cJSON *object, const char *key);

/**
 * 读取 JSON 字符串字段
 * @param object JSON 对象
 * @param key 字段名
 * @param value 接收字符串的输出变量
 * @return 字段存在且类型为字符串时返回 true
 */
bool read_string(const cJSON *object, const char *key, String &value);

/**
 * 读取范围内的无符号整数 JSON 字段
 * @param object JSON 对象
 * @param key 字段名
 * @param value 接收整数的输出变量
 * @param maximum 可接受的最大值
 * @return 字段为非负整数且不超过上限时返回 true
 */
bool read_uint(const cJSON *object, const char *key, uint32_t &value, uint32_t maximum = UINT32_MAX);

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
bool read_number(const cJSON *object, const char *key, double &value, double minimum, double maximum,
                 bool allow_fractional = false);

/**
 * 读取并转义 JSON 字符串内容，不含外围引号
 * @param value 待转义的 UTF-8 字符串
 * @return 可直接放入 JSON 字符串字面量的内容
 */
String escape(const char *value);

/**
 * 生成包含 ok 和 msg 字段的标准 JSON 响应
 * @param ok 操作是否成功
 * @param message 响应消息
 * @return 完整 JSON 对象
 */
String message(bool ok, const char *message);

}

#endif
