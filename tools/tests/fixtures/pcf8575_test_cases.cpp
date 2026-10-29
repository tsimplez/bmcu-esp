/**
 * 连接四片芯片并校验初始化只写低电平
 * 每片独立初始化，连续调用不能重置已经在线的输出
 */
static void connect_all() {
    bus_ready = true;
    for (int device = 0; device < 4; ++device) connected[device] = true;
    assert(PCF8575_init());
    assert(chip_writes.size() == 4);
    for (int device = 0; device < 4; ++device) {
        assert(attempts[device] == 1 && PCF8575_is_online(device));
        assert(chip_writes[device].address == 0x20 + device && chip_writes[device].value == 0);
    }
}

/** 验证共享总线未初始化时不探测、不读写，非法引脚不落到真实 GPIO */
static void test_disabled() {
    assert(!PCF8575_init());
    now_ms = 6000;
    PCF8575_poll();
    for (int pin = 50; pin <= 113; ++pin) {
        pinMode_PCF8575(pin, OUTPUT);
        digitalWrite_PCF8575(pin, HIGH);
        assert(!PCF8575_pin_is_online(pin));
        assert(digitalRead_PCF8575(pin) == LOW);
    }
    PCF8575_update();
    for (int pin : {49, 114, 255}) {
        assert(!IS_PCF8575_VIRTUAL_PIN(pin));
        pinMode_PCF8575(pin, OUTPUT);
        digitalWrite_PCF8575(pin, HIGH);
        analogWrite_PCF8575(pin, 255);
        assert(digitalRead_PCF8575(pin) == LOW);
    }
    assert(chip_writes.empty() && gpio_calls == 0);
    for (int device = 0; device < 4; ++device) assert(attempts[device] == 0);
    assert(!PCF8575_is_online(4) && !PCF8575_pin_is_online(255));
}

/** 验证全部 64 位实际寻址、P10～P17 位序、缓存隔离和输入读取后的输出恢复 */
static void test_all() {
    connect_all();
    for (int device = 0; device < 4; ++device) {
        for (int bit = 0; bit < 16; ++bit) {
            const int pin = 50 + device * 16 + bit;
            const uint16_t mask = uint16_t(1U << bit);
            assert(VIRTUAL_TO_PCF8575_PIN(pin) == (bit < 8 ? bit : bit + 2));
            chip_writes.clear();
            pinMode_PCF8575(pin, OUTPUT);
            digitalWrite_PCF8575(pin, HIGH);
            assert(chip_writes.size() == 1);
            assert(chip_writes[0].address == 0x20 + device && chip_writes[0].value == mask);
            assert(digitalRead_PCF8575(pin) == HIGH && chip_reads[device] == bit * 2);
            pinMode_PCF8575(pin, INPUT);
            input_mask[device] = 0;
            assert(digitalRead_PCF8575(pin) == LOW);
            input_mask[device] = 0xFFFF;
            assert(digitalRead_PCF8575(pin) == HIGH);
            assert(output_latch[device] == mask);
            digitalWrite_PCF8575(pin, LOW);
            for (const auto &write : chip_writes) assert(write.address == 0x20 + device);
            for (int other = 0; other < 4; ++other) assert(output_latch[other] == 0);
        }
    }
    // 相同位在不同芯片上可分别作为输入、输出，并保留独立的输出锁存。
    for (int device = 0; device < 4; ++device) {
        pinMode_PCF8575(50 + device * 16, OUTPUT);
        digitalWrite_PCF8575(50 + device * 16, HIGH);
    }
    pinMode_PCF8575(66, INPUT);
    input_mask[1] = 0;
    assert(digitalRead_PCF8575(50) == HIGH && digitalRead_PCF8575(66) == LOW);
    chip_writes.clear();
    assert(PCF8575_init());
    assert(chip_writes.empty());
    PCF8575_update();
    assert(chip_writes.size() == 4);
    for (int device = 0; device < 4; ++device) assert(output_latch[device] == 1);
    pinMode_PCF8575(2, OUTPUT);
    digitalWrite_PCF8575(2, HIGH);
    assert(digitalRead_PCF8575(2) == HIGH);
    analogWrite_PCF8575(2, 128);
    assert(gpio_calls == 4);
}

/** 验证缺片不借用在线状态，离线独立重试、时间回绕和恢复时的低输出及模式保留 */
static void test_sparse() {
    bus_ready = true;
    connected[1] = connected[3] = true;
    fail_write[3] = true;
    pinMode_PCF8575(98, OUTPUT);
    digitalWrite_PCF8575(98, HIGH);
    assert(PCF8575_init());
    assert(!PCF8575_is_online() && PCF8575_is_online(1));
    assert(!PCF8575_is_online(2) && !PCF8575_is_online(3));
    for (int ch = 0; ch < 16; ++ch) {
        IO_CONFIG.motor[ch][0] = 50 + ch / 4 * 16;
        IO_CONFIG.motor[ch][1] = IO_CONFIG.motor[ch][0] + 1;
        IO_CONFIG.microswitch[ch] = IO_CONFIG.motor[ch][0] + 15;
        CHANNEL_MODE[ch] = CHANNEL_MODE_AUTO;
        assert(BMCU_channel_motor_ready(ch) == (ch / 4 == 1));
    }
    input_mask[1] = 0;
    BMCU_apply_channel_modes();
    for (int ch = 0; ch < 16; ++ch) assert(filament_online[ch] == (ch / 4 == 1));
    IO_CONFIG.motor[0][1] = 66;
    assert(!BMCU_channel_motor_ready(0));
    IO_CONFIG.motor[0][0] = 2;
    assert(BMCU_channel_motor_ready(0));
    IO_CONFIG.motor[0][1] = PIN_NONE;
    assert(!BMCU_channel_motor_ready(0));
    pinMode_PCF8575(66, OUTPUT);
    digitalWrite_PCF8575(66, HIGH);
    digitalWrite_PCF8575(98, HIGH); // 离线输出不得在恢复后自动重放
    fail_write[3] = false;
    now_ms = 4999;
    PCF8575_poll();
    assert(attempts[0] == 1 && attempts[3] == 1);
    now_ms = 5000;
    PCF8575_poll();
    assert(attempts[0] == 2 && attempts[1] == 1 && attempts[2] == 2 && attempts[3] == 2);
    assert(PCF8575_is_online(3) && output_latch[1] == 1 && output_latch[3] == 0);
    digitalWrite_PCF8575(98, HIGH);
    assert(digitalRead_PCF8575(98) == HIGH && chip_reads[3] == 0); // 原输出模式仍保留
    now_ms = UINT32_MAX - 99;
    PCF8575_init();
    const int before = attempts[0];
    connected[0] = connected[2] = true;
    now_ms = 4899;
    PCF8575_poll();
    assert(attempts[0] == before);
    now_ms = 4900;
    PCF8575_poll();
    assert(attempts[0] == before + 1 && PCF8575_is_online(0) && PCF8575_is_online(2));
    assert(output_latch[1] == 1 && output_latch[3] == 1);
}

/** 验证同一时刻多片、多引脚 PWM 不互相吞掉更新，端点值立即生效 */
static void test_pwm() {
    connect_all();
    now_ms = 2;
    for (int pin : {50, 51, 66, 82, 98, 113}) analogWrite_PCF8575(pin, 128);
    assert(output_latch[0] == 3 && output_latch[1] == 1 && output_latch[2] == 1 && output_latch[3] == 0x8001);
    analogWrite_PCF8575(113, 0);
    analogWrite_PCF8575(65, 255);
    assert(output_latch[3] == 1 && output_latch[0] == 0x8003);
}

/**
 * 运行一个独立进程内的 PCF 回归场景
 * @param argc 参数数量
 * @param argv 场景名称
 * @return 0 表示断言全部通过
 */
int main(int argc, char **argv) {
    assert(argc == 2);
    if (!strcmp(argv[1], "disabled")) test_disabled();
    else if (!strcmp(argv[1], "all")) test_all();
    else if (!strcmp(argv[1], "sparse")) test_sparse();
    else if (!strcmp(argv[1], "pwm")) test_pwm();
    else assert(false);
    std::printf("PASS: PCF8575 %s\n", argv[1]);
}
