/**
 * BMCU.cpp - BMCU主控单元实现
 * 
 * 功能说明：
 * 1. 电机控制：实现电机的正转（进料）和反转（退料）控制
 * 2. 传感器模拟：模拟AS5600角度传感器数据
 * 3. 通信协议：与Bambu打印机进行通信
 * 4. 状态管理：管理耗材状态和运动状态
 */

#include "BMCU.h"
#include "Motor.h"
#include <limits>
#include <cmath>
#undef abs
#include <random>
#include "PCF8575_PinMap.h"
#include "settings_storage.h"
#include "logbus.h"

// 硬件参数定义
#define max_filament_num 16        // 最大耗材数量
#define max_ams_num 4              // 最大AMS数量
#define WHEEL_D 4.5                // 轮子直径
#define SERVO_DELAY 100            // 伺服电机延迟时间(ms)
#define PULL_BACK_METER 200        // 退料距离
#define LED_BLINK_INTERVAL 500     // LED闪烁间隔
#define SERVO_COUNT 4              // 伺服电机数量
#define AS5600_PI 3.14159265358979323846  // π值
#define MSW_DEBOUNCE_MS 30         // 微动开关防抖时间（毫秒）

// PCF8575地址
#define PCF8575_ADDR 0x20
PCF8575 pcf8575(PCF8575_ADDR);

// AS5600模拟变量
int16_t as5600_current_angle[4] = {0, 0, 0, 0};  // 当前角度值
int16_t as5600_last_angle[4] = {0, 0, 0, 0};     // 上一次角度值
uint32_t as5600_last_time[4] = {0, 0, 0, 0};     // 上一次时间戳
float as5600_speed[4] = {0.0f, 0.0f, 0.0f, 0.0f}; // 速度值

// 微动开关引脚
const int switchPin = PIN_SWITCH;   // 微动开关引脚

// 全局变量
static uint32_t last_toggle_time = 0;      // 上次切换时间
static bool if_as5600_init = false;        // AS5600初始化状态
static int last_num = -1;                  // 上次耗材编号
static unsigned long last_num_change_time = 0; // 上次耗材变化时间
static bool execute_motion = true;         // 是否执行运动
static bool last_execute_motion = false;   // 上次运动状态
static int last_cmd = BambuBus_package_ERROR;  // 上次命令
static int last_filament_num = 255;        // 上次耗材编号
static float last_meters[max_filament_num];    // 上次耗材长度
static float last_pullback_meters[max_filament_num]; // 上次退料长度
static _filament_motion_state_set last_action[max_filament_num]; // 上次动作
static _filament_motion_state_set act;     // 当前动作状态
static bool ams_enable = true;             // AMS使能
static float target = 0;                   // 目标值
static bool enable_as5600 = true;          // AS5600使能
static int32_t now_distance = 0;           // 当前距离
// 所有电机调用均在 Arduino loop 中串行执行（包括 Web 和总线处理），不在 ISR 中驱动。
// 同一入口完成检查、占位和 GPIO 操作，任何来源都不能绕过并发上限。
struct MotorRun {
    int direction = 0;
    MotorSource source = MOTOR_IDLE;
    uint32_t end = 0;
    bool timed = false;
};
static MotorRun motor_run[IO_CONFIG_MAX_CHANNELS];
static bool printer_blocked[IO_CONFIG_MAX_CHANNELS] = {};
// 打印机总线在超过超时窗口后，禁止新的打印机动作，直到收到合法新帧。
static bool printer_bus_fault = false;
namespace {
    struct MotorJob { int channel, direction; uint32_t duration; };
}
static MotorJob manual_queue[IO_CONFIG_MAX_CHANNELS];
static int manual_queued = 0;

static int queue_position(int ch) {
    for (int i = 0; i < manual_queued; i++) if (manual_queue[i].channel == ch) return i + 1;
    return 0;
}

static void remove_job(int index) {
    for (int i = index; i + 1 < manual_queued; i++) manual_queue[i] = manual_queue[i + 1];
    --manual_queued;
}

// 进料/退料引脚都配置好了才能驱动电机：切换 PCF8575 开关后会先清掉不可用的引脚，
// 「无」的通道必须拒绝启动，否则界面显示在进退料而实际没有任何输出。
bool BMCU_channel_motor_ready(int ch) {
    if (ch < 0 || ch >= IO_CONFIG_MAX_CHANNELS) return false;
    return IO_CONFIG.motor[ch][0] != PIN_NONE && IO_CONFIG.motor[ch][1] != PIN_NONE;
}

static void run_manual_queue() {
    while (manual_queued && BMCU_running_count() < MOTOR_LIMIT) {
        const auto job = manual_queue[0];
        if (job.channel >= active_channel_count()) { remove_job(0); continue; }
        // 该通道没有电机引脚：丢掉这次请求，别让它卡住后面的排队动作。
        if (!BMCU_channel_motor_ready(job.channel)) { remove_job(0); continue; }
        if (!motorCmd(job.channel, job.direction, MOTOR_MANUAL, job.duration)) break;
        remove_job(0);
    }
}

// 随机数生成器（用于模拟传感器数据）
std::mt19937 generator(12345);
static std::normal_distribution<> distribution(0.0f, 0.00083f);
static uint32_t as5600_distance_last_ms = 0;

// 微动开关状态
static int switchState = 0;                // 当前开关状态
static int lastSwitchState = 0;            // 上次开关状态

// 微动辅助进料状态（无料→有料边沿触发，限时进料）
static int8_t   msw_prev[IO_CONFIG_MAX_CHANNELS];                  // 已确认的微动状态：-1 未知 0 无料 1 有料
static int8_t   msw_raw[IO_CONFIG_MAX_CHANNELS];                   // 原始微动状态（防抖前）
static uint32_t msw_raw_time[IO_CONFIG_MAX_CHANNELS];              // 原始状态最后变化时刻

// 电机对象数组；具体引脚由设置加载后在 BMCU_init()/BMCU_apply_io_config() 绑定。
static Motor motors[IO_CONFIG_MAX_CHANNELS];

/**
 * @brief 初始化IO引脚
 * 功能：设置微动开关和电机引脚的模式
 */
void init_io_pins() {
    // 微动开关引脚：输入模式+内部上拉
    if (switchPin != PIN_NONE) {
        pinMode(switchPin, INPUT_PULLUP);
    }
    
    // 只初始化启用通道的微动输入。
    for (int i = 0; i < active_channel_count(); i++) {
        const int pin = IO_CONFIG.microswitch[i];
        if (pin == PIN_NONE) continue;  // 该平台未实现的通道
        // 统一走映射层：物理 GPIO 使用 INPUT_PULLUP，PCF8575 使用其输入模式。
        if (IS_PCF8575_VIRTUAL_PIN(pin)) PCF8575_pinMode(pin, INPUT);
        else pinMode_PCF8575(pin, INPUT_PULLUP);
    }

    // 初始化所有电机引脚
    for (int i = 0; i < active_channel_count(); i++) {
        // 使用PCF8575虚拟引脚处理函数
        pinMode_PCF8575(IO_CONFIG.motor[i][0], OUTPUT); // 正转（进料）引脚
        pinMode_PCF8575(IO_CONFIG.motor[i][1], OUTPUT); // 反转（退料）引脚
        digitalWrite_PCF8575(IO_CONFIG.motor[i][0], LOW);
        digitalWrite_PCF8575(IO_CONFIG.motor[i][1], LOW);
    }
}

void BMCU_apply_io_config() {
    static IoConfig applied_io = {};
    static int applied_channels = 0;
    const int channels = active_channel_count();
    bool changed[IO_CONFIG_MAX_CHANNELS] = {};
    // 先释放所有变更/停用通道，再绑定新引脚，避免引脚交换时旧通道拉低新输出。
    for (int i = 0; i < IO_CONFIG_MAX_CHANNELS; i++) {
        changed[i] = (i < applied_channels) != (i < channels) ||
            (i < channels && (applied_io.motor[i][0] != IO_CONFIG.motor[i][0] ||
                              applied_io.motor[i][1] != IO_CONFIG.motor[i][1] ||
                              applied_io.microswitch[i] != IO_CONFIG.microswitch[i]));
        if (!changed[i]) continue;
        BMCU_stop_motor(i);
        motors[i].setPins(PIN_NONE, PIN_NONE);
        if (i < applied_channels && applied_io.microswitch[i] != PIN_NONE)
            pinMode_PCF8575(applied_io.microswitch[i], INPUT);
        last_action[i] = waiting;
        printer_blocked[i] = false;
        msw_prev[i] = msw_raw[i] = -1;
        msw_raw_time[i] = 0;
    }
    for (int i = 0; i < channels; i++) {
        if (!changed[i]) continue;
        motors[i].setPins(IO_CONFIG.motor[i][0], IO_CONFIG.motor[i][1]);
        const int pin = IO_CONFIG.microswitch[i];
        if (pin != PIN_NONE)
            pinMode_PCF8575(pin, IS_PCF8575_VIRTUAL_PIN(pin) ? INPUT : INPUT_PULLUP);
    }
    applied_io = IO_CONFIG;
    applied_channels = channels;
}

/**
 * @brief 检查微动开关状态并更新自动模式下的通道状态
 * 功能：通道处于自动模式时，微动开关由「无料」变为「有料」会触发该通道电机
 *       限时进料（AUTO_LOAD_MS，可在 Web 配置）。到时或被打断即停止，撤料后
 *       再次插入可重新触发。
 */
// 该通道当前是否不适合辅助进料（模式、互锁或正被打印机/手动占用）
static bool autoload_blocked(int ch) {
    if (ch < 0 || ch >= active_channel_count()) return true;
    if (CHANNEL_MODE[ch] != CHANNEL_MODE_AUTO) return true;   // 仅自动模式触发
    if (IO_CONFIG.microswitch[ch] == PIN_NONE) return true; // 无真实微动输入
    if (motor_run[ch].source == MOTOR_MANUAL) return true;
    if (get_filament_motion(ch) != waiting) return true;       // 打印机正在占用该通道
    return false;
}

void check_microswitch_status() {
    const uint32_t now = millis();

    for (int i = 0; i < active_channel_count(); i++) {
        if (motor_run[i].source == MOTOR_AUTOLOAD && (autoload_blocked(i) || AUTO_LOAD_MS <= 0))
            motorCmd(i, 0);

        // 非自动模式、超出启用通道或无微动输入的通道不参与触发
        if (CHANNEL_MODE[i] != CHANNEL_MODE_AUTO) {
            msw_prev[i] = msw_raw[i] = -1;
            continue;
        }

        const int pin = IO_CONFIG.microswitch[i];
        if (pin == PIN_NONE) {  // 该平台未实现的通道
            msw_prev[i] = msw_raw[i] = -1;
            continue;
        }

        // sw 微动高低电平 默认拉低是在线
        const bool sw = IS_PCF8575_VIRTUAL_PIN(pin) ? PCF8575_digitalRead(pin) : digitalRead(pin);
        const bool online = !sw;
        set_filament_online(i / 4, i % 4, online);   // 通道状态立即生效，界面实时刷新

        // 无料→有料需要稳定一段时间才算有效插入，避免开关抖动反复触发
        const int8_t raw = online ? 1 : 0;
        if (raw != msw_raw[i]) {
            msw_raw[i] = raw;
            msw_raw_time[i] = now;
        }
        if (msw_prev[i] == raw || now - msw_raw_time[i] < MSW_DEBOUNCE_MS) continue;

        const int8_t prev = msw_prev[i];
        msw_prev[i] = raw;
        if (prev == 0 && raw == 1 && AUTO_LOAD_MS > 0 && motor_run[i].source != MOTOR_AUTOLOAD &&
            !autoload_blocked(i) && motors[i].getStopState()) {
            if (motorCmd(i, 1, MOTOR_AUTOLOAD, AUTO_LOAD_MS))
                LOG_MAIN("[autoload] 通道 %d 检测到有料，限时进料 %dms", i + 1, AUTO_LOAD_MS);
            else
                LOG_MAIN("[autoload] 通道 %d 达到并发上限，本次辅助进料已拒绝", i + 1);
        }
    }
}

int BMCU_motor_count() { return active_channel_count(); }
int BMCU_max_ams_count() { return IO_CONFIG_MAX_CHANNELS / 4; }
bool BMCU_manual_active() {
    if (manual_queued) return true;
    for (const auto &m : motor_run) if (m.source == MOTOR_MANUAL) return true;
    return false;
}

// 供 Web 页面显示自动(有料)/(无料)：1=有料 0=无料 -1=该通道没有微动开关
int BMCU_channel_filament_state(const int motorNum) {
    constexpr int pinCount = sizeof(IO_CONFIG.microswitch) / sizeof(IO_CONFIG.microswitch[0]);
    if (motorNum < 0 || motorNum >= active_channel_count() || motorNum >= pinCount) return -1;
    if (IO_CONFIG.microswitch[motorNum] == PIN_NONE) return -1;
    return is_filament_online(motorNum) ? 1 : 0;
}

void BMCU_apply_channel_modes() {
    for (int i = 0; i < 16; i++) {
        const bool enabled = i < active_channel_count();
        bool online_now = enabled && CHANNEL_MODE[i] == CHANNEL_MODE_ONLINE;
        if (enabled && CHANNEL_MODE[i] == CHANNEL_MODE_AUTO &&
            IO_CONFIG.microswitch[i] != PIN_NONE) {
            int pin = IO_CONFIG.microswitch[i];
            online_now = !(IS_PCF8575_VIRTUAL_PIN(pin) ? PCF8575_digitalRead(pin) : digitalRead(pin));
        }
        set_filament_online(i / 4, i % 4, online_now);
    }
}

bool BMCU_can_maintain() {
    // 手动维护不再因为打印机总线近期有通信而被拒绝，只检查本地运动状态。
    if (BMCU_running_count() || manual_queued) return false;
    for (int i = 0; i < active_channel_count(); i++) {
        if (!motors[i].getStopState() || get_filament_motion(i) != waiting) return false;
    }
    return true;
}

int BMCU_running_count() {
    int count = 0;
    for (const auto &m : motor_run) count += m.direction != 0;
    return count;
}

bool BMCU_printer_bus_fault() {
    return printer_bus_fault;
}

MotorStatus BMCU_motor_status(int ch) {
    if (ch < 0 || ch >= active_channel_count()) return {};
    const auto &m = motor_run[ch];
    const auto left = static_cast<int32_t>(m.end - millis());
    const int pos = queue_position(ch);
    return {m.direction, m.source, !m.direction ? 0 : m.timed ? (left > 0 ? left : 0) : -1,
            printer_blocked[ch], pos, pos ? manual_queue[pos - 1].direction : 0};
}

void BMCU_printer_idle(int ch) {
    if (ch < 0 || ch >= active_channel_count()) return;
    printer_blocked[ch] = false;
    last_action[ch] = waiting;
}

void BMCU_stop_motor(int ch) {
    if (ch < 0 || ch >= IO_CONFIG_MAX_CHANNELS) return;
    const int pos = queue_position(ch);
    if (pos) remove_job(pos - 1);
    // 内部设置 waiting 不解锁；只在总线真正收到 idle 后允许打印机再次启动。
    if (motor_run[ch].source == MOTOR_PRINTER || get_filament_motion(ch) != waiting)
        printer_blocked[ch] = true;
    motorCmd(ch, 0);
    set_filament_motion(ch, waiting);
}

bool BMCU_manual_pulse(int ch, const int cmd, const uint32_t duration_ms) {
    if (ch < 0 || ch >= active_channel_count() || (cmd != 1 && cmd != -1) || !duration_ms ||
        duration_ms >= MAX_PULSE_MS || motor_run[ch].direction || queue_position(ch) ||
        manual_queued >= active_channel_count() ||
        !BMCU_channel_motor_ready(ch) ||
        get_filament_motion(ch) != waiting) return false;
    manual_queue[manual_queued++] = {ch, cmd, duration_ms};
    run_manual_queue();
    return true;
}

bool motorCmd(int ch, const int cmd, const MotorSource source, const uint32_t duration_ms) {
    if (ch < 0 || ch >= IO_CONFIG_MAX_CHANNELS || cmd < -1 || cmd > 1) return false;
    auto &m = motor_run[ch];
    if (!cmd) {
        motors[ch].stop();
        m = {};
        return true;
    }
    if (ch >= active_channel_count()) return false;
    if (!BMCU_channel_motor_ready(ch)) return false;
    if (source == MOTOR_PRINTER && (printer_bus_fault || printer_blocked[ch])) return false;
    if ((source != MOTOR_MANUAL && queue_position(ch)) ||
        (m.direction && m.source != source) ||
        (!m.direction && BMCU_running_count() >= MOTOR_LIMIT)) {
        // 拒绝本次打印机动作，不在名额空出后悄悄执行积压命令。
        if (source == MOTOR_PRINTER) {
            printer_blocked[ch] = true;
            LOG_MAIN("[motor] 通道 %d 忙碌或达到并发上限，打印机动作已拒绝", ch + 1);
        }
        return false;
    }
    if (m.direction == cmd && m.source == source) return true; // 重复命令不续期
    m.direction = cmd;
    m.source = source;
    m.end = millis() + duration_ms;
    m.timed = duration_ms > 0;
    if (cmd > 0) motors[ch].forward();
    else motors[ch].backforward();
    return true;
}

static void motor_tick() {
    for (int i = 0; i < active_channel_count(); i++) {
        auto m = BMCU_motor_status(i);
        if (!m.direction || m.remaining_ms != 0) continue;
        if (m.source == MOTOR_PRINTER) BMCU_stop_motor(i);
        else motorCmd(i, 0);
    }
    run_manual_queue();
}

/**
 * @brief 设置电机PWM占空比
 * @param motorNum 电机编号(0~3)
 * @param dutyCycle 占空比(0-255)
 * 功能：设置电机的速度
 */
void setMotorDutyCycle(const int motorNum, const uint8_t dutyCycle) {
    if (motorNum < 0 || motorNum >= active_channel_count()) return;
    motors[motorNum].setDutyCycle(dutyCycle);
}

/**
 * @brief 初始化AS5600传感器（模拟版）
 * 功能：初始化模拟的AS5600角度传感器
 */
void AS5600_init() {
    if_as5600_init = true;
    // 固定种子便于回放；该数据仍是模拟反馈，不代表真实传感器测量值。
    generator.seed(12345u);
    randomSeed(12345u);
    as5600_distance_last_ms = millis();
    
    // 初始化角度和时间戳
    for (int i = 0; i < 4; i++) {
        as5600_current_angle[i] = random(0, 4096);
        as5600_last_angle[i] = as5600_current_angle[i];
        as5600_last_time[i] = millis();
        as5600_speed[i] = 0.0f;
    }
    
    LOG_MAIN("AS5600 初始化%s", if_as5600_init ? "成功" : "失败");
}

/**
 * @brief 获取模拟的AS5600距离数据
 * @return 模拟的距离值
 * 功能：生成模拟的距离数据，用于测试
 */
float AS5600_get_distance_E() {
    if (!enable_as5600) return 0;

    // 以时间增量生成模拟送料距离，避免主循环频率改变累计速度。
    const uint32_t now = millis();
    uint32_t dt_ms = now - as5600_distance_last_ms;
    as5600_distance_last_ms = now;
    if (!dt_ms) return 0.0f;
    if (dt_ms > 100u) dt_ms = 100u; // 异常长调度间隔不制造大跳变

    const float scale = (float)dt_ms / 10.0f;
    const float nominal = 0.0055f * scale;
    const float jitter = (float)distribution(generator) * sqrtf(scale);
    const float lower = 0.003f * scale;
    const float upper = 0.008f * scale;
    const float distance = nominal + jitter;
    return distance < lower ? lower : distance > upper ? upper : distance;
}

/**
 * @brief 获取模拟的AS5600原始角度值
 * @param channel 通道号（0-3）
 * @return 模拟的角度值
 * 功能：生成模拟的角度数据，用于测试
 */
uint16_t AS5600_get_raw_angle(uint8_t channel) {
    if (!enable_as5600 || channel >= 4) return 0;
    
    // 模拟角度值的连续变化
    uint32_t now = millis();
    uint32_t time_diff = now - as5600_last_time[channel];
    
    // 计算角度增量（模拟电机转动）
    int16_t angle_increment = rand() % 10 - 5; // -5到4的随机增量
    
    // 更新角度值，确保在0-4095之间循环
    as5600_current_angle[channel] = (as5600_current_angle[channel] + angle_increment) % 4096;
    if (as5600_current_angle[channel] < 0) {
        as5600_current_angle[channel] += 4096;
    }
    
    // 计算速度
    int32_t angle_diff = as5600_current_angle[channel] - as5600_last_angle[channel];
    if (angle_diff >  2048) angle_diff -= 4096;
    if (angle_diff < -2048) angle_diff += 4096;
    
    if (time_diff > 0) {
        float dist_mm = (float)angle_diff * AS5600_MM_PER_CNT;
        as5600_speed[channel] = dist_mm / static_cast<float>(time_diff) * 1000.0f; // mm/s
    }
    
    // 更新最后角度和时间
    as5600_last_angle[channel] = as5600_current_angle[channel];
    as5600_last_time[channel] = now;
    
    return as5600_current_angle[channel];
}

/**
 * @brief 获取模拟的AS5600磁场状态
 * @param channel 通道号（0-3）
 * @return 磁场状态
 * 功能：返回模拟的磁场状态，始终为正常
 */
AS5600_magnet_stu AS5600_get_magnet_stu(uint8_t channel) {
    if (!enable_as5600 || channel >= 4) return AS5600_offline;
    
    // 始终返回正常磁场状态
    return AS5600_normal;
}

/**
 * @brief 检查AS5600是否在线
 * @param channel 通道号（0-3）
 * @return 是否在线
 * 功能：返回模拟的在线状态，始终为在线
 */
bool AS5600_is_online(uint8_t channel) {
    if (!enable_as5600 || channel >= 4) return false;
    
    // 始终返回在线状态
    return true;
}

/**
 * @brief 获取模拟的AS5600速度
 * @param channel 通道号（0-3）
 * @return 速度值
 * 功能：返回模拟的速度数据
 */
float AS5600_get_speed(uint8_t channel) {
    if (!enable_as5600 || channel >= 4) return 0.0f;
    return as5600_speed[channel];
}

/**
 * @brief BMCU系统初始化
 * 功能：初始化IO引脚、传感器、通信等
 */
void BMCU_init() {
    // 初始化PCF8575虚拟引脚映射
    // 只有 Web 开关打开 PCF8575 时才初始化：关闭时 hal_i2c_begin() 没有 Wire.begin()，
    // 这里再访问 I2C 会产生 "could not acquire lock / NULL TX buffer" 报错。
    if (PCF_ENABLE) PCF8575_init();
    
    if (switchPin != PIN_NONE) pinMode(switchPin, INPUT_PULLUP);
    BMCU_apply_io_config(); // 根据已加载设置绑定并初始化 IO 引脚
    AS5600_init();       // 初始化AS5600传感器

    BambuBus_init();     // 初始化BambuBus通信
    releaseAllPos();     // 释放所有位置
    delay(500);          // 延迟500ms

    // 初始化耗材长度记录
    for (int i = 0; i < max_filament_num; i++) {
        last_meters[i] = std::numeric_limits<float>::lowest();
        last_action[i] = waiting;
    }

    // 微动辅助进料：上电第一帧只记录状态，不立刻触发进料
    for (int i = 0; i < active_channel_count(); i++) {
        msw_prev[i] = -1;
        msw_raw[i] = -1;
        msw_raw_time[i] = 0;
    }

    BMCU_apply_channel_modes();

    LOG_MAIN("BMCU 初始化完成：通道数=%d，AMS 数量=%d", active_channel_count(), AMS_COUNT);
}

/**
 * @brief 微动开关中断处理
 * 功能：当微动开关被触发时，控制选中的电机进料200ms
 */
void switchInterrupt() {
    if (switchPin == PIN_NONE) return;
    switchState = digitalRead(switchPin);
    if (switchState == LOW && lastSwitchState != LOW) {
        int n = get_now_filament_num();
        if (n >= 0 && n < active_channel_count() && !motor_run[n].direction)
            motorCmd(n, 1, MOTOR_AUTOLOAD, 200);
    }
    lastSwitchState = switchState;
}

// 空实现：保留接口兼容性
void selectOnePos(char num) {}
void releaseAllPos() {}
void BMCU_bus_deal_set_motion_res(uint8_t *data, int len) {}

/**
 * BMCU主运行逻辑
 * 功能：处理耗材状态、更新耗材长度、运行通信和调试逻辑
 */
void BMCU_run() {
    motor_tick();
    const int bus_stu = ams_enable ? BambuBus_run() : 0;
    if (bus_stu < 0) {
        if (!printer_bus_fault)
            LOG_MAIN("[bus] 打印机总线超过 1 秒未收到合法帧，停止打印机动作");
        printer_bus_fault = true;
        for (int i = 0; i < active_channel_count(); i++) {
            if (motor_run[i].source == MOTOR_PRINTER || get_filament_motion(i) != waiting)
                BMCU_stop_motor(i);
            // 即使当前没有运行中的电机，也阻止超时期间的新动作。
            printer_blocked[i] = true;
        }
    } else if (bus_stu == 1 && printer_bus_fault) {
        printer_bus_fault = false;
        for (int i = 0; i < active_channel_count(); i++) printer_blocked[i] = false;
        LOG_MAIN("[bus] 收到合法打印机帧，总线恢复，允许新的打印机动作");
    }
    bool if_count_meters = true;
    float d = AS5600_get_distance_E();
    static int now_f = 255;
    int x = get_now_filament_num();
    for (int i = 0; i < active_channel_count(); i++) {
        if (i != x && motor_run[i].source == MOTOR_PRINTER && !motor_run[i].timed)
            motorCmd(i, 0);
    }

    // 耗材槽切换处理
    if (now_f != x) {
        now_f = x;
        Bambubus_set_need_to_save(); // 设置需要保存
    }

    // 处理耗材运动状态（匹配进退料）
    const auto state = (now_f >= 0 && now_f < active_channel_count()) ? get_filament_motion(now_f) : waiting;
    if (now_f >= 0 && now_f < active_channel_count() && state == need_pull_back) {
        // 需要退料
        if (last_action[now_f] != need_pull_back) {
            last_action[now_f] = need_pull_back;
            last_pullback_meters[now_f] = get_filament_meters(now_f);
            motorCmd(now_f, -1, MOTOR_PRINTER, PULLBACK_TIMEOUT[now_f] * 1000u);
        }
    } else if (now_f >= 0 && now_f < active_channel_count() && state == need_send_out) {
        // 需要进料
        last_action[now_f] = need_send_out;
    } else if (now_f >= 0 && now_f < active_channel_count() && state == waiting) {
        // 待机
        last_action[now_f] = waiting;
    }

    // 更新耗材长度
    if ((if_count_meters || ams_enable == false) && now_f >= 0 && now_f < active_channel_count()) {
        if(state == need_send_out){
            d = -d; // 进料时长度减少
        }
        if (!printer_blocked[now_f]) add_filament_meters(now_f, d);
    }

    BMCU_motion();   // 处理电机运动
}

/**
 * @brief 电机运动逻辑
 * 功能：根据当前状态控制电机的运动
 */
void BMCU_motion() {
    if (printer_bus_fault) {
        for (int i = 0; i < active_channel_count(); i++) {
            if (motor_run[i].source == MOTOR_PRINTER) motorCmd(i, 0);
        }
        return;
    }
    int cur = get_now_filament_num();
    if (cur < 0 || cur >= active_channel_count()) {
        for (int i = 0; i < active_channel_count(); i++) {
            if (motor_run[i].source != MOTOR_PRINTER || motor_run[i].timed) continue;
            if (!motors[i].getStopState()) motorCmd(i, 0);
        }
        return;
    }
    // 耗材槽切换防抖
    if (cur != last_num) {
        last_num = cur;
        last_num_change_time = millis();
        execute_motion = false;
    }
    if (!execute_motion && millis()-last_num_change_time >= SERVO_DELAY)
        execute_motion = true;

    if (motor_run[cur].source == MOTOR_MANUAL) {
        if (get_filament_motion(cur) != waiting) motorCmd(cur, 1); // 拒绝占用中的通道，不延后执行
        return;
    }
    if (motor_run[cur].source == MOTOR_AUTOLOAD) {
        if (get_filament_motion(cur) == waiting) return;
        motorCmd(cur, 0);
    }

    // 更新当前动作状态
    if (ams_enable) act = get_filament_motion(cur);

    // 处理不同运动状态（严格匹配进退料）
    switch (act) {
        case need_pull_back:
            // 待退料
            break;
        case need_send_out:
            // 待进料：执行进料（正转）
            if (execute_motion && is_filament_online(cur)) {
                motorCmd(cur, 1); // 正转→进料
            } else {
                motorCmd(cur, 0); // 停止
            }
            break;
        case waiting:
            // 待机：停止非退料电机，处理微动开关
            if (!motor_run[cur].timed) motorCmd(cur, 0);
            switchInterrupt();
            break;
        case act_send_mm:
            // 执行进料（正转）
            if (last_pullback_meters[cur] - get_filament_meters(cur) > target) {
                motorCmd(cur, 0); // 达到目标→停止
                act = waiting;
            } else if (execute_motion && is_filament_online(cur)) {
                motorCmd(cur, 1); // 继续进料（正转）
            } else {
                motorCmd(cur, 0); // 停止
            }
            break;
        case act_pull_mm:
            // 执行退料（反转）
            if (get_filament_meters(cur) - last_pullback_meters[cur] > target) {
                motorCmd(cur, 0); // 达到目标→停止
                act = waiting;
            } else if (execute_motion && is_filament_online(cur)) {
                motorCmd(cur, -1); // 继续退料（反转）
            } else {
                motorCmd(cur, 0); // 停止
            }
            break;
        case cancel:
        case release_all:
            // 取消/释放→停止电机
            motorCmd(cur, 0); 
            act = waiting;
            break;
        default:
            // 默认→处理微动开关
            switchInterrupt(); 
            break;
    }
}
