/**
 * BMCU.cpp - BMCU主控单元实现
 * 
 * 功能说明：
 * 1. 电机控制：实现电机的正转（进料）和反转（退料）控制
 * 2. 传感器模拟：模拟AS5600角度传感器数据
 * 3. 通信协议：与Bambu打印机进行通信
 * 4. 状态管理：管理耗材状态和运动状态
 */

#undef abs
#include "BMCU.h"
#include "Motor.h"
#include <cmath>
#include <random>
#include "PCF8575_PinMap.h"
#include "settings_storage.h"
#include "log_bus.h"
#include "i2c_bus.h"
#include "BambuBus.h"

namespace {
constexpr uint32_t CHANNEL_SWITCH_DELAY_MS = 100; // 通道切换后的运动稳定期
constexpr uint32_t FEED_STOP_DELAY_MS = 1000;     // 打印机停止进料后的延迟停车时间
constexpr uint32_t MSW_DEBOUNCE_MS = 30;          // 微动开关防抖时间
constexpr float ACT_TARGET_METERS = 0.0f;         // 兼容动作的目标行程

// 单个通道的电机运行信息，停止时整体清零
struct MotorRun {
    int direction = 0;                 // 1=进料，-1=退料，0=停止
    MotorSource source = MOTOR_IDLE;   // 本次动作来源
    uint32_t end = 0;                  // 限时动作截止 millis()
    bool timed = false;                // false 表示由打印机状态机持续控制
};

// 一次等待执行的手动点动任务
struct MotorJob {
    int channel;
    int direction;                     // 1=进料，-1=退料
    uint32_t duration;                 // 持续时间（毫秒）
};

// 单通道的电机、互锁和传感器运行状态
struct ChannelState {
    MotorRun motor;
    uint32_t feed_stop_deadline = 0;       // 延迟停车截止 millis()
    filament_motion_state_set previous_motion = waiting;
    float pullback_start_meters = 0.0f;    // 退料开始时的耗材长度
    uint32_t msw_raw_time = 0;             // 原始状态最近变化时间
    // 单字节字段集中放在末尾，减少结构体对齐填充
    bool printer_blocked = false;         // 等待打印机 idle 或总线恢复
    bool feed_stop_pending = false;       // 正在等待进料延迟停车
    int8_t msw_previous = -1;              // 防抖后的微动状态：-1 未知，0 无料，1 有料
    int8_t msw_raw = -1;                   // 防抖前的微动状态
};
}

// PCF8575 扩展 IO 对象；I2C 总线配置有效时由 PCF8575_init() 初始化
PCF8575 pcf8575(PCF8575_ADDR);

// 所有电机调用均在 Arduino loop 中串行执行，不在 ISR 中驱动
// 每个通道的运行、互锁、延迟停车和微动状态集中保存在这里
static ChannelState channel_state[IO_CONFIG_MAX_CHANNELS];
// 打印机总线超时后禁止新的打印机动作，直到收到合法新帧
static bool printer_bus_fault = false;

// 等待启动的手动点动任务，按提交顺序执行
static MotorJob manual_queue[IO_CONFIG_MAX_CHANNELS];
// 手动队列中当前有效的任务数量
static int manual_queued = 0;

/**
 * 查询通道在手动队列中的位置
 * @param ch 通道编号
 * @return 从 1 开始的排队位置；未排队返回 0
 * 顺序扫描手动动作队列，供状态接口显示排队位置
 */
static int queue_position(const int ch) {
    for (int i = 0; i < manual_queued; i++) if (manual_queue[i].channel == ch) return i + 1;
    return 0;
}

/**
 * 从手动队列删除指定任务
 * @param index 队列下标
 * 把后续任务前移并减少队列长度
 */
static void remove_job(const int index) {
    for (int i = index; i + 1 < manual_queued; i++) manual_queue[i] = manual_queue[i + 1];
    --manual_queued;
}

/**
 * 获取电机动作来源名称
 * @param source 电机动作来源
 * @return 供主板日志显示的中文名称
 * 未知来源按空闲处理，避免日志输出枚举数字难以排查
 */
static const char *motor_source_name(const MotorSource source) {
    switch (source) {
        case MOTOR_PRINTER: return "打印机";
        case MOTOR_MANUAL: return "手动";
        case MOTOR_AUTOLOAD: return "微动辅助";
        default: return "空闲";
    }
}

/**
 * 获取电机方向名称
 * @param cmd 电机命令：1=进料，-1=退料，0=停止
 * @return 供主板日志显示的中文方向
 * 仅用于日志格式化，不参与运动判断
 */
static const char *motor_direction_name(const int cmd) {
    return cmd > 0 ? "进料" : cmd < 0 ? "退料" : "停止";
}

/**
 * 检查通道编号是否在固定容量内
 * @param motorNum 通道编号
 * @return true 表示编号可用于索引 channel_state
 * 用于配置、停止和状态查询等允许访问未启用通道的场景
 */
static bool valid_channel(const int motorNum) {
    return motorNum >= 0 && motorNum < IO_CONFIG_MAX_CHANNELS;
}

/**
 * 检查通道是否处于当前启用范围
 * @param motorNum 通道编号
 * @return true 表示通道已启用
 * 电机启动、手动操作和状态展示只处理启用通道
 */
static bool is_active_channel(const int motorNum) {
    return motorNum >= 0 && motorNum < IO_CONFIG_MAX_CHANNELS && motorNum < active_channel_count();
}

/**
 * 判断当前通道是否已经通过切换稳定期
 * @param motorNum 当前耗材通道
 * @return true 表示可以执行运动
 * 首次看到某个通道时只建立基线并返回 false，等待 CHANNEL_SWITCH_DELAY_MS 后放行
 */
static bool channel_switch_ready(const int motorNum) {
    static int previous_channel = -1;
    static uint32_t changed_at = 0;
    if (motorNum != previous_channel) {
        previous_channel = motorNum;
        changed_at = millis();
        return false;
    }
    return millis() - changed_at >= CHANNEL_SWITCH_DELAY_MS;
}

/**
 * 检查通道电机引脚是否配置完整
 * @param motorNum 通道编号
 * @return true 表示进料和退料引脚都已配置
 * 任一方向引脚为 PIN_NONE 时拒绝启动电机，避免界面显示运行但硬件无输出
 */
bool BMCU_channel_motor_ready(const int motorNum) {
    if (!valid_channel(motorNum)) return false;
    const int forward_pin = IO_CONFIG.motor[motorNum][0];
    const int backward_pin = IO_CONFIG.motor[motorNum][1];
    if (forward_pin == PIN_NONE || backward_pin == PIN_NONE) return false;
    // 扩展引脚依赖 PCF8575；芯片未检测到时不能假装电机已就绪
    if ((IS_PCF8575_VIRTUAL_PIN(forward_pin) || IS_PCF8575_VIRTUAL_PIN(backward_pin)) &&
        !PCF8575_is_online()) {
        return false;
    }
    return true;
}

/**
 * 启动排队中的手动电机动作
 * 在并发上限内依次取出队首任务启动；无效通道或引脚未配置的任务会被丢弃
 */
static void run_manual_queue() {
    while (manual_queued && BMCU_running_count() < MOTOR_LIMIT) {
        const auto job = manual_queue[0];
        if (!is_active_channel(job.channel)) {
            remove_job(0);
            continue;
        }
        // 该通道没有电机引脚：丢掉这次请求，别让它卡住后面的排队动作
        if (!BMCU_channel_motor_ready(job.channel)) {
            remove_job(0);
            continue;
        }
        if (!motorCmd(job.channel, job.direction, MOTOR_MANUAL, job.duration)) break;
        remove_job(0);
    }
}

// 电机对象数组；具体引脚由设置加载后在 BMCU_init()/BMCU_apply_io_config() 绑定
// 16 个通道的 Motor 实例，负责实际的 GPIO/PWM 输出
static Motor motors[IO_CONFIG_MAX_CHANNELS];

/**
 * 应用最新的通道引脚配置
 * 对比新旧配置，先停止并释放变更通道，再绑定新引脚；未变化通道保持运行状态
 */
void BMCU_apply_io_config() {
    // 上一次已应用到硬件的引脚配置，用与 IO_CONFIG 比较找出变更通道
    static IoConfig applied_io = {};
    // 上一次已应用的启用通道数，用于识别通道增减
    static int applied_channels = 0;
    const int channels = active_channel_count();
    bool changed[IO_CONFIG_MAX_CHANNELS] = {};
    // 先释放所有变更/停用通道，再绑定新引脚，避免引脚交换时旧通道拉低新输出
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
        auto &ch = channel_state[i];
        ch.previous_motion = waiting;
        ch.printer_blocked = false;
        ch.msw_previous = ch.msw_raw = -1;
        ch.msw_raw_time = 0;
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
 * 获取当前启用通道数
 * @return AMS 数量乘以 4
 * 供状态接口、网页和运动逻辑统一使用
 */
int BMCU_motor_count() { return active_channel_count(); }
/**
 * 获取硬件支持的最大 AMS 数量
 * @return 最大 AMS 数量
 * 由固定通道容量除以 4 得到
 */
int BMCU_max_ams_count() { return IO_CONFIG_MAX_CHANNELS / 4; }
/**
 * 判断是否存在手动动作
 * @return true 表示有手动任务在运行或排队
 * 检查手动队列和所有手动来源的电机
 */
bool BMCU_manual_active() {
    if (manual_queued) return true;
    for (const auto &ch: channel_state) if (ch.motor.source == MOTOR_MANUAL) return true;
    return false;
}

/**
 * 读取通道耗材状态
 * @param motorNum 通道编号
 * @return 1=有料，0=无料，-1=无微动或通道无效
 * 供网页显示自动检测结果
 */
int BMCU_channel_filament_state(const int motorNum) {
    if (!is_active_channel(motorNum)) return -1;
    if (IO_CONFIG.microswitch[motorNum] == PIN_NONE) return -1;
    return is_filament_online(motorNum) ? 1 : 0;
}

/**
 * 按通道模式刷新在线状态
 * 在线模式直接标记有料；自动模式读取微动；其余模式标记无料
 */
void BMCU_apply_channel_modes() {
    for (int i = 0; i < IO_CONFIG_MAX_CHANNELS; i++) {
        const bool enabled = i < active_channel_count();
        bool online_now = enabled && CHANNEL_MODE[i] == CHANNEL_MODE_ONLINE;
        if (enabled && CHANNEL_MODE[i] == CHANNEL_MODE_AUTO &&
            IO_CONFIG.microswitch[i] != PIN_NONE) {
            int pin = IO_CONFIG.microswitch[i];
            if (!IS_PCF8575_VIRTUAL_PIN(pin) || PCF8575_is_online())
                online_now = !(IS_PCF8575_VIRTUAL_PIN(pin) ? PCF8575_digitalRead(pin) : digitalRead(pin));
            else
                online_now = false;
        }
        set_filament_online(i / 4, i % 4, online_now);
    }
}

/**
 * 判断当前是否允许维护操作
 * @return true 表示可以安全修改配置或升级
 * 要求没有手动排队、没有电机运行、所有通道处于停止且打印机状态为 waiting
 */
bool BMCU_can_maintain() {
    // 手动维护不再因为打印机总线近期有通信而被拒绝，只检查本地运动状态
    if (BMCU_running_count() || manual_queued) return false;
    for (int i = 0; i < active_channel_count(); i++) {
        if (!motors[i].getStopState() || get_filament_motion(i) != waiting) return false;
    }
    return true;
}

/**
 * 统计正在运行的电机数量
 * @return 当前方向非零的通道数
 * 用于限制同时运行的电机数量
 */
int BMCU_running_count() {
    int count = 0;
    for (const auto &ch: channel_state) count += ch.motor.direction != 0;
    return count;
}

/**
 * 查询打印机总线故障状态
 * @return true 表示总线超时并已阻止打印机动作
 * 返回最近一次总线合法性判断的结果
 */
bool BMCU_printer_bus_fault() {
    return printer_bus_fault;
}

/**
 * 读取通道电机完整状态
 * @param motorNum 通道编号
 * @return 包含方向、来源、剩余时间、阻断和排队信息的 MotorStatus
 * 越界通道返回默认空闲状态
 */
MotorStatus BMCU_motor_status(const int motorNum) {
    if (!is_active_channel(motorNum)) return {};
    const auto &ch = channel_state[motorNum];
    const auto &m = ch.motor;
    int32_t remaining_ms = 0;
    if (m.direction) {
        if (!m.timed) {
            remaining_ms = -1;
        } else {
            const auto left = static_cast<int32_t>(m.end - millis());
            remaining_ms = left > 0 ? left : 0;
        }
    }
    const int pos = queue_position(motorNum);
    return {m.direction, m.source, remaining_ms, ch.printer_blocked, pos,
            pos ? manual_queue[pos - 1].direction : 0};
}

/**
 * 解除通道的打印机阻断
 * @param motorNum 通道编号
 * 收到打印机 idle 后清除 blocked 标志，并把上一次动作恢复为 waiting
 */
void BMCU_printer_idle(const int motorNum) {
    if (!is_active_channel(motorNum)) return;
    auto &ch = channel_state[motorNum];
    if (ch.motor.source == MOTOR_PRINTER && ch.motor.timed && ch.motor.direction < 0) {
        // 退料由 Web 超时独立控制，打印机提前空闲也必须跑满设定时间
        LOG_MAIN("[motor] 通道 %d 打印机已空闲，但退料定时未到，继续运行", motorNum + 1);
    }
    ch.printer_blocked = false;
    ch.previous_motion = waiting;
}

/**
 * 清除通道的进料延迟停止
 * @param ch 通道编号
 * 主动停车、配置变更或打印机恢复进料时调用；只清除延迟标记，不直接驱动电机
 */
static void clear_feed_stop_delay(const int ch) {
    if (!valid_channel(ch)) return;
    channel_state[ch].feed_stop_pending = false;
    channel_state[ch].feed_stop_deadline = 0;
}

/**
 * 安排打印机进料结束后的延迟停止
 * @param ch 通道编号
 * @return true 表示本次新安排了延迟停止
 * 仅对打印机来源、正转且未设定时长的进料生效；重复的待机帧不会延长时限
 */
static bool arm_feed_stop_delay(const int ch) {
    if (!valid_channel(ch) || channel_state[ch].feed_stop_pending) return false;
    const auto &m = channel_state[ch].motor;
    if (m.source != MOTOR_PRINTER || m.direction <= 0 || m.timed) return false;
    channel_state[ch].feed_stop_pending = true;
    channel_state[ch].feed_stop_deadline = millis() + FEED_STOP_DELAY_MS;
    LOG_MAIN("[motor] 通道 %d 打印机请求停止进料，保持运行 %d ms 后停止", ch + 1,
             FEED_STOP_DELAY_MS);
    return true;
}

/**
 * 停止指定通道并清理排队
 * @param motorNum 通道编号
 * 移除该通道的排队任务、停止电机并回到 waiting；打印机来源的动作会设置阻断，等待总线确认
 */
void BMCU_stop_motor(const int motorNum) {
    if (!valid_channel(motorNum)) return;
    auto &ch = channel_state[motorNum];
    const int pos = queue_position(motorNum);
    if (pos) remove_job(pos - 1);
    // 内部设置 waiting 不解锁；只在总线真正收到 idle 后允许打印机再次启动
    if (ch.motor.source == MOTOR_PRINTER || get_filament_motion(motorNum) != waiting)
        ch.printer_blocked = true;
    motorCmd(motorNum, 0);
    set_filament_motion(motorNum, waiting);
    // 强制停车后同步清掉边沿缓存，避免打印机再次请求退料时被误判为重复命令
    ch.previous_motion = waiting;
}

/**
 * 提交一次手动点动请求
 * @param motorNum 通道编号
 * @param cmd 1=进料，-1=退料
 * @param duration_ms 点动时长（毫秒）
 * @return true 表示已启动或已排队
 * 校验参数、通道状态和并发限制后加入手动队列并尝试立即启动
 */
bool BMCU_manual_pulse(const int motorNum, const int cmd, const uint32_t duration_ms) {
    if (!is_active_channel(motorNum) || (cmd != 1 && cmd != -1) || !duration_ms ||
        duration_ms >= MAX_PULSE_MS)
        return false;
    if (channel_state[motorNum].motor.direction || queue_position(motorNum) ||
        manual_queued >= active_channel_count() || !BMCU_channel_motor_ready(motorNum) ||
        get_filament_motion(motorNum) != waiting)
        return false;
    manual_queue[manual_queued++] = {.channel = motorNum, .direction = cmd, .duration = duration_ms};
    run_manual_queue();
    return true;
}

/**
 * 统一控制电机启停
 * @param motorNum 通道编号
 * @param cmd 1=正转，-1=反转，0=停止
 * @param source 动作来源
 * @param duration_ms 限时动作时长，0 表示不自动停止
 * @return true 表示命令被接受
 * 检查来源互锁、并发上限和通道占用后驱动 Motor，并记录动作截止时间；重复命令不续期
 */
bool motorCmd(const int motorNum, const int cmd, const MotorSource source, const uint32_t duration_ms) {
    if (!valid_channel(motorNum) || cmd < -1 || cmd > 1) return false;
    auto &ch = channel_state[motorNum];
    auto &m = ch.motor;
    if (!cmd) {
        clear_feed_stop_delay(motorNum);
        const int old_direction = m.direction;
        const MotorSource old_source = m.source;
        const bool was_active = old_direction != 0 || !motors[motorNum].getStopState();
        motors[motorNum].stop();
        m = {};
        if (was_active) {
            LOG_MAIN("[motor] 通道 %d 停止，原来源=%s，原方向=%s", motorNum + 1,
                     motor_source_name(old_source), motor_direction_name(old_direction));
        }
        return true;
    }
    if (!is_active_channel(motorNum)) return false;
    if (!BMCU_channel_motor_ready(motorNum)) return false;
    if (source == MOTOR_PRINTER && (printer_bus_fault || ch.printer_blocked)) return false;
    if ((source != MOTOR_MANUAL && queue_position(motorNum)) ||
        (m.direction && m.source != source) ||
        (!m.direction && BMCU_running_count() >= MOTOR_LIMIT)) {
        // 拒绝本次打印机动作，不在名额空出后悄悄执行积压命令
        if (source == MOTOR_PRINTER) {
            ch.printer_blocked = true;
            LOG_MAIN("[motor] 通道 %d 忙碌或达到并发上限，打印机动作已拒绝", motorNum + 1);
        }
        return false;
    }
    if (m.direction == cmd && m.source == source) return true; // 重复命令不续期
    m.direction = cmd;
    m.source = source;
    m.end = millis() + duration_ms;
    m.timed = duration_ms > 0;
    if (cmd > 0) motors[motorNum].forward();
    else motors[motorNum].backward();
    if (duration_ms > 0) {
        LOG_MAIN("[motor] 通道 %d %s启动，来源=%s，定时 %lu ms", motorNum + 1,
                 motor_direction_name(cmd), motor_source_name(source),
                 static_cast<unsigned long>(duration_ms));
    } else {
        LOG_MAIN("[motor] 通道 %d %s启动，来源=%s，持续运行", motorNum + 1,
                 motor_direction_name(cmd), motor_source_name(source));
    }
    return true;
}

/**
 * 同步正在运行的打印机退料超时
 * @param motorNum 通道编号
 * @param previous_timeout_ms 修改前本次动作使用的超时时间（毫秒）
 * 仅更新打印机来源的限时动作，并按已经运行的时间重算剩余时间；其他来源不受影响
 */
void BMCU_apply_pullback_timeout(const int motorNum, const uint32_t previous_timeout_ms) {
    if (!is_active_channel(motorNum)) return;
    auto &m = channel_state[motorNum].motor;
    if (!m.direction || !m.timed || m.source != MOTOR_PRINTER) return;

    const uint32_t now = millis();
    const uint32_t left = m.end > now ? m.end - now : 0;
    const uint32_t elapsed = previous_timeout_ms > left ? previous_timeout_ms - left : 0;
    const uint32_t new_timeout_ms = static_cast<uint32_t>(PULLBACK_TIMEOUT[motorNum]) * 1000u;
    const uint32_t remaining = new_timeout_ms > elapsed ? new_timeout_ms - elapsed : 0;
    m.end = now + remaining;
    LOG_MAIN("[motor] 通道 %d 退料超时更新为 %d 秒，当前动作剩余 %lu ms", motorNum + 1,
             PULLBACK_TIMEOUT[motorNum], static_cast<unsigned long>(remaining));
}

/**
 * 处理限时电机和手动队列
 * 停止已到期的限时动作，并继续启动排队中的手动任务
 */
static void motor_tick() {
    const uint32_t now = millis();
    for (int i = 0; i < active_channel_count(); i++) {
        auto &ch = channel_state[i];
        auto &m = ch.motor;
        // 打印机停止进料后先保持运行；动作失效时直接清除，避免残留延时
        if (ch.feed_stop_pending) {
            if (m.source != MOTOR_PRINTER || m.direction <= 0 || m.timed) {
                clear_feed_stop_delay(i);
            } else if (static_cast<int32_t>(now - ch.feed_stop_deadline) >= 0) {
                LOG_MAIN("[motor] 通道 %d 进料停止延迟到期，停止电机", i + 1);
                motorCmd(i, 0);
            }
        }
        if (!m.direction || !m.timed) continue;
        if (static_cast<int32_t>(now - m.end) < 0) continue;
        if (m.source == MOTOR_PRINTER) {
            // 打印机动作已按 Web 设定的时长完整跑完，属于正常结束，不再等待打印机 idle
            LOG_MAIN("[motor] 通道 %d 打印机定时动作到期，按设定时长停止", i + 1);
            motorCmd(i, 0);
            set_filament_motion(i, waiting);
            BMCU_printer_idle(i);
        } else {
            motorCmd(i, 0);
        }
    }
    run_manual_queue();
}

// ------------------------------ AS5600 start ------------------------------

// 随机数生成器（用于模拟传感器数据） 固定种子的随机数生成器，保证每次上电的模拟距离抖动可复现
std::mt19937 generator(12345);

// 正态采样一次生成两个值，缓存另一个供下一轮使用
static bool as5600_jitter_cached = false;
static float as5600_jitter_next = 0.0f;

/**
 * 生成模拟距离使用的正态抖动
 * @return 均值为 0、标准差为 0.00083 的单精度采样值
 * 直接将 MT19937 高 24 位转为均匀数，再用极坐标法生成正态分布；
 * 避免标准分布内部的 long double/logl 路径在部分 RISC-V 工具链中递归溢出
 */
static float as5600_distance_jitter() {
    if (as5600_jitter_cached) {
        as5600_jitter_cached = false;
        return as5600_jitter_next * 0.00083f;
    }
    float x, y, radius;
    do {
        x = static_cast<float>(generator() >> 8) * (1.0f / 8388608.0f) - 1.0f;
        y = static_cast<float>(generator() >> 8) * (1.0f / 8388608.0f) - 1.0f;
        radius = x * x + y * y;
    } while (radius >= 1.0f || radius == 0.0f);
    const float factor = sqrtf(-2.0f * logf(radius) / radius);
    as5600_jitter_next = x * factor;
    as5600_jitter_cached = true;
    return y * factor * 0.00083f;
}

// 上一次生成模拟距离的 millis() 时间，用于按时间增量累计距离
static uint32_t as5600_distance_last_ms = 0;

// 各通道当前模拟角度（0..4095，对应一圈 4096 个计数）
int16_t as5600_current_angle[4] = {0, 0, 0, 0}; // 当前角度值
// 各通道上一次计算速度时的角度，用于求角度差
int16_t as5600_last_angle[4] = {0, 0, 0, 0}; // 上一次角度值
// 各通道上一次计算速度时的 millis() 时间戳
uint32_t as5600_last_time[4] = {0, 0, 0, 0}; // 上一次时间戳
// 各通道最近一次估算的线速度，单位 mm/s
float as5600_speed[4] = {0.0f, 0.0f, 0.0f, 0.0f}; // 速度值

/**
 * 初始化AS5600传感器（模拟版）
 * 重置模拟角度、距离计时、随机种子和正态采样缓存，保证重新初始化可复现
 */
void AS5600_init() {
    // 固定种子便于回放；该数据仍是模拟反馈，不代表真实传感器测量值
    generator.seed(12345u);
    as5600_jitter_cached = false;
    as5600_jitter_next = 0.0f;
    randomSeed(12345u);
    as5600_distance_last_ms = millis();

    // 初始化角度和时间戳
    for (int i = 0; i < 4; i++) {
        as5600_current_angle[i] = random(0, 4096);
        as5600_last_angle[i] = as5600_current_angle[i];
        as5600_last_time[i] = millis();
        as5600_speed[i] = 0.0f;
    }

    LOG_MAIN("AS5600 初始化成功");
}

/**
 * 获取模拟的AS5600距离数据
 * @return 按时间增量计算的模拟距离；同一毫秒重复调用返回 0
 * 使用单精度正态抖动并限制距离范围，最长只累计 100 毫秒
 */
float AS5600_get_distance_E() {
    // 以时间增量生成模拟送料距离，避免主循环频率改变累计速度
    const uint32_t now = millis();
    uint32_t dt_ms = now - as5600_distance_last_ms;
    as5600_distance_last_ms = now;
    if (!dt_ms) return 0.0f;
    if (dt_ms > 100u) dt_ms = 100u; // 异常长调度间隔不制造大跳变

    const float scale = static_cast<float>(dt_ms) / 10.0f;
    const float nominal = 0.0055f * scale;
    const float jitter = as5600_distance_jitter() * sqrtf(scale);
    const float lower = 0.003f * scale;
    const float upper = 0.008f * scale;
    const float distance = nominal + jitter;
    return distance < lower ? lower : distance > upper ? upper : distance;
}

/**
 * 获取模拟的AS5600原始角度值
 * @param channel 通道号（0-3）
 * @return 模拟的角度值
 * 生成模拟的角度数据，用于测试
 */
uint16_t AS5600_get_raw_angle(const uint8_t channel) {
    if (channel >= 4) return 0;

    // 模拟角度值的连续变化
    const uint32_t now = millis();
    const uint32_t time_diff = now - as5600_last_time[channel];

    // 计算角度增量（模拟电机转动）
    const int16_t angle_increment = rand() % 10 - 5; // -5到4的随机增量

    // 更新角度值，确保在0-4095之间循环
    as5600_current_angle[channel] = (as5600_current_angle[channel] + angle_increment) % 4096;
    if (as5600_current_angle[channel] < 0) {
        as5600_current_angle[channel] += 4096;
    }

    // 计算速度
    int32_t angle_diff = as5600_current_angle[channel] - as5600_last_angle[channel];
    if (angle_diff > 2048) angle_diff -= 4096;
    if (angle_diff < -2048) angle_diff += 4096;

    if (time_diff > 0) {
        const float dist_mm = (float) angle_diff * AS5600_MM_PER_CNT;
        as5600_speed[channel] = dist_mm / static_cast<float>(time_diff) * 1000.0f; // mm/s
    }

    // 更新最后角度和时间
    as5600_last_angle[channel] = as5600_current_angle[channel];
    as5600_last_time[channel] = now;

    return as5600_current_angle[channel];
}

/**
 * 获取模拟的AS5600磁场状态
 * @param channel 通道号（0-3）
 * @return 磁场状态
 * 返回模拟的磁场状态，始终为正常
 */
AS5600_magnet_stu AS5600_get_magnet_stu(const uint8_t channel) {
    if (channel >= 4) return AS5600_offline;

    // 始终返回正常磁场状态
    return AS5600_normal;
}

/**
 * 检查AS5600是否在线
 * @param channel 通道号（0-3）
 * @return 是否在线
 * 返回模拟的在线状态，始终为在线
 */
bool AS5600_is_online(const uint8_t channel) {
    if (channel >= 4) return false;

    // 始终返回在线状态
    return true;
}

/**
 * 获取模拟的AS5600速度
 * @param channel 通道号（0-3）
 * @return 速度值
 * 返回模拟的速度数据
 */
float AS5600_get_speed(const uint8_t channel) {
    if (channel >= 4) return 0.0f;
    return as5600_speed[channel];
}

// ------------------------------ AS5600 end ------------------------------

// ------------------------------ 总微动(五通) start ------------------------------

// 总微动开关引脚
constexpr int switchPin = PIN_SWITCH;

// 总微动开关的当前电平；LOW 表示触发
static int switchState = 0;
// 总微动开关的上一轮电平，用于检测 LOW 上升沿
static int lastSwitchState = 0;

// 微动开关中断处理 当微动开关被触发时，控制选中的电机进料200ms
static void switchInterrupt() {
    if (switchPin == PIN_NONE) return;
    switchState = digitalRead(switchPin);
    if (switchState == LOW && lastSwitchState != LOW) {
        int n = get_now_filament_num();
        if (is_active_channel(n) && !channel_state[n].motor.direction)
            motorCmd(n, 1, MOTOR_AUTOLOAD, 200);
    }
    lastSwitchState = switchState;
}

/**
 * 判断通道是否禁止微动辅助进料
 * @param motorNum 通道编号
 * @return true 表示当前不能触发辅助进料
 * 非自动模式、无微动输入、正被手动或打印机占用时返回 true
 */
static bool autoload_blocked(const int motorNum) {
    if (!is_active_channel(motorNum)) return true;
    if (CHANNEL_MODE[motorNum] != CHANNEL_MODE_AUTO) return true; // 仅自动模式触发
    if (IO_CONFIG.microswitch[motorNum] == PIN_NONE) return true; // 无真实微动输入
    if (channel_state[motorNum].motor.source == MOTOR_MANUAL) return true;
    if (get_filament_motion(motorNum) != waiting) return true; // 打印机正在占用该通道
    return false;
}

/**
 * 检测微动开关并触发自动进料
 * 轮询各通道微动状态，做 30ms 防抖；自动模式下由无料变有料时启动限时辅助进料
 */
void check_microswitch_status() {
    const uint32_t now = millis();

    for (int i = 0; i < active_channel_count(); i++) {
        auto &ch = channel_state[i];
        if (ch.motor.source == MOTOR_AUTOLOAD && (autoload_blocked(i) || AUTO_LOAD_MS <= 0))
            motorCmd(i, 0);

        // 非自动模式、超出启用通道或无微动输入的通道不参与触发
        if (CHANNEL_MODE[i] != CHANNEL_MODE_AUTO) {
            ch.msw_previous = ch.msw_raw = -1;
            continue;
        }

        const int pin = IO_CONFIG.microswitch[i];
        if (pin == PIN_NONE) {
            // 该平台未实现的通道
            ch.msw_previous = ch.msw_raw = -1;
            continue;
        }
        // PCF8575 未检测到时不能读取扩展输入，保持未知状态并跳过辅助进料
        if (IS_PCF8575_VIRTUAL_PIN(pin) && !PCF8575_is_online()) {
            ch.msw_previous = ch.msw_raw = -1;
            continue;
        }

        // sw 微动高低电平 默认拉低是在线
        const bool sw = IS_PCF8575_VIRTUAL_PIN(pin) ? PCF8575_digitalRead(pin) : digitalRead(pin);
        const bool online = !sw;
        set_filament_online(i / 4, i % 4, online); // 通道状态立即生效，界面实时刷新

        // 无料→有料需要稳定一段时间才算有效插入，避免开关抖动反复触发
        const int8_t raw = online ? 1 : 0;
        if (raw != ch.msw_raw) {
            ch.msw_raw = raw;
            ch.msw_raw_time = now;
        }
        if (ch.msw_previous == raw || now - ch.msw_raw_time < MSW_DEBOUNCE_MS) continue;

        const int8_t prev = ch.msw_previous;
        ch.msw_previous = raw;
        if (prev == 0 && raw == 1 && AUTO_LOAD_MS > 0 && ch.motor.source != MOTOR_AUTOLOAD &&
            !autoload_blocked(i) && motors[i].getStopState()) {
            if (motorCmd(i, 1, MOTOR_AUTOLOAD, AUTO_LOAD_MS))
                LOG_MAIN("[autoload] 通道 %d 检测到有料，限时进料 %dms", i + 1, AUTO_LOAD_MS);
            else
                LOG_MAIN("[autoload] 通道 %d 达到并发上限，本次辅助进料已拒绝", i + 1);
        }
    }
}

// ------------------------------ 总微动(五通) end ------------------------------

/**
 * BMCU系统初始化
 * 初始化IO引脚、传感器、通信等
 */
void BMCU_init() {
    // 初始化 PCF8575 虚拟引脚映射；I2C 未配置时不访问总线
    if (i2c_bus_configured()) {
        PCF8575_init();
        LOG_MAIN("[i2c] PCF8575=%s", PCF8575_is_online() ? "已检测" : "未检测");
    }

    if (switchPin != PIN_NONE) pinMode(switchPin, INPUT_PULLUP);
    BMCU_apply_io_config(); // 根据已加载设置绑定并初始化 IO 引脚
    AS5600_init(); // 初始化AS5600传感器

    BambuBus_init(); // 初始化BambuBus通信
    delay(500); // 延迟500ms

    BMCU_apply_channel_modes();

    LOG_MAIN("BMCU 初始化完成：通道数=%d，AMS 数量=%d", active_channel_count(), AMS_COUNT);
}

/**
 * BMCU主运行逻辑
 * 处理耗材状态、更新耗材长度、运行通信和调试逻辑
 */
void BMCU_run() {
    motor_tick();
    PCF8575_poll();
    const int bus_stu = BambuBus_run();
    if (bus_stu < 0) {
        if (!printer_bus_fault)
            LOG_MAIN("[bus] 打印机总线超过 1 秒未收到合法帧，停止打印机动作");
        printer_bus_fault = true;
        for (int i = 0; i < active_channel_count(); i++) {
            if (channel_state[i].motor.source == MOTOR_PRINTER || get_filament_motion(i) != waiting)
                BMCU_stop_motor(i);
            // 即使当前没有运行中的电机，也阻止超时期间的新动作
            channel_state[i].printer_blocked = true;
        }
    } else if (bus_stu == 1 && printer_bus_fault) {
        printer_bus_fault = false;
        for (int i = 0; i < active_channel_count(); i++) channel_state[i].printer_blocked = false;
        LOG_MAIN("[bus] 收到合法打印机帧，总线恢复，允许新的打印机动作");
    }
    float d = AS5600_get_distance_E();
    // 上一轮看到的当前耗材通道号；255 表示尚未初始化
    static int now_f = 255;
    const int x = get_now_filament_num();
    for (int i = 0; i < active_channel_count(); i++) {
        if (i != x && channel_state[i].motor.source == MOTOR_PRINTER && !channel_state[i].motor.timed)
            motorCmd(i, 0);
    }

    // 耗材槽切换处理
    if (now_f != x) {
        now_f = x;
        BambuBus_set_need_to_save(); // 设置需要保存
    }

    // 处理耗材运动状态（匹配进退料）
    const bool current_valid = is_active_channel(now_f);
    const auto state = current_valid ? get_filament_motion(now_f) : waiting;
    if (current_valid) {
        auto &ch = channel_state[now_f];
        if (state == need_pull_back) {
            // 进料延迟停止尚未到期时先继续进料，等延迟结束后再启动退料
            if (ch.previous_motion != need_pull_back && !ch.feed_stop_pending) {
                ch.previous_motion = need_pull_back;
                ch.pullback_start_meters = get_filament_meters(now_f);
                if (!motorCmd(now_f, -1, MOTOR_PRINTER, PULLBACK_TIMEOUT[now_f] * 1000u))
                    LOG_MAIN("[motor] 通道 %d 退料启动失败：通道被阻断或暂时不可用", now_f + 1);
            }
        } else if (state == need_send_out) {
            if (ch.feed_stop_pending) {
                clear_feed_stop_delay(now_f);
                LOG_MAIN("[motor] 通道 %d 打印机恢复进料，取消延迟停止", now_f + 1);
            }
            ch.previous_motion = need_send_out;
        } else if (state == waiting) {
            ch.previous_motion = waiting;
        }

        // 更新耗材长度
        if (state == need_send_out) {
            d = -d; // 进料时长度减少
        }
        if (!ch.printer_blocked) add_filament_meters(now_f, d);
    }

    BMCU_motion(); // 处理电机运动
}

/**
 * 电机运动逻辑
 * 根据当前状态控制电机；打印机明确的 07/00 停止供料跳过普通进料结束延迟
 */
void BMCU_motion() {
    if (printer_bus_fault) {
        for (int i = 0; i < active_channel_count(); i++) {
            if (channel_state[i].motor.source == MOTOR_PRINTER) motorCmd(i, 0);
        }
        return;
    }
    const int cur = get_now_filament_num();
    if (!is_active_channel(cur)) {
        for (int i = 0; i < active_channel_count(); i++) {
            const auto &m = channel_state[i].motor;
            if (m.source != MOTOR_PRINTER || m.timed) continue;
            if (!motors[i].getStopState()) motorCmd(i, 0);
        }
        return;
    }
    auto &ch = channel_state[cur];
    auto &m = ch.motor;
    if (BambuBus_filament_stop_requested(cur) && m.source == MOTOR_PRINTER && m.direction > 0) {
        clear_feed_stop_delay(cur);
        motorCmd(cur, 0);
        return;
    }
    // 打印机发起的定时退料不接受打印机后续状态干扰，必须由 Web 超时参数决定停止时间
    if (m.source == MOTOR_PRINTER && m.timed && m.direction < 0) {
        return;
    }

    const bool execute_motion = channel_switch_ready(cur);
    if (m.source == MOTOR_MANUAL) {
        if (get_filament_motion(cur) != waiting) motorCmd(cur, 1); // 拒绝占用中的通道，不延后执行
        return;
    }
    if (m.source == MOTOR_AUTOLOAD) {
        if (get_filament_motion(cur) == waiting) return;
        motorCmd(cur, 0);
    }

    auto act = get_filament_motion(cur);
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
            // 待机：打印机结束进料时先延迟停车，其他来源仍立即停止
            if (!m.timed) {
                if (ch.feed_stop_pending) {
                    // 已安排延迟，等待 motor_tick() 到期停止
                } else if (!arm_feed_stop_delay(cur)) {
                    motorCmd(cur, 0);
                }
            }
            switchInterrupt();
            break;
        case act_send_mm:
            // 执行进料（正转）
            if (ch.pullback_start_meters - get_filament_meters(cur) > ACT_TARGET_METERS) {
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
            if (get_filament_meters(cur) - ch.pullback_start_meters > ACT_TARGET_METERS) {
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
