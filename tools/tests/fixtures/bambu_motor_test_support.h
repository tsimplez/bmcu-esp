// 只模拟电机硬件与调度上下文，BMCU_motion 和延迟标记函数从 src/BMCU.cpp 原样提取。
enum MotorSource { MOTOR_NONE, MOTOR_PRINTER, MOTOR_MANUAL, MOTOR_AUTOLOAD };
struct TestMotorState { MotorSource source = MOTOR_NONE; int direction = 0; bool timed = false; };
struct TestChannelState {
    TestMotorState motor;
    bool feed_stop_pending = false;
    uint32_t feed_stop_deadline = 0;
    float pullback_start_meters = 0;
};
static TestChannelState channel_state[16];
struct TestMotor {
    bool stopped = true;
    /** 查询模拟电机是否停止 */
    bool getStopState() const { return stopped; }
};
static TestMotor motors[16];
static bool printer_bus_fault = false;
static constexpr uint32_t FEED_STOP_DELAY_MS = 1000;
static constexpr float ACT_TARGET_METERS = 0.05f;
static unsigned motor_commands = 0;
static int PULLBACK_TIMEOUT[16] = {};
struct TestQueuedJob { MotorSource source; };
static TestQueuedJob motor_queue[16];
/**
 * 本组停止测试不包含排队任务
 * @param channel 查询通道
 * @return 固定返回未排队
 */
static int queue_position(int channel) { return 0; }
/**
 * 提供队列删除边界，本组没有排队任务
 * @param index 队列位置
 */
static void remove_job(int index) {}
/** 检查固定容量范围 */
static bool valid_channel(int ch) { return ch >= 0 && ch < 16; }
/** 检查测试当前启用通道 */
static bool is_active_channel(int ch) { return ch >= 0 && ch < active_channel_count(); }
/** 本场景中通道微动就绪，不测试微动硬件 */
static bool channel_switch_ready(int) { return true; }
/**
 * 捕获主循环向电机发送的命令，不驱动物理引脚
 * @param ch 通道编号
 * @param direction 目标方向
 * @param source 动作来源
 * @param duration 动作时长
 * @return true 表示命令已捕获
 */
static bool motorCmd(int ch, int direction, MotorSource source = MOTOR_PRINTER, uint32_t duration = 0) {
    assert(valid_channel(ch));
    ++motor_commands;
    motors[ch].stopped = direction == 0;
    if (direction == 0) channel_state[ch].motor = {};
    else channel_state[ch].motor = {source, direction, duration > 0};
    return true;
}
/** 本场景不执行微动辅助动作 */
static void switchInterrupt() {}
