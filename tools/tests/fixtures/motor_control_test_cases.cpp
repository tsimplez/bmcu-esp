/** 重置硬件桩和运动状态，保留真实协议收发及调度实现 */
static void reset_motor_fixture() {
    AMS_COUNT = 4;
    reset_motion_fixture();
    pause_test = PauseTest();
    BambuBus_need_to_save = false;
    fake_ms += 200;
    printer_bus_fault = false;
    motor_queued = 0;
    MOTOR_LIMIT = 1;
    FEED_STOP_DELAY_MS = 1000;
    IO_CONFIG.switch_pin = PIN_NONE;
    io_apply_pending = false;
    lastSwitchState = -1;
    switch_level = 1;
    input_reads.clear();
    pin_modes.clear();
    input_levels.clear();
    expander_online = true;
    for (int i = 0; i < active_channel_count(); ++i) {
        channel_state[i] = {};
        motors[i].stopped = true;
        IO_CONFIG.motor[i][0] = 1;
        IO_CONFIG.motor[i][1] = 2;
        IO_CONFIG.microswitch[i] = PIN_NONE;
        PULLBACK_TIMEOUT[i] = 7;
        CHANNEL_MODE[i] = CHANNEL_MODE_ONLINE;
        set_filament_online(i / 4, i % 4, true);
    }
    motor_outputs = 0;
}

/**
 * 调用真实网页电机处理器
 * @param channel 全局通道
 * @param direction 方向或停止
 * @param duration 点动毫秒数
 */
static void motor_http(int channel, int direction, int duration = 300) {
    server.args = {{"motor", std::to_string(channel)}, {"dir", std::to_string(direction)},
                   {"duration", std::to_string(duration)}};
    server.status = 0;
    handle_manual_motor();
}

/**
 * 经真实协议提交退料目标
 * @param command Cxx 或 Dxx 命令
 * @param channel 已选中的全局通道
 * 准备、退料请求使用协议要求的顺序，不在收发处理器中运行电机
 */
static void request_pullback(uint8_t command, int channel) {
    request_motion(command, channel / 4, channel % 4, 9, 0xa5);
    request_motion(command, channel / 4, channel % 4, 9, 0x3f);
    assert(get_filament_motion(channel) == need_pull_back);
}

/** 验证可配置进料停止延迟、零延迟、重复帧、配置变更及 millis 回绕 */
static void test_feed_stop_delay() {
    for (uint8_t command : {3, 4}) for (uint32_t start : {1000u, UINT32_MAX - 200u})
    for (int duration : {0, 1, 1234, 29999}) {
        reset_motor_fixture();
        fake_ms = start;
        FEED_STOP_DELAY_MS = duration;
        request_motion(command, 0, 0, 3, 0);
        assert(motorCmd(0, 1, MOTOR_PRINTER));
        request_motion(command, 0, 0, 9, 0xa5);
        BMCU_motion();
        if (!duration) {
            assert(!channel_state[0].feed_stop_pending && motors[0].stopped);
            continue;
        }
        const uint32_t deadline = fake_ms + duration;
        assert(channel_state[0].feed_stop_pending && channel_state[0].feed_stop_deadline == deadline);
        FEED_STOP_DELAY_MS = 0; // 已开始的延迟保留原截止时间
        BMCU_motion();
        assert(channel_state[0].feed_stop_deadline == deadline);
        fake_ms = deadline - 1;
        motor_tick();
        assert(!motors[0].stopped);
        fake_ms = deadline;
        motor_tick();
        assert(motors[0].stopped && !channel_state[0].feed_stop_pending);
    }
}

/** 验证总微动主循环应用、真实/扩展输入、下降沿、离线恢复与禁用保护 */
static void test_switch_input() {
    for (int pin : {9, 66}) {
        reset_motor_fixture();
        IO_CONFIG.switch_pin = pin;
        BMCU_apply_io_config();
        assert(io_apply_pending && pin_modes.empty() && motor_outputs == 0);
        BMCU_run();
        assert(!io_apply_pending && pin_modes[pin] == (pin == 9 ? INPUT_PULLUP : INPUT));
        // 先经真实协议选中通道，再保持待机以测试总微动。
        request_motion(4, 0, 0, 3, 0);
        set_filament_motion(0, waiting);
        switch_level = 0;
        switchInterrupt();
        assert(motors[0].stopped); // 初次低电平不算新触发
        switch_level = 1;
        switchInterrupt();
        switch_level = 0;
        switchInterrupt();
        assert(!motors[0].stopped && channel_state[0].motor.source == MOTOR_AUTOLOAD);
        assert(channel_state[0].motor.end == fake_ms + 200);
        const auto outputs = motor_outputs;
        switchInterrupt();
        assert(outputs == motor_outputs);
        fake_ms += 200;
        motor_tick();
        assert(motors[0].stopped);
        if (pin != 9) {
            expander_online = false;
            const auto reads = input_reads.size();
            switchInterrupt();
            assert(input_reads.size() == reads && lastSwitchState == -1);
            expander_online = true;
            switchInterrupt();
            assert(motors[0].stopped);
        }
        IO_CONFIG.switch_pin = PIN_NONE;
        BMCU_apply_io_config();
        BMCU_run();
        assert(pin_modes[pin] == INPUT);
        const auto reads = input_reads.size();
        switchInterrupt();
        assert(input_reads.size() == reads && motors[0].stopped);
    }
}

/** 验证 Cxx/Dxx 接管、同向保持、反向切换和重复退料不续期，覆盖跨 AMS 通道 */
static void test_printer_target() {
    for (uint8_t command : {3, 4}) for (int channel : {0, 6, 15})
    for (MotorSource source : {MOTOR_MANUAL, MOTOR_AUTOLOAD})
    for (int initial : {-1, 1}) for (int target : {-1, 1}) {
        reset_motor_fixture();
        assert(motorCmd(channel, initial, source, 20000));
        const auto before = motor_outputs;
        request_motion(command, channel / 4, channel % 4, 3, 0);
        if (target < 0) request_pullback(command, channel);
        assert(motor_outputs == before);
        BMCU_run();
        auto state = BMCU_motor_status(channel);
        assert(state.direction == target && state.source == MOTOR_PRINTER);
        assert(motor_outputs == before + (initial != target));
        assert(channel_state[channel].motor.timed == (target < 0));
        const auto deadline = channel_state[channel].motor.end;
        if (target < 0) assert(deadline == fake_ms + 7000);

        const auto started_outputs = motor_outputs;
        fake_ms += 20;
        request_motion(command, channel / 4, channel % 4, target > 0 ? 3 : 9, target > 0 ? 0 : 0x3f);
        BMCU_run();
        assert(channel_state[channel].motor.end == deadline && motor_outputs == started_outputs);
        assert(motor_queued == 0);

        if (target > 0) {
            request_motion(command, channel / 4, channel % 4, 9, 0xa5);
            BMCU_run();
            assert(channel_state[channel].feed_stop_pending);
            request_motion(command, channel / 4, channel % 4, 9, 0x3f);
        } else {
            request_motion(command, channel / 4, channel % 4, 3, 0);
        }
        BMCU_run();
        assert(BMCU_motor_status(channel).direction == -target);
        assert(motor_outputs == started_outputs + 1 && !channel_state[channel].feed_stop_pending);
    }
}

/** 验证共用 FIFO、并发满时排队、方向更新不重复入队、实际启动后才计时 */
static void test_motor_queue() {
    reset_motor_fixture();
    motor_http(0, 1, 3000);
    BMCU_run();
    motor_http(1, 1, 300);
    request_motion(4, 0, 2, 3, 0);
    BMCU_run();
    fake_ms += 101;
    BMCU_run();
    assert(BMCU_running_count() == 1);
    assert(BMCU_motor_status(1).queue_position == 1 && BMCU_motor_status(2).queue_position == 2);
    assert(BMCU_motor_status(2).queued_source == MOTOR_PRINTER && BMCU_manual_active());
    for (int i = 0; i < 5; ++i) {
        request_motion(i % 2 ? 3 : 4, 0, 2, 3, 0);
        BMCU_run();
        assert(motor_queued == 2 && BMCU_motor_status(2).queue_position == 2);
    }
    request_pullback(3, 2);
    BMCU_run();
    assert(BMCU_motor_status(2).queued_direction == -1 && motor_queued == 2);

    fake_ms = channel_state[0].motor.end;
    request_motion(4, 0, 2, 9, 0x3f);
    BMCU_run();
    assert(motors[0].stopped && BMCU_motor_status(1).source == MOTOR_MANUAL);
    assert(BMCU_motor_status(2).queue_position == 1 && BMCU_running_count() == 1);
    fake_ms = channel_state[1].motor.end;
    request_motion(3, 0, 2, 9, 0x3f);
    BMCU_run();
    assert(BMCU_motor_status(2).direction == -1 && BMCU_motor_status(2).source == MOTOR_PRINTER);
    assert(channel_state[2].motor.end == fake_ms + 7000 && motor_queued == 0);
    assert(!BMCU_manual_active() && BMCU_running_count() == 1);
    assert(motorCmd(2, 1));
    assert(BMCU_motor_status(2).direction == 1 && motor_queued == 0);
}

/** 验证打印机接管同通道排队目标，以及停止、换通道和准备完成会取消失效排队 */
static void test_queue_updates() {
    reset_motor_fixture();
    motor_http(0, 1, 20000);
    BMCU_run();
    motor_http(1, -1);
    request_motion(3, 0, 1, 3, 0);
    BMCU_run();
    fake_ms += 101;
    BMCU_run();
    auto state = BMCU_motor_status(1);
    assert(state.queue_position == 1 && state.queued_direction == 1 && state.queued_source == MOTOR_PRINTER);
    assert(!BMCU_manual_pulse(1, -1, 300));
    request_motion(4, 0, 1, 9, 0xa5);
    BMCU_run();
    assert(motor_queued == 0);
    request_pullback(3, 1);
    BMCU_run();
    assert(BMCU_motor_status(1).queue_position == 1);
    motor_http(1, 0);
    assert(BMCU_motor_status(1).queue_position == 1);
    BMCU_run();
    assert(motor_queued == 0);
    request_motion(4, 0, 1, 3, 0);
    BMCU_run();
    assert(BMCU_motor_status(1).queue_position == 1);
    request_motion(3, 1, 2, 3, 0);
    BMCU_run();
    fake_ms += 101;
    BMCU_run();
    assert(BMCU_motor_status(1).queue_position == 0 && BMCU_motor_status(6).queue_position == 1);
    assert(BMCU_running_count() == 1);
    set_filament_online(1, 2, false);
    motor_http(0, 0);
    BMCU_run();
    assert(motors[6].stopped && motor_queued == 0); // 排队期间失去有料状态，不能在释放名额后启动。
}

/** 验证网页启停只提交请求，停止只取消当下，下一条打印机命令可重新启动 */
static void test_manual_requests() {
    reset_motor_fixture();
    motor_http(0, -1);
    assert(server.status == 202 && motors[0].stopped && motor_outputs == 0);
    BMCU_run();
    assert(!motors[0].stopped && BMCU_motor_status(0).direction == -1);
    const auto before = motor_outputs;
    motor_http(0, 0);
    assert(server.status == 202 && !motors[0].stopped && motor_outputs == before);
    assert(!BMCU_manual_pulse(0, 1, 300));
    BMCU_run();
    assert(motors[0].stopped && !channel_state[0].stop_requested);
    request_motion(3, 0, 0, 3, 0);
    BMCU_run();
    fake_ms += 101;
    BMCU_run();
    assert(BMCU_motor_status(0).source == MOTOR_PRINTER);
    motor_http(0, 0);
    BMCU_run();
    assert(motors[0].stopped && get_filament_motion(0) == waiting);
    BMCU_run();
    assert(motors[0].stopped);
    request_motion(4, 0, 0, 3, 0);
    BMCU_run();
    assert(BMCU_motor_status(0).source == MOTOR_PRINTER);
    motor_http(0, 0);
    BMCU_run();
    motor_http(0, 1);
    BMCU_run();
    motor_http(1, 1);
    motor_http(2, -1);
    assert(BMCU_motor_status(1).queue_position == 1 && BMCU_motor_status(2).queue_position == 2);
    motor_http(1, 0);
    BMCU_run();
    assert(BMCU_motor_status(1).queue_position == 0 && BMCU_motor_status(2).queue_position == 1);
    motor_http(0, 0);
    BMCU_run();
    assert(motors[0].stopped && BMCU_motor_status(2).direction == -1);
}

/** 验证总线超时取消打印机排队，保留本地手动运行和排队 */
static void test_manual_bus_timeout() {
    reset_motor_fixture();
    motor_http(0, -1, 20000);
    BMCU_run();
    request_motion(4, 0, 1, 3, 0);
    BMCU_run();
    fake_ms += 101;
    BMCU_run();
    motor_http(2, 1);
    assert(BMCU_motor_status(1).queue_position == 1 && BMCU_motor_status(2).queue_position == 2);
    fake_ms += 1001;
    BMCU_run();
    assert(BMCU_printer_bus_fault());
    assert(BMCU_motor_status(0).source == MOTOR_MANUAL && BMCU_motor_status(0).direction == -1);
    assert(BMCU_motor_status(1).queue_position == 0 && BMCU_motor_status(2).queue_position == 1);
    assert(get_filament_motion(1) == waiting);
    fake_ms = channel_state[0].motor.end;
    BMCU_run();
    assert(motors[0].stopped && BMCU_motor_status(2).source == MOTOR_MANUAL);
    assert(!motorCmd(1, 1));
}

/** 验证队列仍保留通道、引脚、扩展器和时长保护 */
static void test_motor_guards() {
    reset_motor_fixture();
    IO_CONFIG.motor[2][0] = PIN_NONE;
    assert(!BMCU_manual_pulse(2, 1, 300) && !motorCmd(2, 1));
    IO_CONFIG.motor[2][0] = 50;
    expander_online = false;
    assert(!BMCU_manual_pulse(2, 1, 300) && !motorCmd(2, 1));
    assert(!BMCU_manual_pulse(-1, 1, 300) && !BMCU_manual_pulse(16, 1, 300));
    assert(!BMCU_manual_pulse(3, 0, 300) && !BMCU_manual_pulse(3, 1, 0));
    assert(!BMCU_manual_pulse(3, 1, MAX_PULSE_MS));
    AMS_COUNT = 1;
    assert(!BMCU_manual_pulse(4, 1, 300));
    assert(BMCU_manual_pulse(0, 1, 300));
    assert(!BMCU_manual_pulse(0, -1, 300));
    assert(BMCU_manual_pulse(1, -1, 300));
    IO_CONFIG.motor[0][0] = PIN_NONE;
    BMCU_run();
    assert(BMCU_motor_status(0).queue_position == 0 && BMCU_motor_status(1).direction == -1);
}

/** 验证真实暂停实验 HTTP 处理器只登记请求，且解除、非法输入和重复触发行为正确 */
static void test_pause_test_http() {
    reset_motor_fixture();
    server.args.clear(); handle_pause_test(); assert(server.status == 400);
    for (const char *value : {"", "-1", "2", "1x", "true", "0.0"}) {
        server.args = {{"enable", value}};
        handle_pause_test(); assert(server.status == 400);
    }
    server.args = {{"enable", "1"}};
    handle_pause_test(); assert(server.status == 409);
    feed(short_request(0x20, 33)); assert(BambuBus_run() == 1);
    request_motion(3, 0, 0, 7, 0x7f);
    const auto outputs = motor_outputs;
    const auto writes = write_count;
    const auto settings_writes = settings_write_count;
    const auto sent = uart_tx.size();
    handle_pause_test(); assert(server.status == 202);
    assert(!pause_test.active && pause_test.request == 1);
    assert(motor_outputs == outputs && uart_tx.size() == sent && write_count == writes);
    handle_pause_test(); assert(server.status == 409);
    BambuBus_run(); assert(pause_test.active);
    handle_pause_test(); assert(server.status == 409);
    server.args = {{"enable", "0"}};
    handle_pause_test(); assert(server.status == 202 && pause_test.active);
    assert(motor_outputs == outputs && uart_tx.size() == sent);
    BambuBus_run(); assert(!pause_test.active);
    fake_ms += 20000; BambuBus_run();
    handle_pause_test(); assert(server.status == 202); // 离线时解除仍可提交。
    BambuBus_run(); assert(!pause_test.request && !pause_test.active);
    assert(motor_outputs == outputs && write_count == writes);
    assert(settings_write_count == settings_writes);
}

/** 验证退料离线不改变电机截止时间，阻止打印机和两种微动再次进料，并能通过原模式恢复 */
static void test_pause_offline_motor() {
    for (uint8_t command : {3, 4}) for (int mode : {CHANNEL_MODE_ONLINE, CHANNEL_MODE_AUTO}) {
        reset_motor_fixture();
        fake_ms = command == 3 ? 10000u * mode : UINT32_MAX - 500u + 10000u * (mode - 1);
        CHANNEL_MODE[6] = CHANNEL_MODE_AUTO;
        IO_CONFIG.microswitch[6] = 10;
        input_levels[10] = 0;
        BMCU_apply_channel_modes();
        feed(short_request(0x20, 33)); assert(BambuBus_run() == 1);
        request_motion(command, 1, 2, 7, 0x7f);
        assert(BambuBus_request_pause_test(true)); BMCU_run();
        request_motion(command, 1, 2, 7, 0); BMCU_run();
        request_motion(command, 1, 2, 9, 0x3f); BMCU_run();
        const auto end = channel_state[6].motor.end;
        assert(channel_state[6].motor.direction == -1 && channel_state[6].motor.timed);
        const auto outputs = motor_outputs;
        request_motion(command, 1, 0xff, 3, 0);
        assert(motor_outputs == outputs && CHANNEL_MODE[6] == CHANNEL_MODE_OFFLINE);
        BMCU_run(); check_microswitch_status();
        assert(channel_state[6].motor.direction == -1 && channel_state[6].motor.end == end);
        assert(!is_filament_online(6));
        fake_ms += 1001;
        request_motion(command, 1, 0xff, 3, 0);
        fake_ms += 5; BambuBus_save_run();
        assert(!pause_test.active && saved_channel_modes[6] == CHANNEL_MODE_OFFLINE);
        request_motion(command, 1, 2, 3, 0); BMCU_run();
        assert(channel_state[6].motor.direction == -1 && channel_state[6].motor.end == end);
        fake_ms = end - 1;
        request_motion(command, 1, 2, 3, 0); BMCU_run();
        assert(channel_state[6].motor.direction == -1);
        fake_ms = end; BMCU_run();
        assert(channel_state[6].motor.direction == 0);
        for (int level : {1, 0}) {
            input_levels[10] = level; check_microswitch_status();
            fake_ms += 31; check_microswitch_status();
        }
        assert(channel_state[6].motor.direction == 0 && !is_filament_online(6));
        IO_CONFIG.switch_pin = 11;
        switch_level = 1; switchInterrupt();
        switch_level = 0; switchInterrupt();
        assert(channel_state[6].motor.direction == 0); // 五通总微动也不能让离线通道进料。
        CHANNEL_MODE[6] = mode;
        assert(settings_save_to_flash());
        BMCU_apply_channel_modes(); // 复用网页模式恢复后的应用路径。
        assert(is_filament_online(6));
        request_motion(command, 1, 2, 3, 0); BMCU_run();
        assert(channel_state[6].motor.direction == 1);
    }
}

/**
 * 执行单个电机控制回归场景
 * @param argc 参数数量
 * @param argv 程序名和场景名
 * @return 0 表示通过，2 表示场景无效
 */
int main(int argc, char **argv) {
    assert(argc == 2);
    const std::string name = argv[1];
    if (name == "printer_target") test_printer_target();
    else if (name == "motor_queue") test_motor_queue();
    else if (name == "queue_updates") test_queue_updates();
    else if (name == "manual_requests") test_manual_requests();
    else if (name == "manual_bus_timeout") test_manual_bus_timeout();
    else if (name == "feed_stop_delay") test_feed_stop_delay();
    else if (name == "switch_input") test_switch_input();
    else if (name == "motor_guards") test_motor_guards();
    else if (name == "pause_test_http") test_pause_test_http();
    else if (name == "pause_offline_motor") test_pause_offline_motor();
    else return 2;
    std::printf("PASS %s\n", name.c_str());
}
