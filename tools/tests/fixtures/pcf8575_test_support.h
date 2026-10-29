// 只模拟芯片边界和 Arduino GPIO，映射、读写、重试及通道判断使用生产函数。
#define HIGH 1
#define LOW 0
#define INPUT 0
#define OUTPUT 1
#define INPUT_PULLUP 2
#define PIN_NONE 255
#define NUM_DIGITAL_PINS 30
#define PCF8575_OK 0
#define IO_CONFIG_MAX_CHANNELS 16
#define CHANNEL_MODE_AUTO 2
#define CHANNEL_MODE_ONLINE 1

struct Write { uint8_t address; uint16_t value; };
static std::vector<Write> chip_writes;
static bool connected[4] = {}, fail_write[4] = {};
static uint16_t input_mask[4] = {0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF};
static uint16_t output_latch[4] = {};
static int attempts[4] = {}, chip_reads[4] = {}, gpio_calls = 0;
static uint32_t now_ms = 0;
static bool bus_ready = false;

/** 返回测试控制的毫秒计时器 */
static uint32_t millis() { return now_ms; }
/** 模拟共享总线是否完成初始化 */
static bool i2c_bus_ready() { return bus_ready; }
/** 记录真实 GPIO 的模式设置 */
static void pinMode(uint8_t pin, uint8_t) { assert(pin < 30); ++gpio_calls; }
/** 记录真实 GPIO 的数字写入 */
static void digitalWrite(uint8_t pin, uint8_t) { assert(pin < 30); ++gpio_calls; }
/** 记录真实 GPIO 的输入读取 */
static int digitalRead(uint8_t pin) { assert(pin < 30); ++gpio_calls; return HIGH; }
/** 记录真实 GPIO 的模拟输出 */
static void analogWrite(uint8_t pin, uint8_t) { assert(pin < 30); ++gpio_calls; }

class PCF8575 {
    uint8_t address;
    int error = 0;
public:
    /** 记录生产代码选择的实际 I2C 地址 */
    explicit PCF8575(uint8_t addr) : address(addr) { assert(addr >= 0x20 && addr <= 0x23); }
    /** 模拟库的先探测再写入行为，包括写失败但 begin 返回成功的情形 */
    bool begin(uint16_t value = 0xFFFF) {
        assert(bus_ready);
        ++attempts[address - 0x20];
        if (!connected[address - 0x20]) return false;
        write16(value);
        return true;
    }
    /** 记录整个 16 位输出锁存值 */
    void write16(uint16_t value) {
        assert(bus_ready);
        chip_writes.push_back({address, value});
        error = fail_write[address - 0x20] ? 4 : 0;
        if (!error) output_latch[address - 0x20] = value;
    }
    /** 按外部拉低掩码模拟输入，只有锁存为高的位才能读取高电平 */
    uint16_t read16() {
        assert(bus_ready);
        ++chip_reads[address - 0x20];
        return output_latch[address - 0x20] & input_mask[address - 0x20];
    }
    /** 返回并清除错误，模拟实际库的 lastError 接口 */
    int lastError() { const int result = error; error = 0; return result; }
};

static struct { int motor[16][2]; int microswitch[16]; } IO_CONFIG;
static int CHANNEL_MODE[16] = {};
static bool filament_online[16] = {};
/** 提供测试中的有效通道数 */
static int active_channel_count() { return 16; }
/** 提供固定容量的通道范围判断 */
static bool valid_channel(int ch) { return ch >= 0 && ch < 16; }
/** 记录真实通道模式函数产生的有料状态 */
static void set_filament_online(int ams, int ch, bool online) { filament_online[ams * 4 + ch] = online; }
