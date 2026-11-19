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
#include <cmath>
// Arduino Mbed 在 Arduino.h 中定义 abs 宏，须在其加载后清除，避免破坏 <random> 中的 std::abs。
#undef abs
#include <random>
#include "PCF8575_PinMap.h"
#include "settings_storage.h"
#include "log_bus.h"
#include "i2c_bus.h"
#include "BambuBus.h"
#include "lan_ams.h"

namespace {
constexpr uint32_t CHANNEL_SWITCH_DELAY_MS = 100; // 通道切换后的运动稳定期
constexpr uint32_t MSW_DEBOUNCE_MS = 30;          // 微动开关防抖时间
constexpr float ACT_TARGET_METERS = 0.0f;         // 兼容动作的目标行程

// 单个通道的电机运行信息，停止时整体清零
struct MotorRun {
    int direction = 0;                 // 1=进料，-1=退料，0=停止
    MotorSource source = MOTOR_IDLE;   // 本次动作来源
    uint32_t end = 0;                  // 限时动作截止 millis()
    uint32_t duration = 0;             // 本次动作时长，离合稳定后才开始计时
    bool timed = false;                // false 表示由打印机状态机持续控制
    bool pending_start = false;        // 已占用通道，等待离合释放或吸合稳定
};

// 一次等待执行的电机任务，打印机和手动共用队列
struct MotorJob {
    int channel;
    int direction;                     // 1=进料，-1=退料
    MotorSource source;
    uint32_t duration;                 // 持续时间（毫秒）
};

// 网页停止后的打印机进料恢复条件，仅由该通道的有效运动请求推进
enum class PrinterFeedState : uint8_t {
    allowed,
    wait_other_request,                // 先等待非进料动作，重复进料保持停止
    wait_feed_request                  // 已收到非进料动作，仍需新的进料请求才能恢复
};

// 单通道的电机、互锁和传感器运行状态
struct ChannelState {
    MotorRun motor;
    uint32_t feed_stop_deadline = 0;       // 延迟停车截止 millis()
    filament_motion_state_set previous_motion = waiting;
    float pullback_start_meters = 0.0f;    // 退料开始时的耗材长度
    uint32_t msw_raw_time = 0;             // 原始状态最近变化时间
    // 单字节字段集中放在末尾，减少结构体对齐填充
    PrinterFeedState printer_feed_state = PrinterFeedState::allowed;
    bool stop_requested = false;          // 网页提交停止请求，由主循环处理
    bool feed_stop_pending = false;       // 正在等待进料延迟停车
    int8_t msw_previous = -1;              // 防抖后的微动状态：-1 未知，0 无料，1 有料
    int8_t msw_raw = -1;                   // 防抖前的微动状态
};
}

// 所有电机调用均在 Arduino loop 中串行执行，不在 ISR 中驱动
// 每个通道的运行、互锁、延迟停车和微动状态集中保存在这里
static ChannelState channel_state[IO_CONFIG_MAX_CHANNELS];
// 打印机总线超时后禁止新的打印机动作，直到收到合法新帧
static bool printer_bus_fault = false;
// Web 只登记接线应用请求，由主循环释放旧引脚并绑定新引脚
static bool io_apply_pending = false;
// 五通总微动上次读数；-1 表示尚未建立有效输入基线
static int lastSwitchState = -1;
static bool total_switch_triggered = false; // 仅当前主循环有效的压下边沿，不排队补触发

// 等待启动的电机任务，按提交顺序执行，同一通道只保留一个目标动作
static MotorJob motor_queue[IO_CONFIG_MAX_CHANNELS];
// 电机队列中当前有效的任务数量
static int motor_queued = 0;

/**
 * 查询通道在电机队列中的位置
 * @param ch 通道编号
 * @return 从 1 开始的排队位置；未排队返回 0
 * 顺序扫描电机队列，供状态接口显示排队位置
 */
static int queue_position(const int ch) {
    for (int i = 0; i < motor_queued; i++) if (motor_queue[i].channel == ch) return i + 1;
    return 0;
}

/**
 * 从电机队列删除指定任务
 * @param index 队列下标
 * 把后续任务前移并减少队列长度
 */
static void remove_job(const int index) {
    for (int i = index; i + 1 < motor_queued; i++) motor_queue[i] = motor_queue[i + 1];
    --motor_queued;
}

/**
 * 新增或更新一个排队动作
 * @param channel 通道编号
 * @param direction 目标方向
 * @param source 动作来源
 * @param duration 动作时长，0 表示持续运行
 * @return true 表示请求已排队；队列已满时返回 false
 * 同一通道的新目标覆盖旧目标并保留排队位置，重复报文不会增加任务
 */
static bool queue_motor_job(const int channel, const int direction, const MotorSource source,
                            const uint32_t duration) {
    const int pos = queue_position(channel);
    if (!pos && motor_queued >= active_channel_count()) return false;
    const int index = pos ? pos - 1 : motor_queued++;
    motor_queue[index] = {channel, direction, source, duration};
    return true;
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
        case MOTOR_LAN: return "局域网换料";
        case MOTOR_TOTAL_SWITCH: return "五通总微动";
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
 * @return true 表示当前驱动模式所需的引脚完整且所属扩展芯片在线
 * 普通模式检查两个方向引脚；离合模式检查共享电机、本通道离合及所有启用离合的扩展芯片，避免旧离合无法释放时启动其他通道
 */
bool BMCU_channel_motor_ready(const int motorNum) {
    if (!valid_channel(motorNum)) return false;
    const bool clutch_enabled = IO_CONFIG.clutch_enabled != 0;
    const int forward_pin = clutch_enabled ? IO_CONFIG.shared_motor[0] : IO_CONFIG.motor[motorNum][0];
    const int backward_pin = clutch_enabled ? IO_CONFIG.shared_motor[1] : IO_CONFIG.motor[motorNum][1];
    if (forward_pin == PIN_NONE || backward_pin == PIN_NONE) return false;
    // 扩展引脚依赖 PCF8575；芯片未检测到时不能假装电机已就绪
    if ((IS_PCF8575_VIRTUAL_PIN(forward_pin) && !PCF8575_pin_is_online(forward_pin)) ||
        (IS_PCF8575_VIRTUAL_PIN(backward_pin) && !PCF8575_pin_is_online(backward_pin))) {
        return false;
    }
    if (clutch_enabled) {
        if (IO_CONFIG.clutch[motorNum] == PIN_NONE) return false;
        for (int ch = 0; ch < active_channel_count(); ++ch) {
            const int pin = IO_CONFIG.clutch[ch];
            if (IS_PCF8575_VIRTUAL_PIN(pin) && !PCF8575_pin_is_online(pin)) return false;
        }
    }
    return true;
}

/**
 * 启动排队中的电机动作
 * 在并发上限内依次启动队首；丢弃无效接线及已结束、换通道、缺料或被网页停止的打印机进料任务，局域网任务另按当前会话校验
 */
static void run_motor_queue() {
    while (motor_queued && BMCU_running_count() < BMCU_motor_limit()) {
        const auto job = motor_queue[0];
        if (!is_active_channel(job.channel)) {
            remove_job(0);
            continue;
        }
        // 该通道没有电机引脚：丢掉这次请求，别让它卡住后面的排队动作
        if (!BMCU_channel_motor_ready(job.channel)) {
            remove_job(0);
            continue;
        }
        if (job.source == MOTOR_LAN && !lan_ams_motor_allowed(job.channel)) {
            remove_job(0);
            continue;
        }
        if (job.source == MOTOR_PRINTER &&
            (printer_bus_fault || job.channel != get_now_filament_num() ||
             get_filament_motion(job.channel) == waiting ||
             (job.direction > 0 &&
              (channel_state[job.channel].printer_feed_state != PrinterFeedState::allowed ||
               !is_filament_online(job.channel))))) {
            remove_job(0);
            continue;
        }
        if (!motorCmd(job.channel, job.direction, job.source, job.duration)) break;
    }
}

// 电机对象数组；具体引脚由设置加载后在 BMCU_init()/BMCU_apply_io_config() 绑定
// 16 个通道的 Motor 实例，负责实际的 GPIO/PWM 输出
static Motor motors[IO_CONFIG_MAX_CHANNELS];
// 电磁离合模式只使用这一台电机，通道对象不绑定共享方向引脚
static Motor shared_motor;
// 已应用接线用于关闭旧输出，避免网页候选配置改变后错误操作新引脚
static IoConfig applied_io = {};
static int applied_channels = 0;

enum class ClutchPhase : uint8_t {
    idle,
    releasing,
    engaging,
    running
};

struct ClutchDrive {
    ClutchPhase phase = ClutchPhase::idle;
    int channel = -1;                  // 当前独占共享电机的通道，-1 表示没有所属通道
    int direction = 0;
    uint32_t deadline = 0;             // 释放或吸合稳定期的截止时间
};

static ClutchDrive clutch_drive;

/**
 * 释放已应用接线中的全部离合
 * 只写离合的断开电平；共享电机必须由调用方先关闭
 */
static void release_all_clutches() {
    const int inactive_level = applied_io.clutch_active_level == HIGH ? LOW : HIGH;
    for (int ch = 0; ch < applied_channels; ++ch)
        digitalWrite_PCF8575(applied_io.clutch[ch], inactive_level);
}

/**
 * 停止指定通道的离合驱动
 * @param channel 待停止的通道编号
 * 只有所属通道可以关闭共享电机，其他通道只释放自己的离合；停车后的释放稳定期继续由主循环处理
 */
static void stop_clutch_drive(const int channel) {
    if (clutch_drive.channel != channel) {
        digitalWrite_PCF8575(applied_io.clutch[channel], applied_io.clutch_active_level == HIGH ? LOW : HIGH);
        return;
    }
    shared_motor.stop();
    release_all_clutches();
    clutch_drive = {};
    clutch_drive.phase = ClutchPhase::releasing;
    clutch_drive.deadline = millis() + static_cast<uint32_t>(applied_io.clutch_release_ms);
}

/**
 * 预约一个通道的共享电机方向
 * @param channel 独占共享电机的通道编号
 * @param direction 目标方向，1 为进料，-1 为退料
 * 新动作和换向先停车并释放所有离合；已有停车释放期不重新计时，避免重复请求推迟启动
 */
static void request_clutch_drive(const int channel, const int direction) {
    const bool already_releasing = clutch_drive.phase == ClutchPhase::releasing;
    shared_motor.stop();
    release_all_clutches();
    if (!already_releasing)
        clutch_drive.deadline = millis() + static_cast<uint32_t>(applied_io.clutch_release_ms);
    clutch_drive.phase = ClutchPhase::releasing;
    clutch_drive.channel = channel;
    clutch_drive.direction = direction;
}

/**
 * 在主循环推进离合释放、吸合和共享电机启动
 * 每个阶段都重新检查扩展芯片；稳定期内保持电机关闭，实际启动后再开始限时动作的计时
 */
static void run_clutch_drive() {
    if (!applied_io.clutch_enabled || clutch_drive.phase == ClutchPhase::idle) return;
    const uint32_t now = millis();
    const int channel = clutch_drive.channel;
    if (channel >= 0 && !BMCU_channel_motor_ready(channel)) {
        LOG_MAIN("[motor] 通道 %d 离合接线不可用，关闭共享电机", channel + 1);
        if (lan_ams_busy()) lan_ams_cancel();
        BMCU_stop_motor(channel);
        return;
    }
    if (clutch_drive.phase == ClutchPhase::running ||
        static_cast<int32_t>(now - clutch_drive.deadline) < 0) return;
    if (clutch_drive.phase == ClutchPhase::releasing) {
        if (channel < 0) {
            clutch_drive = {};
            return;
        }
        digitalWrite_PCF8575(applied_io.clutch[channel], applied_io.clutch_active_level);
        clutch_drive.phase = ClutchPhase::engaging;
        clutch_drive.deadline = now + static_cast<uint32_t>(applied_io.clutch_engage_ms);
    }
    if (!BMCU_channel_motor_ready(channel)) {
        if (lan_ams_busy()) lan_ams_cancel();
        BMCU_stop_motor(channel);
        return;
    }
    if (clutch_drive.phase != ClutchPhase::engaging ||
        static_cast<int32_t>(now - clutch_drive.deadline) < 0) return;
    if (clutch_drive.direction > 0) shared_motor.forward();
    else shared_motor.backward();
    if (!BMCU_channel_motor_ready(channel)) {
        if (lan_ams_busy()) lan_ams_cancel();
        BMCU_stop_motor(channel);
        return;
    }
    auto &motor = channel_state[channel].motor;
    motor.pending_start = false;
    motor.end = millis() + motor.duration;
    clutch_drive.phase = ClutchPhase::running;
    LOG_MAIN("[motor] 通道 %d 离合已吸合，共享电机开始%s", channel + 1,
             motor_direction_name(clutch_drive.direction));
}

/**
 * 登记最新接线的应用请求
 * 供 Web 回调调用；只设置标记，由主循环操作引脚和电机
 */
void BMCU_apply_io_config() {
    io_apply_pending = true;
}

/**
 * 在主循环应用通道与五通总微动接线
 * 先释放所有变更输入和通道，再按接线切换控制台并绑定新引脚；离合模式、共享接线或离合接线变化时停止全部通道并清理排队，其余未变化通道保持运行
 * 总微动改线或切换压下电平时重新建立输入基线；离合新接线先保持全部断开，等待释放稳定期后才允许吸合
 */
static void apply_io_config() {
    static int applied_switch_active_level = -1;
    const int channels = active_channel_count();
    const bool clutch_mode_changed = applied_io.clutch_enabled != IO_CONFIG.clutch_enabled;
    bool clutch_changed = !applied_channels || clutch_mode_changed;
    if (applied_io.clutch_enabled || IO_CONFIG.clutch_enabled) {
        clutch_changed = clutch_changed || applied_channels != channels ||
                         applied_io.shared_motor[0] != IO_CONFIG.shared_motor[0] ||
                         applied_io.shared_motor[1] != IO_CONFIG.shared_motor[1] ||
                         applied_io.clutch_active_level != IO_CONFIG.clutch_active_level ||
                         applied_io.clutch_engage_ms != IO_CONFIG.clutch_engage_ms ||
                         applied_io.clutch_release_ms != IO_CONFIG.clutch_release_ms;
        for (int i = 0; i < IO_CONFIG_MAX_CHANNELS; ++i)
            if (applied_io.clutch[i] != IO_CONFIG.clutch[i]) clutch_changed = true;
    }
    const bool switch_changed = !applied_channels || applied_io.switch_pin != IO_CONFIG.switch_pin;
    if (switch_changed) {
        if (applied_channels && applied_io.switch_pin != PIN_NONE)
            pinMode_PCF8575(applied_io.switch_pin, INPUT);
    }
    if (switch_changed || applied_switch_active_level != TOTAL_SWITCH_ACTIVE_LEVEL) {
        lastSwitchState = -1;
        total_switch_triggered = false;
    }
    bool changed[IO_CONFIG_MAX_CHANNELS] = {};
    if (clutch_changed) {
        shared_motor.stop();
        if (applied_io.clutch_enabled) release_all_clutches();
    }
    // 先释放所有变更/停用通道，再绑定新引脚，避免引脚交换时旧通道拉低新输出
    for (int i = 0; i < IO_CONFIG_MAX_CHANNELS; i++) {
        changed[i] = clutch_changed || (i < applied_channels) != (i < channels) ||
                     (i < channels && ((!IO_CONFIG.clutch_enabled &&
                                        (applied_io.motor[i][0] != IO_CONFIG.motor[i][0] ||
                                         applied_io.motor[i][1] != IO_CONFIG.motor[i][1])) ||
                                       applied_io.microswitch[i] != IO_CONFIG.microswitch[i]));
        if (!changed[i]) continue;
        BMCU_stop_motor(i);
        motors[i].setPins(PIN_NONE, PIN_NONE);
        if (i < applied_channels && applied_io.microswitch[i] != PIN_NONE)
            pinMode_PCF8575(applied_io.microswitch[i], INPUT);
        auto &ch = channel_state[i];
        ch.previous_motion = waiting;
        ch.msw_previous = ch.msw_raw = -1;
        ch.msw_raw_time = 0;
    }
    if (clutch_changed) shared_motor.setPins(PIN_NONE, PIN_NONE);
    hal_console_begin(); // 旧通道已释放，新通道接管引脚前完成控制台切换
    for (int i = 0; i < channels; i++) {
        if (!changed[i]) continue;
        if (IO_CONFIG.clutch_enabled) {
            const int inactive_level = IO_CONFIG.clutch_active_level == HIGH ? LOW : HIGH;
            PCF8575_set_idle_level(IO_CONFIG.clutch[i], inactive_level);
            digitalWrite_PCF8575(IO_CONFIG.clutch[i], inactive_level);
            pinMode_PCF8575(IO_CONFIG.clutch[i], OUTPUT);
            digitalWrite_PCF8575(IO_CONFIG.clutch[i], inactive_level);
        } else {
            motors[i].setPins(IO_CONFIG.motor[i][0], IO_CONFIG.motor[i][1]);
        }
        const int pin = IO_CONFIG.microswitch[i];
        if (pin != PIN_NONE)
            pinMode_PCF8575(pin, IS_PCF8575_VIRTUAL_PIN(pin) ? INPUT : INPUT_PULLUP);
    }
    if (switch_changed && IO_CONFIG.switch_pin != PIN_NONE)
        pinMode_PCF8575(IO_CONFIG.switch_pin,
                       IS_PCF8575_VIRTUAL_PIN(IO_CONFIG.switch_pin) ? INPUT : INPUT_PULLUP);
    applied_io = IO_CONFIG;
    applied_channels = channels;
    if (clutch_changed) {
        clutch_drive = {};
        if (IO_CONFIG.clutch_enabled) {
            shared_motor.setPins(IO_CONFIG.shared_motor[0], IO_CONFIG.shared_motor[1]);
            clutch_drive.phase = ClutchPhase::releasing;
            clutch_drive.deadline = millis() + static_cast<uint32_t>(IO_CONFIG.clutch_release_ms);
        }
    }
    applied_switch_active_level = TOTAL_SWITCH_ACTIVE_LEVEL;
    io_apply_pending = false;
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
 * 检查共用队列中的手动任务和所有手动来源的电机
 */
bool BMCU_manual_active() {
    for (int i = 0; i < motor_queued; i++) if (motor_queue[i].source == MOTOR_MANUAL) return true;
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
 * 在线模式直接标记有料；自动模式读取微动后检查所属芯片状态，离线或读取失败时标记无料；其余模式标记无料
 */
void BMCU_apply_channel_modes() {
    for (int i = 0; i < IO_CONFIG_MAX_CHANNELS; i++) {
        const bool enabled = i < active_channel_count();
        bool online_now = enabled && CHANNEL_MODE[i] == CHANNEL_MODE_ONLINE;
        if (enabled && CHANNEL_MODE[i] == CHANNEL_MODE_AUTO &&
            IO_CONFIG.microswitch[i] != PIN_NONE) {
            int pin = IO_CONFIG.microswitch[i];
            online_now = !digitalRead_PCF8575(pin) &&
                         (!IS_PCF8575_VIRTUAL_PIN(pin) || PCF8575_pin_is_online(pin));
        }
        set_filament_online(i / 4, i % 4, online_now);
    }
}

/**
 * 判断当前是否允许维护操作
 * @return true 表示可以安全修改配置或升级
 * 要求没有电机排队或运行、离合释放稳定期已结束、所有通道处于停止且打印机状态为 waiting；局域网换料等待确认时也禁止维护
 */
bool BMCU_can_maintain() {
    if (lan_ams_busy()) return false;
    // 手动维护不再因为打印机总线近期有通信而被拒绝，只检查本地运动状态
    if (BMCU_running_count() || motor_queued) return false;
    if (applied_io.clutch_enabled && clutch_drive.phase != ClutchPhase::idle) return false;
    for (int i = 0; i < active_channel_count(); i++) {
        if (!motors[i].getStopState() || get_filament_motion(i) != waiting) return false;
    }
    return true;
}

/**
 * 统计正在运行的电机数量
 * @return 当前方向非零的通道数，包含等待离合稳定的预约动作
 * 用于限制同时运行的电机数量，离合释放和吸合阶段同样独占运行名额
 */
int BMCU_running_count() {
    int count = 0;
    for (const auto &ch: channel_state) count += ch.motor.direction != 0;
    return count;
}

/**
 * 获取当前驱动模式的有效并发上限
 * @return 离合模式固定为 1，普通模式使用已保存的电机并发上限
 * 保留普通模式的用户设置，模式切换后可恢复原上限
 */
int BMCU_motor_limit() {
    return IO_CONFIG.clutch_enabled ? 1 : MOTOR_LIMIT;
}

/**
 * 查询打印机总线故障状态
 * @return true 表示当前通信或换料故障已阻止打印机动作
 * 有线模式返回最近一次总线合法性判断，局域网模式返回当前连接及换料故障
 */
bool BMCU_printer_bus_fault() {
    if (lan_ams_enabled()) return lan_ams_status().fault;
    return printer_bus_fault;
}

/**
 * 读取通道电机完整状态
 * @param motorNum 通道编号
 * @return 包含方向、来源、剩余时间及排队位置、方向和来源的 MotorStatus
 * 越界通道返回默认空闲状态；等待离合稳定时保留目标方向和完整动作时长，防止上层把尚未启动的限时动作视为完成
 */
MotorStatus BMCU_motor_status(const int motorNum) {
    if (!is_active_channel(motorNum)) return {};
    const auto &ch = channel_state[motorNum];
    const auto &m = ch.motor;
    int32_t remaining_ms = 0;
    if (m.direction) {
        if (!m.timed) {
            remaining_ms = -1;
        } else if (m.pending_start) {
            remaining_ms = static_cast<int32_t>(m.duration);
        } else {
            const auto left = static_cast<int32_t>(m.end - millis());
            remaining_ms = left > 0 ? left : 0;
        }
    }
    const int pos = queue_position(motorNum);
    return {m.direction, m.source, remaining_ms, pos,
            pos ? motor_queue[pos - 1].direction : 0, pos ? motor_queue[pos - 1].source : MOTOR_IDLE};
}

/**
 * 处理打印机通道空闲通知
 * @param motorNum 通道编号
 * 收到打印机 idle 后取消尚未启动的打印机任务，并把上一次动作恢复为 waiting
 */
void BMCU_printer_idle(const int motorNum) {
    if (!is_active_channel(motorNum)) return;
    auto &ch = channel_state[motorNum];
    if (ch.motor.source == MOTOR_PRINTER && ch.motor.timed && ch.motor.direction < 0) {
        // 退料由 Web 超时独立控制，打印机提前空闲也必须跑满设定时间
        LOG_MAIN("[motor] 通道 %d 打印机已空闲，但退料定时未到，继续运行", motorNum + 1);
    }
    const int pos = queue_position(motorNum);
    if (pos && motor_queue[pos - 1].source == MOTOR_PRINTER) remove_job(pos - 1);
    ch.previous_motion = waiting;
}

/**
 * 登记打印机对指定通道的有效运动请求
 * @param motorNum 通道编号
 * @param feeding true 表示请求进料，false 表示已接受的非进料动作
 * 网页停止后先等待非进料动作，再等待新的进料请求；只推进恢复条件，不驱动电机或改写协议状态
 * 普通轮询、未知组合、其他通道请求和本地停车不应调用本通道的通知
 */
void BMCU_printer_motion_requested(const int motorNum, const bool feeding) {
    if (!is_active_channel(motorNum)) return;
    if (auto &state = channel_state[motorNum].printer_feed_state; state == PrinterFeedState::wait_other_request && !feeding) {
        state = PrinterFeedState::wait_feed_request;
        LOG_MAIN("[motor] 通道 %d 已收到非进料动作，等待新的打印机进料请求", motorNum + 1);
    } else if (state == PrinterFeedState::wait_feed_request && feeding) {
        state = PrinterFeedState::allowed;
        LOG_MAIN("[motor] 通道 %d 已收到新的打印机进料请求，允许恢复进料", motorNum + 1);
    }
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
 * 仅对打印机来源、正转且未设定时长的进料生效；使用当前配置，0 表示不延迟
 * 已安排的延迟不随配置变更或重复待机帧延长，新的设置从下一次安排开始生效
 */
static bool arm_feed_stop_delay(const int ch) {
    if (!valid_channel(ch) || channel_state[ch].feed_stop_pending || FEED_STOP_DELAY_MS <= 0) return false;
    const auto &m = channel_state[ch].motor;
    if (m.source != MOTOR_PRINTER || m.direction <= 0 || m.timed) return false;
    channel_state[ch].feed_stop_pending = true;
    channel_state[ch].feed_stop_deadline = millis() + FEED_STOP_DELAY_MS;
    LOG_MAIN("[motor] 通道 %d 打印机请求停止进料，保持运行 %d ms 后停止", ch + 1,
             FEED_STOP_DELAY_MS);
    return true;
}

/**
 * 停止指定通道的本地运动并清理排队
 * @param motorNum 通道编号
 * 移除排队任务、清除停止请求、电机计时、延迟停车和本地动作历史；保留协议动作、回复字段及进料恢复条件
 */
static void stop_local_motor(const int motorNum) {
    if (!valid_channel(motorNum)) return;
    auto &ch = channel_state[motorNum];
    ch.stop_requested = false;
    const int pos = queue_position(motorNum);
    if (pos) remove_job(pos - 1);
    motorCmd(motorNum, 0);
    ch.previous_motion = waiting;
    ch.pullback_start_meters = 0.0f;
}

/**
 * 停止指定通道并清理排队及协议动作
 * @param motorNum 通道编号
 * 用于接线变更、故障和局域网会话结束，将协议动作恢复为 waiting；保留网页停止后的进料恢复条件
 */
void BMCU_stop_motor(const int motorNum) {
    if (!valid_channel(motorNum)) return;
    stop_local_motor(motorNum);
    set_filament_motion(motorNum, waiting);
}

/**
 * 提交通道停止请求
 * @param motorNum 通道编号
 * 只设置标记，由主循环停止本地运动并清理排队；打印机进料需先收到非进料动作再收到新的进料请求才能恢复
 */
void BMCU_request_stop_motor(const int motorNum) {
    if (!is_active_channel(motorNum)) return;
    channel_state[motorNum].stop_requested = true;
}

/**
 * 提交一次手动点动请求
 * @param motorNum 通道编号
 * @param cmd 1=进料，-1=退料
 * @param duration_ms 点动时长（毫秒）
 * @return true 表示已加入电机队列，等待主循环启动
 * 检查本地运行、停止请求、排队和引脚状态，局域网换料期间拒绝点动；网页停止后的重复打印机进料不接管点动，有效的新动作仍可接管
 */
bool BMCU_manual_pulse(const int motorNum, const int cmd, const uint32_t duration_ms) {
    if (lan_ams_busy()) return false;
    if (!is_active_channel(motorNum) || (cmd != 1 && cmd != -1) || !duration_ms ||
        duration_ms >= MAX_PULSE_MS)
        return false;
    const auto &ch = channel_state[motorNum];
    if (ch.motor.direction || ch.stop_requested || queue_position(motorNum) ||
        !BMCU_channel_motor_ready(motorNum))
        return false;
    return queue_motor_job(motorNum, cmd, MOTOR_MANUAL, duration_ms);
}

/**
 * 统一控制电机启停
 * @param motorNum 通道编号
 * @param cmd 1=正转，-1=反转，0=停止
 * @param source 动作来源
 * @param duration_ms 限时动作时长，0 表示不自动停止
 * @return true 表示命令已执行或已排队
 * 打印机可接管本通道；同方向不重复驱动，同来源重复命令不续期；普通电机直接换向，离合模式先停车、释放并重新吸合后换向
 * 并发名额不足时通道和五通微动辅助均拒绝，其他来源按通道合并排队并在启动后计时；故障总线不能启动打印机任务，局域网任务仅允许当前会话通道
 * 网页停止后的打印机进料在恢复条件满足前拒绝启动或排队，手动进退料不受此限制；离合稳定期独占共享电机，限时动作从实际转动后计时
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
        if (applied_io.clutch_enabled) stop_clutch_drive(motorNum);
        else motors[motorNum].stop();
        m = {};
        if (was_active) {
            LOG_MAIN("[motor] 通道 %d 停止，原来源=%s，原方向=%s", motorNum + 1,
                     motor_source_name(old_source), motor_direction_name(old_direction));
        }
        return true;
    }
    if (!is_active_channel(motorNum)) return false;
    if (!BMCU_channel_motor_ready(motorNum)) return false;
    if (source == MOTOR_LAN && (!lan_ams_enabled() || !lan_ams_motor_allowed(motorNum) ||
                               CHANNEL_MODE[motorNum] == CHANNEL_MODE_OFFLINE)) return false;
    if (lan_ams_enabled() && source == MOTOR_PRINTER) return false;
    if (lan_ams_busy() && source != MOTOR_LAN) return false;
    if (source == MOTOR_PRINTER && printer_bus_fault) return false;
    if (source == MOTOR_PRINTER && cmd > 0 && ch.printer_feed_state != PrinterFeedState::allowed)
        return false;
    const int pos = queue_position(motorNum);
    if (source != MOTOR_PRINTER &&
        ((m.direction && m.source != source) || (pos && motor_queue[pos - 1].source != source))) return false;
    if (!m.direction && (BMCU_running_count() >= BMCU_motor_limit() || (motor_queued && pos != 1))) {
        if (source == MOTOR_AUTOLOAD || source == MOTOR_TOTAL_SWITCH) return false;
        return queue_motor_job(motorNum, cmd, source, duration_ms);
    }
    if (pos) remove_job(pos - 1);
    if (m.direction == cmd && m.source == source) return true; // 重复命令不续期
    const bool same_direction = m.direction == cmd;
    clear_feed_stop_delay(motorNum);
    m.direction = cmd;
    m.source = source;
    m.end = millis() + duration_ms;
    m.duration = duration_ms;
    m.timed = duration_ms > 0;
    if (!same_direction) {
        if (applied_io.clutch_enabled) {
            m.pending_start = true;
            request_clutch_drive(motorNum, cmd);
        } else {
            m.pending_start = false;
            if (cmd > 0) motors[motorNum].forward();
            else motors[motorNum].backward();
        }
    }
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
 * 仅更新打印机来源的限时动作，并按已经运行的时间重算剩余时间；等待离合稳定的动作尚未计时，直接更新完整时长，其他来源不受影响
 */
void BMCU_apply_pullback_timeout(const int motorNum, const uint32_t previous_timeout_ms) {
    if (!is_active_channel(motorNum)) return;
    auto &m = channel_state[motorNum].motor;
    if (!m.direction || !m.timed || m.source != MOTOR_PRINTER) return;

    const uint32_t new_timeout_ms = static_cast<uint32_t>(PULLBACK_TIMEOUT[motorNum]) * 1000u;
    if (m.pending_start) {
        m.duration = new_timeout_ms;
        return;
    }
    const uint32_t now = millis();
    const int32_t delta = static_cast<int32_t>(m.end - now);
    const uint32_t left = delta > 0 ? static_cast<uint32_t>(delta) : 0;
    const uint32_t elapsed = previous_timeout_ms > left ? previous_timeout_ms - left : 0;
    const uint32_t remaining = new_timeout_ms > elapsed ? new_timeout_ms - elapsed : 0;
    m.end = now + remaining;
    LOG_MAIN("[motor] 通道 %d 退料超时更新为 %d 秒，当前动作剩余 %lu ms", motorNum + 1,
             PULLBACK_TIMEOUT[motorNum], static_cast<unsigned long>(remaining));
}

/**
 * 处理停止请求、失效电机接线和限时动作
 * 先处理网页停止请求和失效接线，局域网换料期间同时取消整个会话，再停止已到期的限时动作；队列等本轮打印机状态处理完成后再启动
 * 网页停止保留协议动作及回复，打印机运行或排队进料先锁定恢复条件，避免同一轮及后续重复报文重启电机
 * 扩展芯片离线时停止受影响的运行通道并清理排队，不保留等待重连的旧动作
 */
static void motor_tick() {
    const uint32_t now = millis();
    for (int i = 0; i < active_channel_count(); i++) {
        auto &ch = channel_state[i];
        auto &m = ch.motor;
        if (ch.stop_requested) {
            if (lan_ams_busy()) lan_ams_cancel();
            const int pos = queue_position(i);
            const auto act = get_filament_motion(i);
            if (!lan_ams_enabled() && ch.printer_feed_state == PrinterFeedState::allowed &&
                (act == need_send_out || act == act_send_mm ||
                 (m.source == MOTOR_PRINTER && m.direction > 0) ||
                 (pos && motor_queue[pos - 1].source == MOTOR_PRINTER && motor_queue[pos - 1].direction > 0))) {
                ch.printer_feed_state = PrinterFeedState::wait_other_request;
                LOG_MAIN("[motor] 通道 %d 网页停止打印机进料，等待非进料动作及新的进料请求", i + 1);
            }
            stop_local_motor(i);
            continue;
        }
        if ((m.direction || queue_position(i)) && !BMCU_channel_motor_ready(i)) {
            LOG_MAIN("[motor] 通道 %d 电机接线不可用，停止并清理排队", i + 1);
            if (lan_ams_busy()) lan_ams_cancel();
            BMCU_stop_motor(i);
            continue;
        }
        // 打印机停止进料后先保持运行；动作失效时直接清除，避免残留延时
        if (ch.feed_stop_pending) {
            if (m.source != MOTOR_PRINTER || m.direction <= 0 || m.timed) {
                clear_feed_stop_delay(i);
            } else if (static_cast<int32_t>(now - ch.feed_stop_deadline) >= 0) {
                LOG_MAIN("[motor] 通道 %d 进料停止延迟到期，停止电机", i + 1);
                motorCmd(i, 0);
            }
        }
        if (!m.direction || !m.timed || m.pending_start) continue;
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

/**
 * 在主循环采样五通总微动，按配置的压下电平检测边沿
 * 不依赖打印机或电机状态；未配置、扩展器离线或读取失败时清除基线，首次有效读数只建立基线
 * 边沿只保留到本轮结束，避免换料或电机忙时的压下在之后补触发
 */
static void sample_total_switch() {
    total_switch_triggered = false;
    const int pin = IO_CONFIG.switch_pin;
    if (pin == PIN_NONE || (IS_PCF8575_VIRTUAL_PIN(pin) && !PCF8575_pin_is_online(pin))) {
        lastSwitchState = -1;
        return;
    }
    const int switchState = digitalRead_PCF8575(pin);
    if (IS_PCF8575_VIRTUAL_PIN(pin) && !PCF8575_pin_is_online(pin)) {
        lastSwitchState = -1;
        return;
    }
    total_switch_triggered = lastSwitchState >= 0 && lastSwitchState != switchState &&
                             switchState == TOTAL_SWITCH_ACTIVE_LEVEL;
    lastSwitchState = switchState;
}

/**
 * 获取五通总微动最近一次采样的压下状态
 * @return 1 表示已压下，0 表示未压下，-1 表示输入不可用或等待采样
 * 只读取缓存；改线或电平尚未应用、扩展器离线时不返回旧状态
 */
int BMCU_total_switch_state() {
    if (const int pin = IO_CONFIG.switch_pin; io_apply_pending || pin == PIN_NONE || lastSwitchState < 0 ||
                                              (IS_PCF8575_VIRTUAL_PIN(pin) && !PCF8575_pin_is_online(pin))) return -1;
    return lastSwitchState == TOTAL_SWITCH_ACTIVE_LEVEL ? 1 : 0;
}

/**
 * 在运动调度允许时响应五通总微动的压下边沿，按配置时长进料
 * 只触发当前有料且空闲的通道，保留并发和接线保护；局域网使用已确认且状态新鲜的装料通道，换料期间不触发
 * 本函数由主循环调用，不是硬件中断回调；五通动作独立于通道自吸时长
 */
static void switchInterrupt() {
    if (!total_switch_triggered || lan_ams_busy()) return;
    total_switch_triggered = false;
    const int n = lan_ams_enabled() ? lan_ams_active_channel() : get_now_filament_num();
    if (is_active_channel(n) && is_filament_online(n) && !channel_state[n].motor.direction)
        motorCmd(n, 1, MOTOR_TOTAL_SWITCH, TOTAL_SWITCH_FEED_MS);
}

/**
 * 判断通道是否禁止微动辅助进料
 * @param motorNum 通道编号
 * @return true 表示当前不能触发辅助进料
 * 非自动模式、无微动输入、正被手动或打印机占用以及局域网换料期间返回 true
 */
static bool autoload_blocked(const int motorNum) {
    if (lan_ams_busy()) return true;
    if (!is_active_channel(motorNum)) return true;
    if (CHANNEL_MODE[motorNum] != CHANNEL_MODE_AUTO) return true; // 仅自动模式触发
    if (IO_CONFIG.microswitch[motorNum] == PIN_NONE) return true; // 无真实微动输入
    if (channel_state[motorNum].motor.source == MOTOR_MANUAL) return true;
    if (get_filament_motion(motorNum) != waiting) return true; // 打印机正在占用该通道
    return false;
}

/**
 * 检测微动开关并触发自动进料
 * 轮询各通道微动状态，读取后检查所属扩展芯片状态并做 30ms 防抖；自动模式下由无料变有料触发辅助进料
 * 离线或读取失败时清除有料及防抖状态，恢复后的首次读数只建立基线；五通进料不受通道自吸设置影响，通道无料时仍停止
 */
void check_microswitch_status() {
    const uint32_t now = millis();

    for (int i = 0; i < active_channel_count(); i++) {
        auto &ch = channel_state[i];
        if (ch.motor.source == MOTOR_AUTOLOAD && (autoload_blocked(i) || AUTO_LOAD_MS <= 0))
            motorCmd(i, 0);
        if (ch.motor.source == MOTOR_TOTAL_SWITCH && !is_filament_online(i)) motorCmd(i, 0);

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
        // sw 微动高低电平 默认拉低是在线；离线读取不访问总线
        const bool sw = digitalRead_PCF8575(pin);
        // 读取本身也可能发现断线，不能把失败返回的低电平当作有料
        if (IS_PCF8575_VIRTUAL_PIN(pin) && !PCF8575_pin_is_online(pin)) {
            ch.msw_previous = ch.msw_raw = -1;
            set_filament_online(i / 4, i % 4, false);
            continue;
        }

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
            !autoload_blocked(i) && !ch.motor.direction) {
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
 * 初始化四片扩展 IO、通道和总微动引脚、传感器和通信，逐地址记录检测结果
 */
void BMCU_init() {
    // 初始化 PCF8575 虚拟引脚映射；I2C 未配置时不访问总线
    if (i2c_bus_configured()) {
        PCF8575_init();
        for (uint8_t device = 0; device < PCF8575_DEVICE_COUNT; ++device)
            LOG_MAIN("[i2c] PCF8575 0x%02X=%s", PCF8575_ADDR + device,
                     PCF8575_is_online(device) ? "已检测到" : "未检测到");
    }

    apply_io_config(); // 根据已加载设置绑定并初始化 IO 引脚
    AS5600_init(); // 初始化AS5600传感器

    BambuBus_init(); // 初始化BambuBus通信
    delay(500); // 延迟500ms

    BMCU_apply_channel_modes();

    LOG_MAIN("BMCU 初始化完成：通道数=%d，AMS 数量=%d", active_channel_count(), AMS_COUNT);
}

/**
 * BMCU主运行逻辑
 * 先应用待处理接线并探测扩展芯片，持续采样五通状态，再处理停止请求、打印机目标和共用队列；有线总线故障取消打印机运行及排队，保留本地手动任务
 * 双空接线时由局域网会话提供换料目标，跳过有线状态机，仍使用共用定时和队列保护；所有动作提交完成后推进非阻塞离合切换
 */
void BMCU_run() {
    if (io_apply_pending) apply_io_config();
    PCF8575_poll();
    sample_total_switch();
    motor_tick();
    if (lan_ams_enabled()) {
        lan_ams_run();
        switchInterrupt();
        run_motor_queue();
        run_clutch_drive();
        return;
    }
    const int bus_stu = BambuBus_run();
    if (bus_stu < 0) {
        if (!printer_bus_fault)
            LOG_MAIN("[bus] 打印机总线超过 1 秒未收到合法帧，停止打印机动作");
        printer_bus_fault = true;
        for (int i = 0; i < active_channel_count(); i++) {
            if (channel_state[i].motor.source == MOTOR_PRINTER)
                BMCU_stop_motor(i);
            else if (get_filament_motion(i) != waiting) {
                set_filament_motion(i, waiting);
                channel_state[i].previous_motion = waiting;
            }
            const int pos = queue_position(i);
            if (pos && motor_queue[pos - 1].source == MOTOR_PRINTER) remove_job(pos - 1);
        }
    } else if (bus_stu == 1 && printer_bus_fault) {
        printer_bus_fault = false;
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
            if (ch.previous_motion != need_pull_back) {
                ch.previous_motion = need_pull_back;
                ch.pullback_start_meters = get_filament_meters(now_f);
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
        if (ch.motor.source == MOTOR_PRINTER && ch.motor.direction && !ch.motor.pending_start)
            add_filament_meters(now_f, d);
    }

    BMCU_motion(); // 处理电机运动
    run_motor_queue();
    run_clutch_drive();
}

/**
 * 电机运动逻辑
 * 按打印机目标方向接管或切换电机；重复请求保持运行，达到当前驱动模式的有效并发上限时排队；离合稳定中的预约动作同样参与停止和接管
 * 网页停止后的重复进料跳过整个进料分支，保持停止且不接管或中断手动点动
 * 明确的 07/00 停止供料跳过普通进料结束延迟；定时退料仅可被在线通道的新进料请求切换，待机时保留手动、通道和五通微动的限时动作
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
            if (channel_state[i].motor.direction || !motors[i].getStopState()) motorCmd(i, 0);
        }
        return;
    }
    auto &ch = channel_state[cur];
    auto &m = ch.motor;
    auto act = get_filament_motion(cur);
    const int pos = queue_position(cur);
    if (act == waiting && pos && motor_queue[pos - 1].source == MOTOR_PRINTER) remove_job(pos - 1);
    if ((act == need_send_out || act == act_send_mm) && ch.printer_feed_state != PrinterFeedState::allowed)
        return;
    if (BambuBus_filament_stop_requested(cur) && m.source == MOTOR_PRINTER && m.direction > 0) {
        clear_feed_stop_delay(cur);
        motorCmd(cur, 0);
        return;
    }
    // 待机和重复退料保持原截止时间，离线期间的进料重试不得打断已开始的退料。
    if (m.source == MOTOR_PRINTER && m.timed && m.direction < 0 &&
        (act != need_send_out || !is_filament_online(cur))) {
        return;
    }

    const bool execute_motion = channel_switch_ready(cur) || m.direction != 0;
    if (act == waiting && (m.source == MOTOR_MANUAL || m.source == MOTOR_AUTOLOAD || m.source == MOTOR_TOTAL_SWITCH)) return;

    switch (act) {
        case need_pull_back:
            clear_feed_stop_delay(cur);
            motorCmd(cur, -1, MOTOR_PRINTER, PULLBACK_TIMEOUT[cur] * 1000u);
            break;
        case need_send_out:
            // 待进料：执行进料
            if (execute_motion && is_filament_online(cur)) {
                motorCmd(cur, 1); // 进料
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
            // 执行进料
            if (ch.pullback_start_meters - get_filament_meters(cur) > ACT_TARGET_METERS) {
                motorCmd(cur, 0); // 达到目标→停止
                act = waiting;
            } else if (execute_motion && is_filament_online(cur)) {
                motorCmd(cur, 1); // 继续进料
            } else {
                motorCmd(cur, 0); // 停止
            }
            break;
        case act_pull_mm:
            // 执行退料
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
