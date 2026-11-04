/**
 * 重置硬件桩与设置
 * @param start_ms 起始时钟
 * @param ams 启用 AMS 数量
 */
static void reset_hall_fixture(uint32_t start_ms = 0, int ams = 1) {
    fake_ms = start_ms;
    AMS_COUNT = ams;
    IO_CONFIG.switch_pin = PIN_NONE;
    for (int ch = 0; ch < IO_CONFIG_MAX_CHANNELS; ++ch) {
        IO_CONFIG.motor[ch][0] = IO_CONFIG.motor[ch][1] = PIN_NONE;
        IO_CONFIG.microswitch[ch] = IO_CONFIG.hall[ch] = PIN_NONE;
    }
    adc_reads = 0;
    memset(pin_reads, 0, sizeof(pin_reads));
    memset(prepares, 0, sizeof(prepares));
    for (auto &available : adc_available) available = true;
    fail_read = ramp_values = false;
    forced_value = -1;
    observed_samples.clear();
    input_pins.clear();
}

/**
 * 执行一次真实调度并限制单轮 ADC 调用数量
 */
static void run_hall_once() {
    const auto before = adc_reads;
    hall_sensor_run();
    assert(adc_reads - before <= 1);
}

/**
 * 按毫秒推进主循环，覆盖一轮中的多个已配置通道
 * @param duration 要推进的毫秒数
 */
static void run_hall_for(uint32_t duration) {
    for (uint32_t elapsed = 0; elapsed < duration; ++elapsed) {
        for (int pass = 0; pass < 6; ++pass) run_hall_once();
        ++fake_ms;
    }
}

/**
 * 验证 20Hz 采样和首个 200ms 桶的独立统计结果
 */
static void test_hall_sampling() {
    reset_hall_fixture();
    IO_CONFIG.hall[0] = 0;
    IO_CONFIG.hall[1] = 1;
    ramp_values = true;
    hall_sensor_init();
    assert(!hall_sensor_status(0).valid);
    run_hall_for(200);
    run_hall_once();
    const auto info = hall_sensor_history_info();
    assert(info.count == 1 && info.channels == 4);
    assert(info.end_ms == 200 && info.now_ms == 200);
    for (int ch = 0; ch < 2; ++ch) {
        HallHistoryPoint point{};
        assert(hall_sensor_history_point(ch, info.cursor, point));
        uint32_t sum = 0;
        unsigned count = 0;
        uint16_t low = UINT16_MAX, high = 0;
        for (const auto &sample : observed_samples) {
            if (sample.pin != ch || sample.time >= 200) continue;
            sum += sample.value;
            ++count;
            low = min(low, sample.value);
            high = max(high, sample.value);
        }
        assert(count == 4);
        assert(point.minimum == low && point.maximum == high);
        assert(point.average == (sum + count / 2) / count);
        const auto status = hall_sensor_status(ch);
        assert(status.configured && status.valid && status.statistics_valid);
        assert(status.minimum == low && status.maximum == high && status.average == point.average);
    }
    run_hall_for(800);
    for (int pin = 0; pin < 2; ++pin) assert(pin_reads[pin] >= 20 && pin_reads[pin] <= 21);
    const auto empty = hall_sensor_status(2);
    assert(!empty.configured && !empty.valid && !empty.statistics_valid && empty.pin == PIN_NONE);
    assert(!hall_sensor_status(-1).configured && !hall_sensor_status(16).configured);
    HallHistoryPoint point{};
    assert(!hall_sensor_history_point(-1, info.cursor, point));
    assert(!hall_sensor_history_point(16, info.cursor, point));
}

/**
 * 验证只保留 300 桶且高编号逻辑通道正确映射 ADC 输入
 */
static void test_hall_capacity() {
    static_assert(sizeof(slots) <= 10240, "C3 霍尔历史不得按十六逻辑通道分配");
    reset_hall_fixture(0, 4);
    for (int pin = 0; pin < 4; ++pin) IO_CONFIG.hall[12 + pin] = pin;
    IO_CONFIG.hall[0] = 4;
    hall_sensor_init();
    run_hall_for(60400);
    run_hall_once();
    const auto info = hall_sensor_history_info();
    assert(info.count == 300 && info.end_ms == 60400 && info.channels == 16);
    HallHistoryPoint point{};
    assert(!hall_sensor_history_point(12, info.cursor - 300, point));
    assert(!hall_sensor_history_point(12, info.cursor + 1, point));
    for (int offset = 0; offset < 300; ++offset) {
        assert(hall_sensor_history_point(12, info.cursor - offset, point));
        assert(point.average == 1000 && point.minimum == 1000 && point.maximum == 1000);
        assert(hall_sensor_history_point(15, info.cursor - offset, point));
        assert(point.average == 1300);
    }
    assert(hall_sensor_status(0).raw == 1400);
    assert(hall_sensor_status(15).statistics_valid && hall_sensor_status(15).average == 1300);
    std::printf("C3 Hall history slots: %zu bytes\n", sizeof(slots));
}

/**
 * 验证调度长暂停留下空桶，失败读取使当前值过期且一分钟后旧统计退出
 */
static void test_hall_gaps() {
    reset_hall_fixture();
    IO_CONFIG.hall[0] = 0;
    hall_sensor_init();
    run_hall_for(200);
    run_hall_once();
    const auto initial = hall_sensor_history_info();
    fail_read = true;
    fake_ms = 5200;
    run_hall_once();
    auto info = hall_sensor_history_info();
    assert(info.count == 26 && info.cursor - initial.cursor == 25);
    const auto stale = hall_sensor_status(0);
    assert(stale.configured && !stale.valid);
    HallHistoryPoint point{};
    assert(hall_sensor_history_point(0, initial.cursor, point) && point.average == 1000);
    assert(hall_sensor_history_point(0, info.cursor, point));
    assert(point.average == HALL_INVALID_VALUE && point.minimum == HALL_INVALID_VALUE && point.maximum == HALL_INVALID_VALUE);
    const auto before = adc_reads;
    fake_ms = 125200;
    run_hall_once();
    assert(adc_reads - before <= 1);
    info = hall_sensor_history_info();
    assert(info.count == 300);
    assert(!hall_sensor_history_point(0, initial.cursor, point));
    assert(!hall_sensor_status(0).valid && !hall_sensor_status(0).statistics_valid);
    fail_read = false;
    run_hall_for(400);
    run_hall_once();
    assert(hall_sensor_status(0).valid && hall_sensor_status(0).statistics_valid);
}

/**
 * 验证接线、启用数量和重启使历史换代，未配置或初始化失败不产生有效值
 */
static void test_hall_configuration() {
    reset_hall_fixture(0, 2);
    IO_CONFIG.hall[7] = 0;
    hall_sensor_init();
    run_hall_for(400);
    run_hall_once();
    const auto original = hall_sensor_history_info();
    assert(hall_sensor_status(7).valid);
    IO_CONFIG.hall[7] = 1;
    hall_sensor_sync_config();
    auto changed = hall_sensor_history_info();
    assert(changed.epoch != original.epoch && changed.count == 0);
    assert(hall_sensor_status(7).pin == 1 && !hall_sensor_status(7).valid);
    run_hall_for(200);
    run_hall_once();
    assert(hall_sensor_status(7).raw == 1100);
    AMS_COUNT = 1;
    run_hall_once();
    const auto reduced = hall_sensor_history_info();
    assert(reduced.epoch != changed.epoch && reduced.channels == 4);
    assert(!hall_sensor_status(7).configured);
    IO_CONFIG.hall[0] = 5;
    IO_CONFIG.hall[1] = 50;
    const auto before = adc_reads;
    run_hall_for(400);
    assert(adc_reads == before && !hall_sensor_status(0).valid && !hall_sensor_status(1).valid);
    IO_CONFIG.hall[0] = 2;
    adc_available[2] = false;
    hall_sensor_sync_config();
    run_hall_for(400);
    assert(!hall_sensor_status(0).valid);
    const auto previous_epoch = hall_sensor_history_info().epoch;
    hall_sensor_init();
    assert(hall_sensor_history_info().epoch != previous_epoch && hall_sensor_history_info().count == 0);
}

/**
 * 验证毫秒回绕不破坏采样间隔、桶时间和过期判断
 */
static void test_hall_wrap() {
    reset_hall_fixture(UINT32_MAX - 199);
    IO_CONFIG.hall[0] = 0;
    hall_sensor_init();
    run_hall_for(600);
    run_hall_once();
    const auto info = hall_sensor_history_info();
    assert(info.count == 3 && info.end_ms == 400);
    HallHistoryPoint point{};
    assert(hall_sensor_history_point(0, info.cursor, point));
    assert(point.average == 1000 && hall_sensor_status(0).valid);
    fail_read = true;
    fake_ms += 501;
    assert(!hall_sensor_status(0).valid);
}

/**
 * 验证霍尔接线交给电机后不会被采集模块重新改成输入
 * 同时检查重复 ADC 的运行期保护及无关设置修改不会重置历史
 */
static void test_hall_ownership() {
    reset_hall_fixture();
    IO_CONFIG.hall[0] = 0;
    IO_CONFIG.hall[1] = 1;
    hall_sensor_init();
    run_hall_for(400);
    run_hall_once();
    const auto before = hall_sensor_history_info();
    IO_CONFIG.motor[3][0] = 8;
    hall_sensor_sync_config();
    assert(hall_sensor_history_info().epoch == before.epoch);
    assert(hall_sensor_history_info().cursor == before.cursor);
    input_pins.clear();
    IO_CONFIG.motor[0][0] = 0;
    IO_CONFIG.hall[0] = 2;
    hall_sensor_sync_config();
    assert(std::find(input_pins.begin(), input_pins.end(), 0) == input_pins.end());
    assert(hall_sensor_status(0).pin == 2);
    input_pins.clear();
    IO_CONFIG.hall[0] = PIN_NONE;
    hall_sensor_sync_config();
    assert(std::find(input_pins.begin(), input_pins.end(), 2) == input_pins.end());
    IO_CONFIG.hall[2] = 1;
    hall_sensor_sync_config();
    assert(hall_sensor_status(1).configured && !hall_sensor_status(2).configured);
    const auto duplicate_reads = pin_reads[1];
    run_hall_for(200);
    assert(pin_reads[1] - duplicate_reads == 4);
}

/**
 * 用真实时钟推进跨越统计游标上限，验证环形索引不依赖游标绝对大小
 */
static void test_hall_cursor_wrap() {
    reset_hall_fixture();
    IO_CONFIG.hall[0] = 0;
    hall_sensor_init();
    for (int pass = 0; pass < 214; ++pass) {
        fake_ms += 4000000000u;
        run_hall_once();
    }
    const uint32_t remaining = (UINT32_MAX - 1u) - hall_sensor_history_info().cursor;
    fake_ms += remaining * HALL_HISTORY_INTERVAL_MS;
    run_hall_once();
    assert(hall_sensor_history_info().cursor == UINT32_MAX - 1u);
    run_hall_for(600);
    run_hall_once();
    const auto info = hall_sensor_history_info();
    assert(info.cursor == 1u && info.count == HALL_HISTORY_CAPACITY);
    for (uint32_t cursor : {UINT32_MAX, 0u, 1u}) {
        HallHistoryPoint point{};
        assert(hall_sensor_history_point(0, cursor, point));
        assert(point.average == 1000 && point.minimum == 1000 && point.maximum == 1000);
    }
    HallHistoryPoint point{};
    assert(!hall_sensor_history_point(0, 2u, point));
    server.params = {{"epoch", String(info.epoch)}, {"cursor", String(UINT32_MAX)}};
    const auto reads = adc_reads;
    handle_hall();
    assert(adc_reads == reads && server.code == 200);
    assert(server.body.find("\"cursor\":1,\"reset\":false,\"more\":false") != String::npos);
}

/**
 * 验证零点和满量程都是有效读数，越界转换不会污染统计或继续显示旧当前值
 */
static void test_hall_limits() {
    reset_hall_fixture();
    IO_CONFIG.hall[0] = 0;
    forced_value = 0;
    hall_sensor_init();
    run_hall_for(200);
    forced_value = 4095;
    run_hall_once();
    HallHistoryPoint point{};
    assert(hall_sensor_history_point(0, hall_sensor_history_info().cursor, point));
    assert(point.average == 0 && point.minimum == 0 && point.maximum == 0);
    assert(hall_sensor_status(0).valid && hall_sensor_status(0).raw == 4095);
    run_hall_for(200);
    forced_value = 4096;
    run_hall_once();
    const auto limits = hall_sensor_status(0);
    assert(!limits.valid && limits.statistics_valid);
    assert(limits.minimum == 0 && limits.maximum == 4095 && limits.average == 2048);
    run_hall_for(200);
    run_hall_once();
    assert(hall_sensor_history_point(0, hall_sensor_history_info().cursor, point));
    assert(point.average == HALL_INVALID_VALUE);
    forced_value = 0;
    run_hall_for(50);
    run_hall_once();
    assert(hall_sensor_status(0).valid && hall_sensor_status(0).raw == 0);
}

/**
 * 输出一份真实 HTTP 响应供 Python 独立解析
 * @param name 本次请求标签
 * @param params 查询参数
 * 检查读取接口没有采样、改动游标或配置，并确认成功响应禁止缓存
 */
static void emit_hall_response(const char *name, const std::map<String, String> &params = {}) {
    server.params = params;
    server.headers.clear();
    const auto before = hall_sensor_history_info();
    const auto reads = adc_reads;
    handle_hall();
    const auto after = hall_sensor_history_info();
    assert(adc_reads == reads && before.epoch == after.epoch && before.cursor == after.cursor &&
           before.end_ms == after.end_ms && before.count == after.count);
    if (server.code == 200) assert(server.headers["Cache-Control"] == "no-store");
    std::printf("HTTP\t%s\t%d\t%s\n", name, server.code, server.body.c_str());
}

/**
 * 生成完整分页、增量、参数错误、空档和接线换代的真实接口响应
 */
static void test_hall_http() {
    reset_hall_fixture();
    IO_CONFIG.hall[0] = 0;
    IO_CONFIG.hall[1] = 1;
    hall_sensor_init();
    const auto epoch = hall_sensor_history_info().epoch;
    emit_hall_response("empty");
    run_hall_for(60000);
    run_hall_once();
    emit_hall_response("page1");
    for (int page = 2; page <= 6; ++page) {
        const auto label = "page" + std::to_string(page);
        emit_hall_response(label.c_str(), {{"epoch", String(epoch)}, {"cursor", String((page - 1) * 50)}});
    }
    run_hall_for(1000);
    run_hall_once();
    emit_hall_response("increment", {{"epoch", String(epoch)}, {"cursor", "300"}});
    emit_hall_response("unchanged", {{"epoch", String(epoch)}, {"cursor", "305"}});
    emit_hall_response("old_epoch", {{"epoch", String(epoch - 1)}, {"cursor", "300"}});
    emit_hall_response("future", {{"epoch", String(epoch)}, {"cursor", "306"}});
    emit_hall_response("too_old", {{"epoch", String(epoch)}, {"cursor", "4"}});
    emit_hall_response("oldest_edge", {{"epoch", String(epoch)}, {"cursor", "5"}});
    emit_hall_response("bad_missing_epoch", {{"cursor", "305"}});
    emit_hall_response("bad_missing_cursor", {{"epoch", String(epoch)}});
    int index = 0;
    for (const char *invalid : {"", "-1", "+1", "1x", "0x01", "1.5", "4294967296", " 1", "1 "}) {
        const auto label = "bad_cursor_" + std::to_string(index++);
        emit_hall_response(label.c_str(), {{"epoch", String(epoch)}, {"cursor", invalid}});
    }
    emit_hall_response("bad_epoch", {{"epoch", "4294967296"}, {"cursor", "305"}});
    fail_read = true;
    run_hall_for(1000);
    run_hall_once();
    emit_hall_response("failed", {{"epoch", String(epoch)}, {"cursor", "305"}});
    IO_CONFIG.hall[0] = 2;
    hall_sensor_sync_config();
    emit_hall_response("changed", {{"epoch", String(epoch)}, {"cursor", "310"}});
}

/**
 * 运行指定霍尔场景
 * @param argc 参数数量
 * @param argv 测试程序与场景名
 * @return 0 表示场景通过
 */
int main(int argc, char **argv) {
    assert(argc == 2);
    const std::string scenario = argv[1];
    if (scenario == "sampling") test_hall_sampling();
    else if (scenario == "capacity") test_hall_capacity();
    else if (scenario == "gaps") test_hall_gaps();
    else if (scenario == "configuration") test_hall_configuration();
    else if (scenario == "wrap") test_hall_wrap();
    else if (scenario == "ownership") test_hall_ownership();
    else if (scenario == "cursor_wrap") test_hall_cursor_wrap();
    else if (scenario == "limits") test_hall_limits();
    else if (scenario == "http") test_hall_http();
    else assert(false);
    std::printf("PASS hall_%s\n", argv[1]);
}
