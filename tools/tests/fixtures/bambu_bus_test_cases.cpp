/** 为测试请求填写真实 CRC；黄金心跳另行检查固定 CRC 值 */
static void seal(std::vector<uint8_t> &frame) {
    const bool short_frame = frame[1] & 0x80u;
    const unsigned index = short_frame ? 3u : 6u;
    frame[index] = bus_crc8(frame.data(), index);
    const auto crc = bus_crc16(frame.data(), frame.size() - 2u);
    frame[frame.size() - 2u] = crc & 255u;
    frame.back() = crc >> 8;
}
/** 创建短请求，长度包含 CRC16 尾部 */
static std::vector<uint8_t> short_request(uint8_t command, size_t length) {
    std::vector<uint8_t> frame(length, 0);
    frame[0] = 0x3d; frame[1] = 0xc5; frame[2] = static_cast<uint8_t>(length); frame[4] = command;
    seal(frame); return frame;
}
/** 创建 AMS08 长请求，数据区从偏移 13 开始 */
static std::vector<uint8_t> long_request(uint16_t type, size_t payload_length) {
    std::vector<uint8_t> frame(15u + payload_length, 0);
    frame[0] = 0x3d; frame[1] = 5; frame[2] = 0x42;
    frame[4] = frame.size() & 255u; frame[5] = frame.size() >> 8;
    frame[8] = 7; frame[10] = 3; frame[11] = type & 255u; frame[12] = type >> 8;
    seal(frame); return frame;
}
/** 经真实 UART 回调注入一帧，不绕过生产代码的组帧逻辑 */
static void feed(const std::vector<uint8_t> &frame) {
    uart_rx.insert(uart_rx.end(), frame.begin(), frame.end()); on_uart_rx();
}
/** 验证集中校验的最小载荷、CRC、地址、通道范围及未知命令 */
static void test_parser() {
    assert(get_package_type(nullptr, 0) == BambuBus_package_ERROR);
    for (const auto rule : {std::pair<int,int>{3,12}, {4,13}, {5,8}, {8,45}}) {
        for (int n = 7; n < rule.second; ++n) {
            auto f = short_request(rule.first, n);
            assert(get_package_type(f.data(), f.size()) == BambuBus_package_ERROR);
        }
        auto f = short_request(rule.first, rule.second);
        assert(get_package_type(f.data(), f.size()) != BambuBus_package_ERROR);
    }
    for (const auto rule : {std::pair<int,int>{0x21a,1}, {0x211,2}, {0x218,34}, {0x103,1}, {0x402,1}}) {
        for (int n = 0; n < rule.second; ++n) {
            auto f = long_request(rule.first, n);
            assert(get_package_type(f.data(), f.size()) == BambuBus_package_ERROR);
        }
        auto f = long_request(rule.first, rule.second);
        assert(get_package_type(f.data(), f.size()) != BambuBus_package_ERROR);
        const auto expected = get_package_type(f.data(), f.size());
        f[1] = 4; seal(f);
        assert(get_package_type(f.data(), f.size()) == expected);
        f[1] = 0; seal(f);
        assert(get_package_type(f.data(), f.size()) == BambuBus_package_IGNORED);
        f[1] = 4;
        f[8] = 3; seal(f);
        assert(get_package_type(f.data(), f.size()) == BambuBus_package_IGNORED);
    }
    auto f = short_request(0x20, 33);
    assert(f[3] == 0x6b && f[31] == 0xbd && f[32] == 0x2c);
    f[3] ^= 1;
    const auto crc = bus_crc16(f.data(), f.size() - 2);
    f[31] = crc & 255u; f[32] = crc >> 8;
    assert(get_package_type(f.data(), f.size()) == BambuBus_package_ERROR);
    f = short_request(4, 13); f.back() ^= 1;
    assert(get_package_type(f.data(), f.size()) == BambuBus_package_ERROR);
    f = short_request(4, 13); f[5] = 1; seal(f);
    assert(get_package_type(f.data(), f.size()) == BambuBus_package_IGNORED);
    AMS_COUNT = 4;
    for (int ams = 0; ams < 4; ++ams) for (int ch = 0; ch < 4; ++ch) {
        f = short_request(4, 13); f[5] = ams; f[9] = ch; seal(f);
        assert(get_package_type(f.data(), f.size()) == BambuBus_package_filament_motion_long);
    }
    f[9] = 4; seal(f); assert(get_package_type(f.data(), f.size()) == BambuBus_package_ERROR);
    f[9] = 255; seal(f); assert(get_package_type(f.data(), f.size()) == BambuBus_package_filament_motion_long);
    f[5] = 4; seal(f); assert(get_package_type(f.data(), f.size()) == BambuBus_package_ERROR);
    for (uint8_t node = 0x80u; node <= 0x83u; ++node) {
        f = short_request(5, 26); f[5] = 1; f[6] = node; seal(f);
        assert(get_package_type(f.data(), f.size()) == BambuBus_package_IGNORED);
    }
    f = short_request(0x55, 7); assert(get_package_type(f.data(), f.size()) == BambuBus_package_ETC);
    f[1] = 0xc0; seal(f); assert(get_package_type(f.data(), f.size()) == BambuBus_package_IGNORED);
    f = long_request(0x999, 985); assert(get_package_type(f.data(), f.size()) == BambuBus_package_ETC);
    f = long_request(0x999, 986); assert(get_package_type(f.data(), f.size()) == BambuBus_package_ERROR);
}

/**
 * 验证认证帧经真实接收链路写入主板日志，且不产生认证应答
 * 覆盖两种请求标志、总线日志开关、其他目标地址、最大帧、坏 CRC、回显及发送背压
 */
static void test_auth_logging() {
    BambuBus_init();
    const auto initial_channel = get_now_filament_num();
    const auto initial_motion = get_filament_motion(0);
    for (const auto command : {0x040D, 0x040E}) {
        const auto expected = command == 0x040D ? BambuBus_long_package_certification :
                                                BambuBus_long_package_authorization;
        for (const auto flags : {0x04, 0x05}) {
            for (const auto payload_length : {0, 34, 985}) {
                auto frame = long_request(command, payload_length);
                frame[1] = flags;
                seal(frame);
                BUS_LOG_ENABLE = 0;
                bus_logs.clear(); main_logs.clear(); uart_tx.clear();
                feed(frame);
                assert(BambuBus_run() == 1 && cmd == expected);
                assert(uart_tx.empty() && main_logs.size() == 1 && bus_logs.size() == 1);
                assert(bus_logs[0].channel == LOG_CH_MAIN && bus_logs[0].type == expected);
                assert(bus_logs[0].direction == 'R' && bus_logs[0].data == frame);
                assert(main_logs[0].find(command == 0x040D ? "0x040D" : "0x040E") != std::string::npos);
                assert(main_logs[0].find("src=0x0300 dst=0x0700 seq=66") != std::string::npos);
                assert(main_logs[0].find("len=" + std::to_string(frame.size()) + " payload=" +
                                        std::to_string(payload_length)) != std::string::npos);
                assert(main_logs[0].find(flags == 0x04 ? "flags=0x04" : "flags=0x05") != std::string::npos);
                assert(main_logs[0].find("result=LOG_ONLY") != std::string::npos);
                assert(!have_registered[0] && get_now_filament_num() == initial_channel);
                assert(get_filament_motion(0) == initial_motion);

                BUS_LOG_ENABLE = 1;
                bus_logs.clear(); main_logs.clear();
                feed(frame); assert(BambuBus_run() == 1);
                assert(bus_logs.size() == 2 && main_logs.size() == 1 && uart_tx.empty());
                assert(bus_logs[0].channel == LOG_CH_BUS && bus_logs[0].type == expected);
                assert(bus_logs[0].data == frame && bus_logs[1].channel == LOG_CH_MAIN);

                frame[8] = 0x12; seal(frame);
                BUS_LOG_ENABLE = 0;
                bus_logs.clear(); main_logs.clear();
                const auto last_packet = last_bus_packet_ms;
                const auto address = bambubus_ams_address;
                feed(frame); assert(BambuBus_run() == 0 && cmd == BambuBus_package_IGNORED);
                assert(bus_logs.size() == 1 && main_logs.size() == 1 && uart_tx.empty());
                assert(bus_logs[0].data == frame && bus_logs[0].type == expected);
                assert(main_logs[0].find("dst=0x1200") != std::string::npos);
                assert(main_logs[0].find("result=IGNORED") != std::string::npos);
                assert(last_bus_packet_ms == last_packet && bambubus_ams_address == address);
            }
        }

        auto frame = long_request(command, 4);
        frame.back() ^= 1u;
        bus_logs.clear(); main_logs.clear();
        feed(frame); assert(BambuBus_run() == 2);
        assert(main_logs.empty() && bus_logs.empty() && uart_tx.empty());

        frame = long_request(command, 4); frame[1] = 0; seal(frame);
        feed(frame); assert(BambuBus_run() == 0);
        assert(main_logs.empty() && bus_logs.empty() && uart_tx.empty());

        frame = long_request(command, 4);
        TX_IDLE = false;
        feed(frame); assert(BambuBus_run() == 0 && main_logs.empty());
        TX_IDLE = true;
        assert(BambuBus_run() == 1 && main_logs.size() == 1 && uart_tx.empty());
        assert(BambuBus_run() == 0 && main_logs.size() == 1);
    }
}
/** 验证队列次序、满队列保护、显式释放和序号回绕 */
static void test_queue() {
    rx_write_seq = rx_read_seq = UINT32_MAX - 1u;
    uint8_t a[] = {1,2,3}, b[] = {4,5}, c[] = {6};
    completed_frame_publish(a, sizeof(a));
    const int held = completed_frame_peek();
    completed_frame_publish(b, sizeof(b));
    completed_frame_publish(c, sizeof(c));
    assert(rx_dropped_load() == 1 && completed_frame_peek() == held);
    assert(memcmp(completed_buf[held], a, sizeof(a)) == 0);
    completed_frame_release();
    completed_frame_publish(c, sizeof(c));
    assert(completed_len[completed_frame_peek()] == 2);
    assert(completed_buf[completed_frame_peek()][0] == 4);
    completed_frame_release();
    assert(completed_buf[completed_frame_peek()][0] == 6);
    completed_frame_release(); assert(completed_frame_peek() == -1);
}
/** 并发压测真实生产者/消费者队列，验证帧内容在释放前不会撕裂 */
#ifndef BMCU_TEST_WASI
static void test_queue_threads() {
    std::atomic<bool> done{false};
    std::thread producer([&] {
        for (uint32_t n = 1; n <= 50000; ++n) {
            uint32_t frame[] = {n, ~n};
            completed_frame_publish(reinterpret_cast<const uint8_t *>(frame), sizeof(frame));
        }
        done.store(true);
    });
    uint32_t previous = 0;
    while (!done.load() || completed_frame_peek() >= 0) {
        const int slot = completed_frame_peek();
        if (slot < 0) { std::this_thread::yield(); continue; }
        uint32_t first[2], second[2];
        assert(completed_len[slot] == sizeof(first));
        memcpy(first, completed_buf[slot], sizeof(first));
        std::this_thread::yield();
        memcpy(second, completed_buf[slot], sizeof(second));
        assert(first[0] > previous && first[1] == ~first[0]);
        assert(memcmp(first, second, sizeof(first)) == 0);
        previous = first[0]; completed_frame_release();
    }
    producer.join(); assert(previous > 0);
}
#endif
/** 验证残帧超时恢复、头部损坏、长包双字节长度和回调忙状态 */
static void test_receiver() {
    fake_ms = UINT32_MAX - 40u;
    feed({0x3d,0xc5}); assert(BambuBus_recent_activity(5));
    fake_ms += 100; assert(BambuBus_recent_activity(5));
    ++fake_ms; assert(!BambuBus_recent_activity(5));
    auto f = short_request(0x20, 33); feed(f);
    assert(BambuBus_run() == 1 && BambuBus_printer_status() == 1);
    f[3] ^= 1; feed(f); assert(BambuBus_rx_error_count() >= 1);
    f = long_request(0x999, 285); feed(f);
    assert(completed_len[completed_frame_peek()] == 300);
    assert(BambuBus_run() == 1);
    feed({0x3d, 0x05, 0, 0, 0xff, 0xff});
    assert(BambuBus_rx_error_count() >= 2);
    during_read = [] { assert(BambuBus_recent_activity(0)); };
    feed(short_request(0x20,33)); during_read = nullptr;
    assert(BambuBus_run() == 1);
}
/** 验证无通信启动、10 秒心跳边界、错误帧不续期、其它设备不解锁及回绕后超时锁存 */
static void test_timeout() {
    fake_ms = 2000; assert(BambuBus_run() == 0 && BambuBus_printer_status() == 0);
    fake_ms = UINT32_MAX - 500u; feed(short_request(0x20,33)); assert(BambuBus_run() == 1);
    fake_ms += 1000; assert(BambuBus_run() == 0);
    ++fake_ms; assert(BambuBus_run() == -1);
    auto bad = short_request(0x20,33); bad.back() ^= 1; feed(bad);
    assert(BambuBus_run() == -1);
    auto other = long_request(0x21a,1); other[8] = 3; seal(other); feed(other);
    assert(BambuBus_run() == -1);
    fake_ms += 8998; assert(BambuBus_run() == -1 && BambuBus_printer_status() == 1);
    ++fake_ms; assert(BambuBus_run() == -1 && BambuBus_printer_status() == 2);
    fake_ms = UINT32_MAX - 499u;
    assert(BambuBus_run() == -1 && BambuBus_printer_status() == 2);
    feed(short_request(0x20,33)); assert(BambuBus_run() == 1 && BambuBus_printer_status() == 1);
}
/** 验证保存静默条件、协议处理与保存分离、回读损坏重试及长期冷却边界 */
static void test_save() {
    fake_ms = UINT32_MAX - 10u;
    BambuBus_set_need_to_save(); feed(short_request(0x20,33));
    BambuBus_save_run(); assert(write_count == 0);
    assert(BambuBus_run() == 1 && write_count == 0);
    fake_ms += 4; BambuBus_save_run(); assert(write_count == 0);
    ++fake_ms; fail_write = true; BambuBus_save_run(); assert(write_count == 1 && BambuBus_need_to_save);
    fake_ms += 999; BambuBus_save_run(); assert(write_count == 1);
    ++fake_ms; fail_write = false; corrupt_read = true; BambuBus_save_run();
    assert(write_count == 2 && read_count == 1 && BambuBus_need_to_save);
    fake_ms += 1000; corrupt_read = false; BambuBus_save_run();
    assert(write_count == 3 && read_count == 2 && !BambuBus_need_to_save);
    fake_ms += 1000; BambuBus_save_run(); // 没有待保存内容也推进冷却状态
    fake_ms += 25u * 86400000u;
    BambuBus_set_need_to_save(); BambuBus_save_run(); assert(write_count == 4);
    fake_ms += 1000; BambuBus_set_need_to_save();
    feed({0x3d,0xc5}); fake_ms += 99; BambuBus_save_run(); assert(write_count == 4);
    fake_ms += 2; BambuBus_save_run(); assert(write_count == 5);
    fake_ms += 1000; BambuBus_set_need_to_save();
    uart_rx.push_back(0); BambuBus_save_run(); assert(write_count == 5);
    uart_rx.clear(); TX_IDLE = false; BambuBus_save_run(); assert(write_count == 5);
    TX_IDLE = true; rx_callback_seq = 1; BambuBus_save_run(); assert(write_count == 5);
    rx_callback_seq = 0; last_bus_tx_ms = fake_ms; bus_tx_seen = true;
    BambuBus_save_run(); assert(write_count == 5);
    fake_ms += 5; BambuBus_save_run(); assert(write_count == 6);
    fake_ms += 1000; BambuBus_set_need_to_save();
    // 在空闲检查读取快照之后、返回之前完成一次接收回调，产生新的残帧
    during_available = [] { feed({0x3d,0xc5}); };
    BambuBus_save_run(); assert(write_count == 6);
    fake_ms += 101; BambuBus_save_run(); assert(write_count == 7);
}
/** 验证零毫秒可作为有效起点，Dxx 首次时间为零也能累计后续间隔 */
static void test_zero_time() {
    process_motion_state_machine(0, 0, 3, 0);
    process_motion_state_machine(0, 0, 7, 0x7f);
    fake_ms = 14999; process_motion_state_machine(0, 0, 7, 0x7f);
    assert(filament_ext[0][0].motion == filament_motion_internal::send_out);
    fake_ms = 15000; process_motion_state_machine(0, 0, 7, 0x7f);
    assert(filament_ext[0][0].motion == filament_motion_internal::on_use);
    reset_filament_meters(0);
    set_filament_motion(0, waiting);
    fake_ms = 0; auto f = short_request(4,13); f[6] = 7; f[7] = 0x7f; seal(f);
    feed(f); BambuBus_run(); fake_ms = 100; feed(f); BambuBus_run();
    assert(filament_ext[0][0].meters_virtual_count == 100u);
}
/**
 * 从广播回复构造身份确认请求
 * @param advertisement 29 字节注册广播回复
 * @return 带真实 CRC 的 26 字节注册确认请求
 * 确认前缀固定为 0A，只复制 16 字节身份，不包含回复的预留字段或 CRC
 */
static std::vector<uint8_t> registration_query(const std::vector<uint8_t> &advertisement) {
    assert(advertisement.size() == 29);
    auto frame = short_request(5, 26);
    frame[5] = 1; frame[6] = advertisement[6];
    frame[7] = 0x0A;
    memcpy(frame.data() + 8, advertisement.data() + 8, 16);
    seal(frame);
    return frame;
}

/**
 * 读取指定 AMS 的完整序列号载荷
 * @param ams AMS 编号
 * @param payload_length 请求载荷长度；不超过 33 时验证旧式查询回退
 * @return 66 字节身份载荷
 * 经真实收发检查载荷位置、地址、长度与 CRC，首字节故意与 AMS 不同以防误用字段
 */
static std::vector<uint8_t> query_serial(uint8_t ams, size_t payload_length = 34) {
    auto frame = long_request(0x402, payload_length);
    frame[13] = 0xff;
    if (payload_length > 33) frame[13 + 33] = ams;
    seal(frame);
    const auto before = uart_tx.size();
    feed(frame); assert(BambuBus_run() == 1 && uart_tx.size() == before + 1);
    const auto &reply = uart_tx.back();
    assert(reply.size() == 81 && reply[4] == 81 && reply[5] == 0);
    assert(reply[7] == 0 && reply[8] == 3 && reply[9] == 0 && reply[10] == 7);
    assert(reply[6] == bus_crc8(reply.data(), 6) && package_check_crc16(reply.data(), reply.size()));
    assert(reply[13] == 15u);
    assert(reply[13 + 65] == (payload_length > 33 ? ams : 0));
    return {reply.begin() + 13, reply.end() - 2};
}

/**
 * 验证多 AMS 的身份确认及注册隔离
 * 广播、身份错误、CRC 错误和重复查询均通过真实收发路径；错误请求不得发送确认或取消注册
 */
static void test_registration() {
    AMS_COUNT = 4;
    auto broadcast = short_request(5, 8);
    clock_step = 1; feed(broadcast); assert(BambuBus_run() == 1); clock_step = 0;
    assert(uart_tx.size() == 4);
    const auto advertised = uart_tx;
    for (const auto &frame : advertised) assert(frame[7] == 0x0C);
    clock_step = 1; feed(broadcast); assert(BambuBus_run() == 1); clock_step = 0;
    assert(uart_tx.size() == 8);
    for (size_t i = 4; i < 8; ++i) assert(uart_tx[i][7] == 0x0A);
    for (uint8_t ams = 0; ams < 4; ++ams) {
        assert(advertised[ams][6] == ams && advertised[ams].size() == 29);
        assert(advertised[ams][3] == bus_crc8(advertised[ams].data(), 3));
        assert(package_check_crc16(advertised[ams].data(), 29));
        const auto serial = query_serial(ams);
        assert(memcmp(advertised[ams].data() + 8, serial.data() + 33, 16) == 0);
        for (uint8_t other = 0; other < ams; ++other)
            assert(memcmp(advertised[ams].data() + 8, advertised[other].data() + 8, 16) != 0);
        auto query = registration_query(advertised[ams]);
        const auto count = uart_tx.size();
        const auto last_valid = last_bus_packet_ms;
        auto first_prefix = query; first_prefix[7] = 0x0C; seal(first_prefix);
        feed(first_prefix); assert(BambuBus_run() == 0 && uart_tx.size() == count);
        for (int offset = 7; offset < 24; ++offset) {
            auto bad = query; bad[offset] ^= 1; seal(bad);
            fake_ms += 1;
            feed(bad); assert(BambuBus_run() == 0);
            assert(uart_tx.size() == count && !have_registered[ams] && last_bus_packet_ms == last_valid);
        }
        auto bad_crc = query; bad_crc.back() ^= 1;
        feed(bad_crc); assert(BambuBus_run() == 2 && !have_registered[ams]);
        feed(query); assert(BambuBus_run() == 1 && have_registered[ams]);
        assert(uart_tx.size() == count + 1 && uart_tx.back()[5] == 1);
        assert(memcmp(uart_tx.back().data() + 7, query.data() + 7, 17) == 0);
        auto wrong_ams = query; wrong_ams[6] = (ams + 1) % 4; seal(wrong_ams);
        feed(wrong_ams); assert(BambuBus_run() == 0 && uart_tx.size() == count + 1);
        auto mismatched = query; mismatched[7] ^= 1; seal(mismatched);
        feed(mismatched); assert(BambuBus_run() == 0 && have_registered[ams]);
        assert(uart_tx.size() == count + 1);
        for (uint8_t other = 0; other < 4; ++other) assert(have_registered[other] == (other <= ams));
        feed(query); assert(BambuBus_run() == 1 && have_registered[ams]);
    }
    const auto count = uart_tx.size();
    clock_step = 1; feed(broadcast); assert(BambuBus_run() == 1); clock_step = 0;
    assert(uart_tx.size() == count); // 已注册 AMS 不重复发布
    bool registered_before[4];
    memcpy(registered_before, have_registered, sizeof(registered_before));
    auto auth_probe = short_request(5, 26);
    auth_probe[5] = 1;
    for (uint8_t node = 0x80u; node <= 0x83u; ++node) {
        auth_probe[6] = node; seal(auth_probe);
        feed(auth_probe); assert(BambuBus_run() == 0);
        assert(completed_frame_peek() < 0);
        assert(uart_tx.size() == count);
        assert(memcmp(have_registered, registered_before, sizeof(registered_before)) == 0);
    }
    auto short_query = short_request(5, 25); short_query[5] = 1; seal(short_query);
    assert(get_package_type(short_query.data(), short_query.size()) == BambuBus_package_ERROR);
}

/**
 * 验证四台自动身份、自定义序列号和数量切换时的槽位隔离
 * 自动后缀末位分别为 0..3；完整 16 位文本与注册身份一致，只有实际身份变化才重新注册
 */
static void test_serial_identity() {
    AMS_COUNT = 4;
    std::vector<uint8_t> automatic[4];
    for (uint8_t ams = 0; ams < 4; ++ams) {
        automatic[ams] = query_serial(ams);
        char expected[17] = {};
        snprintf(expected, sizeof(expected), "0EA%u56789ABCDEF%u", ams, ams);
        assert(automatic[ams][0] == 15 && memcmp(automatic[ams].data() + 1, expected, 16) == 0);
        assert(automatic[ams][16] == '0' + ams && automatic[ams][17] == 0);
        assert(memcmp(automatic[ams].data() + 33, F00_res[ams] + 8, 16) == 0);
        auto confirm = registration_query(std::vector<uint8_t>(F00_res[ams], F00_res[ams] + 29));
        feed(confirm); assert(BambuBus_run() == 1 && have_registered[ams]);
        for (uint8_t other = 0; other < ams; ++other)
            assert(memcmp(automatic[ams].data() + 33, automatic[other].data() + 33, 16) != 0);
    }
    for (const uint8_t ams : {3, 1, 0, 2})
        assert(query_serial(ams) == automatic[ams] && have_registered[ams]);

    // 用户已验证的完整 SN，末位 8 必须按自定义输入保留，不能替换为槽位编号 1。
    static const char suffix[] = "7BF9E5A60A48";
    static const char verified_serial[] = "0EA17BF9E5A60A48";
    static const uint8_t verified_identity[16] = {
        0x0E, 0xA1, 0x7B, 0xF9, 0xE5, 0xA6, 0x0A, 0x48,
        0xDE, 0xF0, 0x37, 0x33, 0xFF, 0xFF, 0xFF, 0xFF
    };
    memcpy(CUSTOM_SERIAL[1], suffix, sizeof(suffix));
    assert(query_serial(3) == automatic[3]); // 查询别台也刷新变化槽位，但不能让别台掉注册。
    assert(!have_registered[1] && online_detect_phase[1] == 0 && F00_res[1][7] == 0x0C);
    const auto custom = query_serial(1);
    assert(custom[0] == 15 && memcmp(custom.data() + 1, verified_serial, 16) == 0);
    assert(memcmp(custom.data() + 33, verified_identity, sizeof(verified_identity)) == 0);
    assert(memcmp(F00_res[1] + 8, verified_identity, sizeof(verified_identity)) == 0);
    for (uint8_t ams = 0; ams < 4; ++ams) {
        if (ams == 1) continue;
        assert(query_serial(ams) == automatic[ams] && have_registered[ams]);
    }
    auto confirm = registration_query(std::vector<uint8_t>(F00_res[1], F00_res[1] + 29));
    feed(confirm); assert(BambuBus_run() == 1 && have_registered[1]);
    assert(query_serial(1) == custom && have_registered[1]);

    AMS_COUNT = 1;
    assert(query_serial(0) == automatic[0] && have_registered[0]);
    auto disabled = long_request(0x402, 34);
    disabled[13 + 33] = 1; seal(disabled);
    const auto before = uart_tx.size();
    feed(disabled); assert(BambuBus_run() == 0 && uart_tx.size() == before);
    AMS_COUNT = 4;
    assert(query_serial(1) == custom && have_registered[1]);
    for (const uint8_t ams : {0, 2, 3})
        assert(query_serial(ams) == automatic[ams] && have_registered[ams]);

    CUSTOM_SERIAL[1][0] = 0; // 留下旧尾部字符，清空输入仍必须恢复自动身份。
    assert(query_serial(1) == automatic[1] && !have_registered[1]);
    for (const uint8_t ams : {0, 2, 3})
        assert(query_serial(ams) == automatic[ams] && have_registered[ams]);
    confirm = registration_query(std::vector<uint8_t>(F00_res[1], F00_res[1] + 29));
    feed(confirm); assert(BambuBus_run() == 1 && have_registered[1]);

    // 自定义值等于自动结果时，以及再恢复自动时，均不能重置已有注册。
    memcpy(CUSTOM_SERIAL[1], "56789ABCDEF1", CUSTOM_SERIAL_LENGTH + 1);
    assert(query_serial(1) == automatic[1] && have_registered[1]);
    CUSTOM_SERIAL[1][0] = 0;
    assert(query_serial(1) == automatic[1] && have_registered[1]);
    assert(query_serial(3, 33) == automatic[0]); // 旧式查询不把载荷尾部或 CRC 当 AMS 字段。
    assert(query_serial(3, 1) == automatic[0]);
    auto invalid = long_request(0x402, 34);
    invalid[13 + 33] = 4; seal(invalid);
    assert(get_package_type(invalid.data(), invalid.size()) == BambuBus_package_ERROR);
    invalid[13 + 33] = 3; seal(invalid); invalid.back() ^= 1;
    assert(get_package_type(invalid.data(), invalid.size()) == BambuBus_package_ERROR);
}

/**
 * 读取指定 AMS 的版本应答载荷
 * @param ams AMS 编号
 * @return 21 字节版本与型号载荷
 * 经真实请求路径检查版本字节顺序和末字节 AMS 编号
 */
static std::vector<uint8_t> query_version(uint8_t ams) {
    auto frame = long_request(0x103, 1);
    frame[13] = ams;
    seal(frame);
    const auto before = uart_tx.size();
    feed(frame); assert(BambuBus_run() == 1 && uart_tx.size() == before + 1);
    const auto &reply = uart_tx.back();
    assert(reply.size() == 36 && reply[4] == 36 && reply[5] == 0);
    assert(reply[6] == bus_crc8(reply.data(), 6) && package_check_crc16(reply.data(), reply.size()));
    assert(reply[13 + 20] == ams);
    return {reply.begin() + 13, reply.end() - 2};
}

/** 验证四台独立版本按 D.C.B.A 上线，单台修改和数量切换保留其余版本及注册身份 */
static void test_ams_version() {
    AMS_COUNT = 4;
    const int defaults[4] = {1, 0, 6, 87};
    for (uint8_t ams = 0; ams < 4; ++ams) {
        const auto factory = query_version(ams);
        assert(factory.size() == 21);
        assert(factory[0] == 87 && factory[1] == 6 && factory[2] == 0 && factory[3] == 1);
        assert(memcmp(factory.data() + 4, "AMS08", 5) == 0 && factory[20] == ams);
        query_serial(ams);
        have_registered[ams] = true;
    }

    const int custom[4][4] = {{90, 1, 2, 3}, {1, 2, 3, 4}, {10, 50, 0, 0}, {255, 0, 128, 7}};
    memcpy(AMS_VERSION, custom, sizeof(custom));
    std::vector<uint8_t> versions[4];
    for (uint8_t ams = 0; ams < 4; ++ams) {
        versions[ams] = query_version(ams);
        for (int part = 0; part < 4; ++part)
            assert(versions[ams][part] == custom[ams][3 - part]);
        assert(memcmp(versions[ams].data() + 4, "AMS08", 5) == 0 && versions[ams][20] == ams);
        query_serial(ams);
        assert(have_registered[ams]);
    }
    memcpy(AMS_VERSION[2], defaults, sizeof(defaults));
    const auto changed = query_version(2);
    assert(changed[0] == 87 && changed[1] == 6 && changed[2] == 0 && changed[3] == 1);
    for (const uint8_t ams : {0, 1, 3}) assert(query_version(ams) == versions[ams]);
    AMS_COUNT = 1;
    assert(query_version(0) == versions[0]);
    auto disabled = long_request(0x103, 1);
    disabled[13] = 3; seal(disabled);
    const auto before = uart_tx.size();
    feed(disabled); assert(BambuBus_run() == 0 && uart_tx.size() == before);
    AMS_COUNT = 4;
    assert(query_version(3) == versions[3]);
}

/** 验证发送端边界、真实 Cxx/Dxx/长包回复，以及回复时接收不覆盖队首 */
static void test_responses() {
    during_tx = [] {
        auto hb = short_request(0x20,33);
        feed(hb); feed(hb);
    };
    auto f = short_request(3,12); feed(f); assert(BambuBus_run() == 1);
    during_tx = nullptr;
    assert(uart_tx.size() == 1 && uart_tx[0].size() == sizeof(Cxx_res));
    assert(BambuBus_rx_dropped_count() == 1);
    assert(BambuBus_run() == 1 && BambuBus_run() == 0);
    f = short_request(4,13); feed(f); assert(BambuBus_run() == 1);
    assert(uart_tx.back().size() == sizeof(Dxx_res));
    f = long_request(0x21a,1); feed(f); assert(BambuBus_run() == 1);
    for (auto &tx : uart_tx) {
        const bool is_short = tx[1] & 0x80u;
        const size_t declared = is_short ? tx[2] : tx[4] | (tx[5] << 8);
        const unsigned index = is_short ? 3u : 6u;
        assert(declared == tx.size() && tx[index] == bus_crc8(tx.data(), index));
        assert(package_check_crc16(tx.data(), tx.size()));
    }
    const auto count = uart_tx.size();
    package_send_with_crc(nullptr, 0);
    uint8_t invalid[] = {0x3d,0xc0,99,0,0,0,0}; package_send_with_crc(invalid,sizeof(invalid));
    long_packge_data long_data{}; long_data.data_length = 986; BambuBus_long_package_send(&long_data);
    assert(uart_tx.size() == count);
}
/**
 * 重置运动测试场景
 * 清除运动历史、通知计数和已发送回复，保留真实队列的发布及消费序号
 */
static void reset_motion_fixture() {
    data_save = flash_save_struct();
    data_save.BambuBus_now_filament_num = 0xFF;
    for (auto &ams : filament_ext) for (auto &ch : ams) ch = _filament_ext();
    std::fill(std::begin(idle_notifications), std::end(idle_notifications), 0u);
    uart_tx.clear();
}

/**
 * 从真实接收路径发送运动请求并检查响应封装
 * @param command 3 为 Cxx，4 为 Dxx
 * @param ams AMS 编号
 * @param channel 请求通道
 * @param status 状态标志
 * @param motion 运动标志
 * 验证两种不同字段布局都通过集中校验、状态机和发送端 CRC 处理
 */
static void request_motion(uint8_t command, uint8_t ams, uint8_t channel, uint8_t status, uint8_t motion) {
    auto frame = short_request(command, command == 3 ? 12 : 13);
    frame[5] = ams; frame[6] = status;
    frame[command == 3 ? 7 : 9] = channel;
    frame[command == 3 ? 8 : 7] = motion;
    seal(frame);
    const size_t previous = uart_tx.size();
    feed(frame);
    assert(BambuBus_run() == 1 && uart_tx.size() == previous + 1);
    const auto &reply = uart_tx.back();
    assert(reply.size() == (command == 3 ? 44u : 60u));
    assert(reply[2] == reply.size() && reply[4] == command);
    assert(reply[3] == bus_crc8(reply.data(), 3));
    assert(package_check_crc16(reply.data(), reply.size()));
    if (command == 4) assert(reply[12] == channel);
}

/**
 * 校验最新响应的关键运动字段
 * @param ams 应回复的 AMS 编号
 * @param channel 应反馈的当前通道
 * @param use_flag 使用标志
 * @param pressure 期望压力
 * 同时检查 Cxx 的第二通道字段和 Dxx 的 AMS 状态编号
 */
static void expect_motion_feedback(uint8_t ams, uint8_t channel, uint8_t use_flag, uint16_t pressure) {
    const auto &reply = uart_tx.back();
    const size_t offset = reply[4] == 3 ? 5u : 17u;
    assert(reply[5] == ams && reply[offset] == ams);
    assert(reply[offset + 2] == use_flag && reply[offset + 3] == channel);
    assert((reply[offset + 8] | (reply[offset + 9] << 8)) == pressure);
    if (reply[4] == 3) assert(reply[38] == channel);
    if (channel == 0xFF) {
        float meters = 0;
        memcpy(&meters, reply.data() + offset + 4, sizeof(meters));
        assert(meters == 0.0f);
    }
}

/**
 * 验证完整运动协议序列
 * 对全部 AMS/通道分别运行 Cxx、Dxx 及交替序列，验证状态、重复请求历史和无通道反馈
 */
static void test_motion_sequences() {
    AMS_COUNT = 4;
    for (int mode = 0; mode < 3; ++mode) for (uint8_t ams = 0; ams < 4; ++ams) {
        for (uint8_t ch = 0; ch < 4; ++ch) {
            reset_motion_fixture();
            int step = 0;
            const auto send = [&](uint8_t channel, uint8_t status, uint8_t motion) {
                const uint8_t command = mode == 0 ? 3 : mode == 1 ? 4 : 3 + (step++ % 2);
                fake_ms += 100;
                request_motion(command, ams, channel, status, motion);
            };
            send(ch, 3, 0);
            assert(get_filament_motion(ams * 4 + ch) == need_send_out);
            expect_motion_feedback(ams, ch, 2, 0x4700);
            send(ch, 9, 0xa5);
            assert(get_filament_motion(ams * 4 + ch) == waiting);
            expect_motion_feedback(ams, ch, 4, 0x4700);
            send(ch, 9, 0xa5); // 重复准备请求保持使用中压力
            expect_motion_feedback(ams, ch, 4, 0x2b00);
            send(ch, 7, 0x7f);
            assert(filament_ext[ams][ch].motion == filament_motion_internal::on_use);
            expect_motion_feedback(ams, ch, 4, 0x2b00);
            send(0xff, 0, 0); // 无通道查询保留使用状态并回复当前通道
            expect_motion_feedback(ams, ch, 4, 0x2b00);
            send((ch + 1) % 4, 7, 0); // 非当前通道的停止请求不得改变所选通道
            expect_motion_feedback(ams, ch, 4, 0x2b00);
            assert(get_now_filament_num() == ams * 4 + ch);
            send(ch, 7, 0);
            assert(filament_ext[ams][ch].motion == filament_motion_internal::stop_on_use);
            expect_motion_feedback(ams, ch, 4, 0x2b00);
            send(ch, 9, 0x3f);
            assert(get_filament_motion(ams * 4 + ch) == need_pull_back);
            expect_motion_feedback(ams, ch, 4, 0x2b00);
            send(0xff, 3, 0);
            assert(filament_ext[ams][ch].motion == filament_motion_internal::pull_back);
            expect_motion_feedback(ams, ch, 2, 0x4700);
            send(0xff, 1, 0); // 轮询不得中止正在进行的退料
            assert(get_filament_motion(ams * 4 + ch) == need_pull_back);
            expect_motion_feedback(ams, ch, 2, 0x4700);
            set_filament_motion(ams * 4 + ch, waiting); // 模拟主循环退料完成
            send(0xff, 1, 0);
            assert(get_filament_motion(ams * 4 + ch) == waiting && get_now_filament_num() == 0xff);
            expect_motion_feedback(ams, 0xff, 0, 0xff74);
            send(0xff, 0, 0);
            expect_motion_feedback(ams, 0xff, 0, 0xff74);
        }
    }
    // PJARCZAK 注释中的原始 Cxx 请求，避免测试仅依赖自身构造器
    reset_motion_fixture();
    feed({0x3d,0xc5,0x0c,0xc8,0x03,0x00,0x07,0x00,0x7f,0x02,0x36,0x54});
    assert(BambuBus_run() == 1);
    expect_motion_feedback(0, 0, 4, 0x2b00);
}

/**
 * 验证不同 AMS、通道之间的运动状态隔离
 * 无通道请求不能清掉另一 AMS 的计时、选择和阻断，真实换料仍释放旧通道
 */
static void test_motion_isolation() {
    AMS_COUNT = 4;
    reset_motion_fixture();
    fake_ms = 100;
    request_motion(3, 1, 2, 3, 0);
    request_motion(4, 1, 2, 7, 0x7f);
    const auto started = filament_ext[1][2].sendout_onuse_start_ticks;
    for (uint8_t ams : {0, 2, 3}) for (uint8_t status : {3, 1, 0}) {
        fake_ms += 100;
        request_motion(ams % 2 ? 3 : 4, ams, 0xff, status, 0);
        expect_motion_feedback(ams, 0xff, 0, 0xff74);
        assert(get_now_filament_num() == 6 && get_filament_motion(6) == need_send_out);
        assert(filament_ext[1][2].is_sendout_onuse_timing);
        assert(filament_ext[1][2].sendout_onuse_start_ticks == started && idle_notifications[6] == 0);
    }
    fake_ms = started + 15000;
    request_motion(3, 1, 2, 7, 0x7f);
    expect_motion_feedback(1, 2, 4, 0x2b00);
    request_motion(4, 0, 2, 3, 0); // 同号通道，真实切换到另一 AMS
    assert(get_now_filament_num() == 2 && get_filament_motion(6) == waiting);
    assert(!filament_ext[1][2].is_sendout_onuse_timing && idle_notifications[6] == 1);
    request_motion(3, 0, 2, 9, 0xa5);
    expect_motion_feedback(0, 2, 4, 0x4700);
    assert(filament_ext[1][2].motion == filament_motion_internal::idle);

    // 非当前通道的探测时间不能改变所用通道的长度累计间隔
    reset_motion_fixture(); fake_ms = 0;
    request_motion(3, 0, 0, 7, 0x7f);
    fake_ms = 100; request_motion(4, 0, 1, 0, 0);
    fake_ms = 150; request_motion(3, 1, 0, 0, 0);
    fake_ms = 200; request_motion(4, 0, 0, 7, 0x7f);
    assert(filament_ext[0][0].meters_virtual_count == 200);
    assert(filament_ext[0][1].meters_virtual_count == 0 && filament_ext[1][0].meters_virtual_count == 0);

    // 内部停车只复位协议历史，打印机后续 idle 才解除阻断
    const auto notified = idle_notifications[0];
    set_filament_motion(0, waiting);
    assert(idle_notifications[0] == notified && !filament_ext[0][0].last_time_valid);
    request_motion(3, 0, 0xff, 1, 0);
    assert(idle_notifications[0] == notified + 1);
    expect_motion_feedback(0, 0xff, 0, 0xff74);
}

/**
 * 验证混合请求的计时与长度上限
 * 在零起点和 32 位回绕点验证 15 秒边界，同毫秒 Cxx/Dxx 不重复累计时间
 */
static void test_motion_timing() {
    for (const uint32_t start : {0u, 0xfffffff0u}) {
        reset_motion_fixture(); fake_ms = start;
        request_motion(3, 0, 0, 3, 0);
        request_motion(4, 0, 0, 7, 0x7f);
        fake_ms = start + 14999u; request_motion(3, 0, 0, 7, 0x7f);
        assert(get_filament_motion(0) == need_send_out);
        expect_motion_feedback(0, 0, 4, 0x2b00);
        fake_ms = start + 15000u; request_motion(4, 0, 0, 7, 0x7f);
        assert(filament_ext[0][0].motion == filament_motion_internal::on_use);
        assert(!filament_ext[0][0].is_sendout_onuse_timing);
        expect_motion_feedback(0, 0, 4, 0x2b00);
    }
    reset_motion_fixture(); fake_ms = 0;
    request_motion(3, 0, 0, 7, 0x7f);
    for (int n = 0; n < 12; ++n) {
        fake_ms += 900;
        request_motion(3, 0, 0, 7, 0x7f);
        request_motion(4, 0, 0, 7, 0x7f);
        assert(filament_ext[0][0].meters_virtual_count == std::min((n + 1) * 900, 10000));
    }
    reset_filament_meters(0);
    fake_ms += 1001; request_motion(3, 0, 0, 7, 0x7f);
    assert(filament_ext[0][0].meters_virtual_count == 0); // 长时间无请求不补算长度
    fake_ms += 1; request_motion(4, 0, 0, 7, 0x7f);
    assert(filament_ext[0][0].meters_virtual_count == 1);

    // 取消一次未完成的等待后重新送出，不得沿用旧起点
    request_motion(3, 0, 0, 3, 0);
    request_motion(4, 0, 0, 7, 0x7f);
    fake_ms += 4000; set_filament_motion(0, waiting);
    assert(!filament_ext[0][0].is_sendout_onuse_timing);
    request_motion(3, 0, 0, 3, 0);
    request_motion(4, 0, 0, 7, 0x7f);
    fake_ms += 1000; request_motion(3, 0, 0, 7, 0x7f);
    assert(get_filament_motion(0) == need_send_out);
}

/**
 * 验证发送占用时的请求保留和业务状态不变
 * 模拟发送槽已占用，确认运动及设置请求都延后处理，释放后按队列顺序回复
 */
static void test_tx_backpressure() {
    reset_motion_fixture();
    auto motion = short_request(3, 12); motion[6] = 3; seal(motion);
    feed(motion);
    const auto read = rx_read_seq;
    const auto previous_cmd = cmd;
    TX_IDLE = false;
    assert(BambuBus_run() == 0 && rx_read_seq == read && cmd == previous_cmd);
    assert(get_filament_motion(0) == waiting && uart_tx.empty());
    assert(!bus_packet_seen && BambuBus_recent_activity(0));
    TX_IDLE = true;
    assert(BambuBus_run() == 1 && rx_read_seq == read + 1 && uart_tx.size() == 1);
    assert(get_filament_motion(0) == need_send_out && !bus_request_active);

    auto setting = short_request(8, 45);
    memcpy(setting.data() + 7, "TEST1234", 8); seal(setting);
    const auto saved = data_save;
    BambuBus_need_to_save = false;
    feed(setting);
    TX_IDLE = false;
    fake_ms += 1001; // 等待发送时仍必须触发通信超时
    assert(BambuBus_run() == -1);
    assert(memcmp(&saved, &data_save, sizeof(saved)) == 0 && !BambuBus_need_to_save);
    assert(uart_tx.size() == 1 && rx_read_seq == read + 1);
    TX_IDLE = true;
    assert(BambuBus_run() == 1 && uart_tx.size() == 2 && BambuBus_need_to_save);
    assert(memcmp(data_save.filament[0][0].ID, "TEST1234", 8) == 0);
    assert(rx_read_seq == read + 2 && !bus_request_active && TX_IDLE);
}

/**
 * 验证发送和日志期间的缓冲所有权
 * 在 RX 日志、UART 写入和 TX 日志中尝试重入，队首内容、发送内容和关联类型必须保持稳定
 */
static void test_tx_lifetime() {
    reset_motion_fixture();
    BUS_LOG_ENABLE = 1;
    during_log = [](char direction) {
        assert(bus_request_active);
        const auto read = rx_read_seq;
        assert(BambuBus_run() == 0 && rx_read_seq == read);
        if (direction == 'T') assert(!TX_IDLE);
    };
    during_tx = [] {
        assert(!TX_IDLE && bus_request_active && bus_tx_context_type == BambuBus_package_filament_motion_short);
        const auto read = rx_read_seq;
        const auto original = std::vector<uint8_t>(packge_send_buf, packge_send_buf + sizeof(Cxx_res));
        auto next = short_request(4, 13); next[6] = 3; next[9] = 1; seal(next);
        feed(next);
        assert(BambuBus_run() == 0 && rx_read_seq == read);
        assert(get_filament_motion(1) == waiting);
        Cxx_res[8] = 3; // 修改源模板，实际在途帧与日志必须仍使用发送槽副本
        send_uart(Cxx_res, sizeof(Cxx_res));
        package_send_with_crc(Cxx_res, sizeof(Cxx_res));
        long_packge_data long_data{}; BambuBus_long_package_send(&long_data);
        assert(std::equal(original.begin(), original.end(), packge_send_buf));
        assert(cmd == BambuBus_package_filament_motion_short);
    };
    auto frame = short_request(3, 12); frame[6] = 3; seal(frame);
    feed(frame); assert(BambuBus_run() == 1);
    during_tx = nullptr;
    assert(uart_tx.size() == 1 && uart_tx[0][8] == 0);
    assert(package_check_crc16(uart_tx[0].data(), uart_tx[0].size()));
    assert(bus_logs.size() == 2);
    assert(bus_logs[0].direction == 'R' && bus_logs[0].data == frame);
    assert(bus_logs[1].direction == 'T' && bus_logs[1].data == uart_tx[0]);
    for (const auto &log : bus_logs) assert(log.type == BambuBus_package_filament_motion_short);
    assert(TX_IDLE && !bus_request_active && bus_tx_context_type == -1);
    during_log = nullptr;
    assert(BambuBus_run() == 1 && get_filament_motion(1) == need_send_out && uart_tx.size() == 2);
    assert(bus_logs.back().type == BambuBus_package_filament_motion_long);
    assert(completed_frame_peek() < 0);
    BUS_LOG_ENABLE = 0;
}

#include "bambu_u_motion_golden.h"

/**
 * 验证 BMCU-U 的打印准备、持续轮询和明确停料
 * 全部 45 帧与独立生成的完整字节样例比较，再检查结束、跨 AMS、忙状态和恢复边界
 */
static void test_u_compatibility() {
    BambuBus_init();
    assert(bus_baud == 1250000);
    AMS_COUNT = 4;
    reset_motion_fixture();
    bambubus_ams_address = BambuBus_address_AMS08;
    for (auto &filament : data_save.filament[0]) filament.statu = online;
    data_save.filament[0][0].meters = 1.0f;
    package_num = 0;
    for (unsigned i = 0; i < 45; ++i) {
        request_motion(3, 0, 0, 9, 0x7f);
        assert(memcmp(uart_tx.back().data(), U_PREPARE_GOLDEN[i], 44) == 0);
        assert(filament_ext[0][0].prepare_reply_index == i + 1);
    }
    request_motion(3, 0, 0, 9, 0x7f);
    expect_motion_feedback(0, 0, 4, 0x1e34);
    assert(uart_tx.back()[15] == 0xfd && uart_tx.back()[16] == 0x0a);
    assert(memcmp(uart_tx.back().data() + 17, Cxx_template + 17, 12) == 0);
    assert(uart_tx.back()[29] == 0x57);
    assert(memcmp(uart_tx.back().data() + 30, Cxx_template + 30, 8) == 0);
    for (unsigned i = 0; i < 6; ++i) {
        request_motion(i % 2 ? 4 : 3, 0, 0, 7, 0x7f);
        expect_motion_feedback(0, 0, 4, i < 5 ? 0x1e34 : 0x2b00);
    }
    request_motion(3, 0, 0xff, 1, 0);
    assert(get_now_filament_num() == 0 && filament_ext[0][0].motion == filament_motion_internal::on_use);
    expect_motion_feedback(0, 0, 4, 0x2b00);

    // 普通 C/D 包必须恢复模板；与协议报告中的整帧构造样例一致。
    const uint8_t normal_c[] = {
        0x3d,0xc0,0x2c,0xd9,0x03,0x00,0x00,0x04,0x00,0x00,0x00,0x80,0x3f,0x00,0x2b,0xff,
        0xff,0x36,0x00,0x00,0x00,0xf8,0xff,0xf7,0xff,0x00,0x00,0x27,0x00,0x57,0xfb,0xf4,
        0xf2,0xf6,0xb5,0xb7,0xb4,0xb1,0x00,0x00,0x00,0x00,0x46,0xda};
    const uint8_t normal_d[] = {
        0x3d,0xc8,0x3c,0x06,0x04,0x00,0xdc,0x00,0x14,0x0f,0x0f,0x0f,0x00,0x00,0x00,0x00,
        0x00,0x00,0x00,0x04,0x00,0x00,0x00,0x80,0x3f,0x00,0x2b,0xff,0xff,0x36,0x00,0x00,
        0x00,0xf9,0xff,0xf8,0xff,0x00,0x00,0x27,0x00,0x57,0xfb,0xf4,0xf2,0xf6,0xb5,0xb7,
        0xb4,0xb1,0x00,0x00,0x00,0x00,0xff,0xff,0xff,0xff,0x00,0xeb};
    package_num = 0;
    request_motion(3, 0, 0, 7, 0x7f);
    assert(memcmp(uart_tx.back().data(), normal_c, sizeof(normal_c)) == 0);
    sensor_online = true;
    request_motion(4, 0, 0, 7, 0x7f);
    assert(memcmp(uart_tx.back().data(), normal_d, sizeof(normal_d)) == 0);
    sensor_online = false;

    // Dxx 和其他 AMS 的探测不得消耗本通道的 Cxx 回放位置。
    request_motion(3, 0, 0, 9, 0x7f);
    assert(filament_ext[0][0].prepare_reply_index == 1);
    request_motion(4, 0, 0, 9, 0x7f);
    expect_motion_feedback(0, 0, 4, 0x1e34);
    request_motion(3, 1, 0xff, 1, 0);
    assert(filament_ext[0][0].prepare_reply_index == 1);
    auto prepare = short_request(3, 12);
    prepare[6] = 9; prepare[8] = 0x7f; seal(prepare);
    auto bad = prepare; bad.back() ^= 1;
    feed(bad); assert(BambuBus_run() == 2);
    assert(filament_ext[0][0].prepare_reply_index == 1);
    TX_IDLE = false; feed(prepare); assert(BambuBus_run() == 0);
    assert(filament_ext[0][0].prepare_reply_index == 1 && completed_frame_peek() >= 0);
    TX_IDLE = true; assert(BambuBus_run() == 1);
    assert(filament_ext[0][0].prepare_reply_index == 2);
    request_motion(3, 0, 0, 9, 0xa5);
    assert(filament_ext[0][0].prepare_reply_index == 0);
    request_motion(3, 0, 0, 9, 0x7f);
    expect_motion_feedback(0, 0, 4, 0x403d);
    request_motion(3, 2, 3, 3, 0); // 真正换 AMS，旧历史必须释放
    request_motion(3, 2, 3, 9, 0x7f);
    assert(filament_ext[0][0].prepare_reply_index == 0 && filament_ext[2][3].prepare_reply_index == 1);
    expect_motion_feedback(2, 3, 4, 0x403d);
    request_motion(4, 2, 3, 7, 0);
    assert(BambuBus_filament_stop_requested(11));
    expect_motion_feedback(2, 3, 4, 0x1e34);

    // 送出中的停止请求也必须有效，不能继续等待 15 秒；非当前通道不影响供料。
    for (const uint8_t command : {3, 4}) {
        reset_motion_fixture();
        request_motion(command, 0, 1, 3, 0);
        request_motion(command, 0, 1, 7, 0x7f);
        request_motion(command, 0, 2, 7, 0);
        assert(get_filament_motion(1) == need_send_out && !BambuBus_filament_stop_requested(1));
        request_motion(command, 0, 1, 7, 0);
        assert(BambuBus_filament_stop_requested(1) && get_filament_motion(1) == waiting);
        assert(!filament_ext[0][1].is_sendout_onuse_timing);
        expect_motion_feedback(0, 1, 4, 0x2b00);
    }
    assert(!BambuBus_filament_stop_requested(-1) && !BambuBus_filament_stop_requested(16));
}

/**
 * 验证两种长帧 flags 都经 UART 收包、命令处理和应答
 * 检查全部已支持长命令的地址交换、双字节序号回显、CRC 与应答长度
 */
static void test_long_flags() {
    for (uint8_t flag : {4, 5}) {
        for (auto rule : {std::pair<int, int>{0x103, 1}, {0x402, 34}, {0x211, 2}, {0x218, 34}, {0x21a, 1}}) {
            auto frame = long_request(rule.first, rule.second);
            frame[1] = flag; frame[2] = 0x34; frame[3] = 0x12;
            frame[9] = 0x56; frame[10] = 0x78; seal(frame);
            const auto previous = uart_tx.size();
            feed(frame); assert(BambuBus_run() == 1 && uart_tx.size() == previous + 1);
            const auto &reply = uart_tx.back();
            assert(reply[1] == 0 && reply[2] == 0x34 && reply[3] == 0x12);
            assert(reply[7] == 0x56 && reply[8] == 0x78 && reply[9] == 0 && reply[10] == 7);
            assert(reply[11] == frame[11] && reply[12] == frame[12]);
            assert((reply[4] | (reply[5] << 8)) == reply.size());
            assert(reply[6] == bus_crc8(reply.data(), 6) && package_check_crc16(reply.data(), reply.size()));
        }
    }
}

/**
 * 验证真实 BMCU_motion 对停止请求的处理
 * 请求先通过完整 BambuBus 收发，再执行电机主循环；检查立即停止、取消延迟及来源隔离
 */
static void test_motor_stop() {
    for (uint8_t command : {3, 4}) {
        reset_motion_fixture();
        channel_state[0] = {};
        motor_commands = 0;
        data_save.filament[0][0].statu = online;
        request_motion(command, 0, 0, 3, 0);
        BMCU_motion();
        assert(channel_state[0].motor.direction == 1 && motor_commands == 1);
        request_motion(command, 0, 0, 7, 0); // 送出时直接停止，不等待计时
        assert(motor_commands == 1); // 收发路径只修改状态
        BMCU_motion();
        assert(motors[0].stopped && motor_commands == 2);

        request_motion(command, 0, 0, 3, 0); BMCU_motion();
        request_motion(command, 0, 0, 9, 0xa5); BMCU_motion();
        assert(channel_state[0].feed_stop_pending && !motors[0].stopped);
        const auto deadline = channel_state[0].feed_stop_deadline;
        request_motion(command, 0, 0, 7, 0);
        assert(fake_ms < deadline);
        BMCU_motion();
        assert(motors[0].stopped && !channel_state[0].feed_stop_pending);
        assert(channel_state[0].feed_stop_deadline == 0);

        // 手动/辅助点动以及已经开始的定时退料保留原有互锁和截止行为。
        for (MotorSource source : {MOTOR_MANUAL, MOTOR_AUTOLOAD}) {
            channel_state[0].motor = {source, 1, true};
            const auto before = motor_commands;
            BMCU_motion();
            assert(motor_commands == before && channel_state[0].motor.source == source);
        }
        channel_state[0].motor = {MOTOR_PRINTER, -1, true};
        const auto before = motor_commands;
        BMCU_motion();
        assert(motor_commands == before && channel_state[0].motor.direction == -1);
    }
}

/**
 * 准备具有真实心跳和运动请求的暂停实验场景
 * @param start 模拟开始时间，支持零毫秒和回绕附近
 */
static void reset_pause_test_fixture(uint32_t start) {
    reset_motion_fixture();
    pause_test = PauseTest();
    BambuBus_need_to_save = false;
    fail_settings_save = false;
    std::fill(std::begin(CHANNEL_MODE), std::end(CHANNEL_MODE), CHANNEL_MODE_AUTO);
    AMS_COUNT = 4;
    fake_ms = start;
    feed(short_request(0x20, 33));
    assert(BambuBus_run() == 1);
}

/** 验证堵转反馈、AMS 隔离、退料离线顺序、保存失败重试、手动解除和恢复模式 */
static void test_pause_test() {
    assert(!BambuBus_request_pause_test(true));
    reset_pause_test_fixture(0);
    data_save.BambuBus_now_filament_num = 6;
    assert(!BambuBus_pause_test_status().available); // Flash 中的旧选择不能用于触发。
    request_motion(3, 1, 2, 9, 0x7f);
    assert(!BambuBus_request_pause_test(true)); // 使用前准备不能触发。

    for (uint8_t command : {3, 4}) {
        reset_pause_test_fixture(1000);
        request_motion(command, 1, 2, 7, 0x7f);
        const auto normal = uart_tx.back();
        const auto record = data_save;
        const auto writes = write_count;
        assert(BambuBus_pause_test_status().available);
        assert(BambuBus_pause_test_status().candidate_channel == 6);
        assert(BambuBus_request_pause_test(true));
        assert(!BambuBus_request_pause_test(true));
        assert(BambuBus_pause_test_status().pending == 1 && !pause_test.active);
        BambuBus_run();
        assert(pause_test.active && !pause_test.frames);
        request_motion(command, 1, 2, 7, 0x7f);
        auto expected = normal;
        expected[1] = uart_tx.back()[1];
        const size_t offset = command == 3 ? 13u : 25u;
        expected[offset] = 0x6f; expected[offset + 1] = 0xf0;
        expected[offset + 2] = 0xe7; expected[offset + 3] = 0x1c;
        seal(expected);
        assert(uart_tx.back() == expected); // 仅压力和相邻字段改变，封装与 CRC 仍正确。
        assert(pause_test.frames == 1 && memcmp(&record, &data_save, sizeof(record)) == 0);
        request_motion(command == 3 ? 4 : 3, 1, 2, 7, 0x7f);
        expect_motion_feedback(1, 2, 4, 0xf06f);
        assert(pause_test.frames == 2); // 交替 Cxx/Dxx 使用同一测试目标。
        request_motion(command, 0, 0xff, 1, 0);
        expect_motion_feedback(0, 0xff, 0, 0xff74);
        assert(pause_test.frames == 2);
        auto bad = short_request(command, command == 3 ? 12 : 13);
        bad.back() ^= 1;
        feed(bad); assert(BambuBus_run() == 2 && pause_test.frames == 2);
        TX_IDLE = false;
        BambuBus_run();
        assert(pause_test.frames == 2);
        TX_IDLE = true;
        assert(BambuBus_request_pause_test(false) && pause_test.active);
        BambuBus_run();
        assert(!pause_test.active && !strcmp(pause_test.result, "cleared"));
        request_motion(command, 1, 2, 7, 0x7f);
        expected = normal; expected[1] = uart_tx.back()[1]; seal(expected);
        assert(uart_tx.back() == expected && write_count == writes);
        assert(BambuBus_request_pause_test(true));
        request_motion(command, 1, 2, 7, 0x7f);
        for (int i = 0; i < active_channel_count(); ++i) set_filament_online(i / 4, i % 4, true);
        request_motion(command, 1, 2, 7, 0);
        assert(pause_test.active && pause_test.stop_seen && !pause_test.offline_pending);
        request_motion(command, 1, 2, 9, 0x3f);
        expect_motion_feedback(1, 2, 4, 0xf06f);
        assert(CHANNEL_MODE[6] == CHANNEL_MODE_AUTO && is_filament_online(6));
        request_motion(command, 0, 0xff, 3, 0); // 其他 AMS 的退料轮询不得使目标离线。
        assert(!pause_test.offline_pending);
        const auto settings_writes = settings_write_count;
        request_motion(command, 1, 0xff, 3, 0);
        expect_motion_feedback(1, 2, 2, 0xf06f);
        assert(pause_test.active && pause_test.offline_pending);
        assert(CHANNEL_MODE[6] == CHANNEL_MODE_OFFLINE && !is_filament_online(6));
        assert(settings_write_count == settings_writes); // 收发处理器不能直接写 Flash。
        for (int i = 0; i < active_channel_count(); ++i) if (i != 6) {
            assert(CHANNEL_MODE[i] == CHANNEL_MODE_AUTO && is_filament_online(i));
        }
        assert((uart_tx.back()[command == 3 ? 29 : 41] & 0x30) == 0);
        if (command == 4) for (int i : {9, 10, 11}) assert((uart_tx.back()[i] & 4) == 0);
        BambuBus_save_run(); // 刚回复不能写入，等静默窗口。
        assert(settings_write_count == settings_writes);
        fake_ms += 1001;
        TX_IDLE = false; BambuBus_save_run(); TX_IDLE = true;
        assert(settings_write_count == settings_writes);
        fail_settings_save = true;
        BambuBus_save_run();
        assert(settings_write_count == settings_writes + 1 && pause_test.active);
        assert(!strcmp(pause_test.result, "save_failed"));
        fake_ms += 999; BambuBus_save_run();
        assert(settings_write_count == settings_writes + 1); // 写入失败也需要冷却。
        fail_settings_save = false;
        fake_ms += 1; BambuBus_save_run();
        assert(settings_write_count == settings_writes + 2 && !pause_test.active && !pause_test.offline_pending);
        assert(!strcmp(pause_test.result, "offline") && saved_channel_modes[6] == CHANNEL_MODE_OFFLINE);
        for (int i = 0; i < active_channel_count(); ++i) if (i != 6) assert(saved_channel_modes[i] == CHANNEL_MODE_AUTO);
        for (uint8_t reply_type : {3, 4}) {
            request_motion(reply_type, 1, 0xff, 3, 0);
            expect_motion_feedback(1, 2, 2, 0x4700);
            assert((uart_tx.back()[reply_type == 3 ? 29 : 41] & 0x30) == 0);
            if (reply_type == 4) for (int i : {9, 10, 11}) assert((uart_tx.back()[i] & 4) == 0);
        }
        assert(BambuBus_request_pause_test(false)); BambuBus_run();
        assert(CHANNEL_MODE[6] == CHANNEL_MODE_OFFLINE); // 解除测试不会恢复通道。
        request_motion(command, 1, 2, 3, 0);
        request_motion(command, 1, 2, 9, 0x7f);
        assert((uart_tx.back()[command == 3 ? 29 : 41] & 0x30) == 0); // 准备表也不能把离线通道重新报在线。
        request_motion(command, 1, 2, 9, 0xa5);
        request_motion(command, 1, 2, 7, 0x7f);
        assert(!BambuBus_request_pause_test(true)); // 离线通道的旧使用请求不能再次触发。
        fake_ms += 1001; BambuBus_save_run();
        assert(settings_write_count == settings_writes + 2); // 一次成功只保存一次。
        BambuBus_init();
        assert(CHANNEL_MODE[6] == CHANNEL_MODE_OFFLINE && saved_channel_modes[6] == CHANNEL_MODE_OFFLINE);

    }

    for (int restored_mode : {CHANNEL_MODE_ONLINE, CHANNEL_MODE_AUTO}) {
        reset_pause_test_fixture(10000);
        request_motion(3, 1, 2, 7, 0x7f);
        assert(BambuBus_request_pause_test(true)); BambuBus_run();
        request_motion(3, 1, 0xff, 3, 0);
        assert(pause_test.offline_pending);
        CHANNEL_MODE[6] = restored_mode; // 等效于网页成功保存后的运行期设置。
        const int writes = settings_write_count;
        BambuBus_save_run();
        assert(!pause_test.active && !pause_test.offline_pending && !strcmp(pause_test.result, "restored"));
        assert(settings_write_count == writes && CHANNEL_MODE[6] == restored_mode);
    }

    for (uint32_t start : {0u, UINT32_MAX - 500u}) {
        reset_pause_test_fixture(start);
        request_motion(3, 0, 0, 7, 0x7f);
        assert(BambuBus_request_pause_test(true));
        BambuBus_run();
        for (uint32_t elapsed = 1000; elapsed < 60000; elapsed += 1000) {
            fake_ms = start + elapsed;
            feed(short_request(0x20, 33)); assert(BambuBus_run() == 1);
            request_motion(4, 0, 0, 7, 0x7f);
            assert(pause_test.active && !BambuBus_request_pause_test(true));
            expect_motion_feedback(0, 0, 4, 0xf06f);
        }
        fake_ms = start + 3600000;
        TX_IDLE = false; // 长时间断线或无发送槽都不清除故障。
        BambuBus_run();
        TX_IDLE = true;
        assert(pause_test.active);
        request_motion(3, 0, 0, 7, 0x7f);
        expect_motion_feedback(0, 0, 4, 0xf06f);
        assert(BambuBus_request_pause_test(false)); BambuBus_run();
        request_motion(3, 0, 0, 7, 0x7f);
        expect_motion_feedback(0, 0, 4, 0x2b00);
    }

    reset_pause_test_fixture(1000);
    request_motion(3, 0, 0, 7, 0x7f);
    assert(BambuBus_request_pause_test(true));
    assert(BambuBus_request_pause_test(false)); // 主循环执行前解除，不能发出任何异常反馈。
    BambuBus_run();
    assert(!pause_test.active && pause_test.frames == 0);
    assert(BambuBus_request_pause_test(true));
    set_filament_motion(0, waiting);
    BambuBus_run();
    assert(!pause_test.active && !strcmp(pause_test.result, "rejected"));
    request_motion(3, 0, 0, 7, 0x7f);
    assert(BambuBus_request_pause_test(true)); BambuBus_run();
    fake_ms += 1001;
    BambuBus_run();
    assert(pause_test.active);
    assert(!BambuBus_request_pause_test(true));
    assert(BambuBus_request_pause_test(false)); BambuBus_run(); // 断线时允许解除。

    reset_pause_test_fixture(1000);
    request_motion(3, 3, 3, 7, 0x7f);
    assert(BambuBus_request_pause_test(true)); BambuBus_run();
    AMS_COUNT = 1;
    BambuBus_run();
    assert(!pause_test.active && !strcmp(pause_test.result, "target_disabled"));
    reset_pause_test_fixture(1000);
    request_motion(3, 0, 0, 7, 0x7f);
    assert(BambuBus_request_pause_test(true)); BambuBus_run();
    request_motion(3, 0, 0, 9, 0xa5);
    assert(pause_test.active);
    expect_motion_feedback(0, 0, 4, 0xf06f);
    assert(BambuBus_request_pause_test(false)); BambuBus_run();
    request_motion(3, 0, 0, 7, 0x7f);
    fake_ms += 1001;
    feed(short_request(0x20, 33)); BambuBus_run();
    assert(!BambuBus_request_pause_test(true)); // 心跳不能让旧运动状态重新变新鲜。
    request_motion(3, 0, 0, 7, 0x7f);
    assert(BambuBus_request_pause_test(true)); BambuBus_run();
    BambuBus_init();
    assert(!pause_test.active && pause_test.channel == -1 && !BambuBus_pause_test_status().available);
}

/**
 * 执行指定回归场景
 * @param argc 参数数量，必须包含一个场景名
 * @param argv 程序名和场景名
 * @return 0 表示通过，2 表示场景不存在
 * 每个场景单独进程执行，保证函数局部静态状态从真实启动初值开始
 */
int main(int argc, char **argv) {
    assert(argc == 2);
    const std::string name = argv[1];
    if (name == "parser") test_parser();
    else if (name == "queue") test_queue();
#ifndef BMCU_TEST_WASI
    else if (name == "queue_threads") test_queue_threads();
#endif
    else if (name == "receiver") test_receiver();
    else if (name == "timeout") test_timeout();
    else if (name == "save") test_save();
    else if (name == "zero_time") test_zero_time();
    else if (name == "registration") test_registration();
    else if (name == "serial_identity") test_serial_identity();
    else if (name == "ams_version") test_ams_version();
    else if (name == "responses") test_responses();
    else if (name == "motion_sequences") test_motion_sequences();
    else if (name == "motion_isolation") test_motion_isolation();
    else if (name == "motion_timing") test_motion_timing();
    else if (name == "tx_backpressure") test_tx_backpressure();
    else if (name == "tx_lifetime") test_tx_lifetime();
    else if (name == "u_compatibility") test_u_compatibility();
    else if (name == "long_flags") test_long_flags();
    else if (name == "auth_logging") test_auth_logging();
    else if (name == "motor_stop") test_motor_stop();
    else if (name == "pause_test") test_pause_test();
    else return 2;
    std::printf("PASS %s\n", name.c_str());
}
