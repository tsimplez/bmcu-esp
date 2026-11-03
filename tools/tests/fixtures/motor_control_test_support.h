// 电机调度使用真实 BMCU 代码；这里只替换硬件输出、采样和 HTTP 传输。
#include <map>

static constexpr int PIN_NONE = 255, INPUT = 0, INPUT_PULLUP = 2;
static constexpr uint32_t MAX_PULSE_MS = 30000;
static int PULLBACK_TIMEOUT[IO_CONFIG_MAX_CHANNELS];
static int MANUAL_PULSE_MS = 300, MOTOR_LIMIT = 1;
static int AUTO_LOAD_MS = 1000;
static int FEED_STOP_DELAY_MS = 1000;
static IoConfig IO_CONFIG;
static bool expander_online = true;
static unsigned motor_outputs = 0;
static int switch_level = 1;
static std::vector<int> input_reads;
static std::map<int, int> pin_modes;
static std::map<int, int> input_levels;
#define IS_PCF8575_VIRTUAL_PIN(pin) ((pin) >= 50 && (pin) < 128)

class Motor {
public:
    bool stopped = true;
    /**
     * 模拟绑定引脚
     * @param forward_pin 进料引脚
     * @param backward_pin 退料引脚
     */
    void setPins(int forward_pin, int backward_pin) { stop(); }
    /** 记录进料输出 */
    void forward() { stopped = false; ++motor_outputs; }
    /** 记录退料输出 */
    void backward() { stopped = false; ++motor_outputs; }
    /** 记录停车输出 */
    void stop() { stopped = true; ++motor_outputs; }
    /**
     * 查询模拟电机输出
     * @return true 表示电机已停止
     */
    bool getStopState() const { return stopped; }
};

/**
 * 返回测试扩展器状态
 * @param pin 扩展引脚
 * @return 当前模拟在线状态
 */
static bool PCF8575_pin_is_online(int pin) { return expander_online; }
/**
 * 模拟引脚配置边界
 * @param pin 引脚编号
 * @param mode 输入输出模式
 */
static void pinMode_PCF8575(int pin, int mode) { pin_modes[pin] = mode; }
/**
 * 模拟总微动和指定通道的输入，未指定的通道微动保持未触发
 * @param pin 引脚编号
 * @return 指定测试电平，未指定时为高电平
 */
static int digitalRead_PCF8575(int pin) {
    input_reads.push_back(pin);
    return pin == IO_CONFIG.switch_pin ? switch_level : input_levels.count(pin) ? input_levels[pin] : 1;
}
/** 模拟扩展器轮询，不访问 I2C */
static void PCF8575_poll() {}
/**
 * 固定模拟采样，避免与距离算法耦合
 * @return 本轮模拟距离增量
 */
static float AS5600_get_distance_E() { return 0.0f; }
static void switchInterrupt();

struct TestArg {
    std::string value;
    /**
     * 提供请求参数原文供真实处理器校验
     * @return 未经数值转换的参数字符串
     */
    operator String() const { return value; }
    /**
     * 将请求参数转换为整数
     * @return 参数值；测试仅提交合法数字
     */
    int toInt() const { return std::atoi(value.c_str()); }
};
struct TestServer {
    std::map<std::string, std::string> args;
    int status = 0;
    std::string body;
    /**
     * 检查请求字段
     * @param key 字段名
     * @return true 表示字段存在
     */
    bool hasArg(const std::string &key) const { return args.count(key) != 0; }
    /**
     * 读取请求字段
     * @param key 字段名
     * @return 模拟 HTTP 参数
     */
    TestArg arg(const std::string &key) const { return {args.at(key)}; }
    /**
     * 捕获 HTTP 响应
     * @param code 状态码
     * @param type 内容类型
     * @param response 响应正文
     */
    void send(int code, const char *type, const char *response) { status = code; body = response; }
};
static TestServer server;
/**
 * 提供已知数字参数的范围检查边界
 * @param key 字段名
 * @param low 最小值
 * @param high 最大值
 * @return true 表示缺省参数或范围内数值；本组不测试数字解析
 */
static bool valid_number(const String &key, int low, int high) {
    if (!server.hasArg(key)) return true;
    const int value = server.arg(key).toInt();
    return value >= low && value <= high;
}
