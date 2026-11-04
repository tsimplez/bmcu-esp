// 仅替换 ADC、GPIO 与时钟；采样调度、统计、缓存及配置检测均使用生产实现。
#include <limits>
using std::min;
using std::max;
static constexpr int PIN_NONE = 255;
static constexpr uint8_t INPUT = 0;
static constexpr int HAL_HALL_ADC_CAPACITY = 5;
IoConfig IO_CONFIG{};
int AMS_COUNT = 1;
static uint32_t fake_ms = 0;
static uint32_t epoch_seed = 0xABC00000;
static unsigned adc_reads = 0;
static unsigned pin_reads[5] = {};
static unsigned prepares[5] = {};
static bool adc_available[5] = {true, true, true, true, true};
static bool fail_read = false;
static bool ramp_values = false;
static int forced_value = -1;
struct ObservedSample { uint32_t time; int pin; uint16_t value; };
static std::vector<ObservedSample> observed_samples;
static std::vector<int> input_pins;

struct HallTestServer {
    std::map<String, String> params;
    std::map<String, String> headers;
    int code = 0;
    String body;
    /**
     * 查询请求是否包含参数
     * @param key 参数名
     * @return 是否存在该参数
     */
    bool hasArg(const String &key) { return params.count(key) != 0; }
    /**
     * 读取请求参数
     * @param key 参数名
     * @return 参数值，不存在时为空
     */
    String arg(const String &key) { return params[key]; }
    /**
     * 记录 HTTP 响应
     * @param status 响应状态码
     * @param type 响应类型
     * @param value 完整响应正文
     */
    void send(int status, const char *type, const String &value) {
        assert(!strcmp(type, "application/json"));
        code = status;
        body = value;
    }
    /**
     * 记录缓存等响应头
     * @param name 响应头名
     * @param value 响应头值
     */
    void sendHeader(const char *name, const char *value) { headers[name] = value; }
} server;

/**
 * 返回测试当前启用通道数
 * @return AMS 数量乘以四
 */
int active_channel_count() { return AMS_COUNT * 4; }

/**
 * 返回可控毫秒时钟
 * @return 模拟设备时钟，允许无符号回绕
 */
uint32_t millis() { return fake_ms; }

/**
 * 模拟 C3 ADC1 的五个候选引脚
 * @param pin GPIO 编号
 * @return 是否有可用 ADC 通道
 */
bool hal_adc_pin_available(int pin) { return pin >= 0 && pin < 5; }

/**
 * 提供采集模块需要的 ADC 引脚可用性边界
 * @param pin 主控 GPIO 编号
 * @param allow_none 是否接受未配置标记
 * @param bus 未使用的候选总线接线
 * @return true 表示本测试允许该模拟输入
 * 完整接线冲突由项目设置套件使用真实 io_config.cpp 验证
 */
bool io_config_hall_pin_available(int pin, bool allow_none, const IoBusConfig *bus) {
    (void)bus;
    return (allow_none && pin == PIN_NONE) || hal_adc_pin_available(pin);
}

/**
 * 模拟 ADC 初始化并允许指定输入失败
 * @param pin GPIO 编号
 * @return 初始化是否成功
 */
bool hal_adc_prepare(int pin) {
    assert(hal_adc_pin_available(pin));
    ++prepares[pin];
    return adc_available[pin];
}

/**
 * 读取可预测模拟输入并记录生产模块真正取得的每个样本
 * @param pin GPIO 编号
 * @param raw 输出 ADC 值
 * @return false 表示本次硬件采集失败
 */
bool hal_adc_read(int pin, uint16_t &raw) {
    assert(hal_adc_pin_available(pin));
    ++adc_reads;
    const unsigned index = pin_reads[pin]++;
    if (fail_read || !adc_available[pin]) return false;
    raw = uint16_t(forced_value >= 0 ? forced_value :
        ramp_values ? 100 + pin * 500 + (index % 4) * 100 : 1000 + pin * 100);
    observed_samples.push_back({fake_ms, pin, raw});
    return true;
}

/**
 * 返回不同的启动标识以验证客户端不会拼接重启前历史
 * @return 本次模拟启动标识
 */
uint32_t hal_hall_epoch() { return ++epoch_seed; }

/**
 * 提供引脚输入配置边界，不访问实体设备
 * @param pin GPIO 编号
 * @param mode 预期为输入模式
 */
void pinMode_PCF8575(uint8_t pin, uint8_t mode) {
    assert(pin < 5 && mode == INPUT);
    input_pins.push_back(pin);
}
