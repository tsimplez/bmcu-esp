/**
 * 比较两个完整快照
 * @param expected 预期状态
 * @param actual 实际状态
 * 所有字段包含 CRC 必须逐字节一致
 */
static void same_settings(const SettingsData &expected, const SettingsData &actual) {
    assert(memcmp(&expected, &actual, sizeof(expected)) == 0);
}

/**
 * 验证项目切换、新引脚编号、NVS 长度、旧格式拒绝及当前格式配置保留
 * @param argc 参数个数
 * @param argv 可选 --json 导出真实项目 API 响应
 * @return 0 表示全部通过
 */
int main(int argc, char **argv) {
    settings_reset_to_defaults();
    if (argc == 2 && std::string(argv[1]) == "--json") {
        handle_projects_get();
        std::puts(response_body.c_str());
        return 0;
    }
    const auto *direct = project_preset_find("esp32c3-4switch");
    const auto *pcb = project_preset_find("esp32c3-tuanzi");
    assert(direct && pcb && !project_preset_find("unknown"));
    assert(AMS_COUNT == 1 && RS485_MODE == 1 && BUS_RX == 18 && BUS_TX == 19);
    assert(PCF_SDA == PIN_NONE && PCF_SCL == PIN_NONE);
    assert(IO_CONFIG.motor[3][0] == 7 && IO_CONFIG.motor[3][1] == 8 && IO_CONFIG.microswitch[3] == 5);
    assert(std::string(project_preset_match(settings_snapshot())) == direct->id);
    auto original = settings_snapshot();
    // NVS 只接受完整记录；短、长、不完整读取或缺失都返回全 0xFF。
    flash_record = original;
    for (int test = 0; test < 5; ++test) {
        stored_size = test == 0 ? 0 : test == 1 ? sizeof(original) - 4 :
                      test == 2 ? sizeof(original) + 4 : sizeof(original);
        short_read = test == 3;
        nvs_open = test != 4;
        SettingsData invalid{};
        hal_storage_read(HAL_REGION_SETTINGS, &invalid, sizeof(invalid));
        const auto *bytes = reinterpret_cast<const unsigned char *>(&invalid);
        for (size_t i = 0; i < sizeof(invalid); ++i) assert(bytes[i] == 0xFF);
    }
    stored_size = sizeof(original); short_read = false; nvs_open = true;
    SettingsData readback{};
    hal_storage_read(HAL_REGION_SETTINGS, &readback, sizeof(readback));
    same_settings(original, readback);
    assert(project_preset_validate(*direct) && project_preset_validate(*pcb));
    same_settings(original, settings_snapshot()); // 校验候选配置不能改变当前引脚

    // 现有三类校验仍拒绝系统保留脚、重复脚、总线冲突和缺失 I2C 的扩展脚。
    for (int test = 0; test < 8; ++test) {
        auto bad = *pcb;
        if (test == 0) bad.bus.tx = bad.bus.rx;
        if (test == 1) bad.pins[0][0] = 14;
        if (test == 2) bad.pins[1][0] = bad.pins[0][0];
        if (test == 3) bad.bus.sda = bad.bus.rx;
        if (test == 4) bad.bus.sda = PIN_NONE;
        if (test == 5) bad.bus.sda = bad.bus.scl = PIN_NONE;
        if (test == 6) bad.pins[0][0] = bad.bus.scl;
        if (test == 7) bad.target = "esp32s3";
        String error;
        assert(!project_preset_validate(bad, &error) && !error.empty());
        same_settings(original, settings_snapshot());
    }

    // 当前格式的有效记录即使为自定义，也应保留，不强制改为默认项目。
    IO_CONFIG.microswitch[0] = PIN_NONE;
    MANUAL_PULSE_MS = 432; AUTO_LOAD_MS = 765; MOTOR_LIMIT = 3;
    for (int ams = 0; ams < AMS_IDENTITY_COUNT; ++ams) {
        strcpy(CUSTOM_SERIAL[ams], "123456ABCDEF");
        CUSTOM_SERIAL[ams][CUSTOM_SERIAL_LENGTH - 1] = static_cast<char>('A' + ams);
        AMS_VERSION[ams][0] = 90 + ams;
    }
    strcpy(OTA_URL, "https://example.test/ota");
    CHANNEL_MODE[0] = CHANNEL_MODE_OFFLINE; PULLBACK_TIMEOUT[0] = 9;
    assert(settings_save_to_flash());
    original = settings_snapshot();
    settings_reset_to_defaults();
    settings_storage_init();
    same_settings(original, settings_snapshot());
    assert(!*project_preset_match(settings_snapshot()));

    // 未知 ID 和运行中请求不得写入或重启。
    int before = writes;
    server.params = {{"id", "unknown"}}; handle_project_save();
    assert(response_code == 400 && writes == before && restarts == 0);
    server.params = {{"id", pcb->id}}; busy = true; handle_project_save();
    assert(response_code == 409 && writes == before && restarts == 0);
    busy = false; fail_write = true; handle_project_save();
    assert(response_code == 500 && restarts == 0);
    same_settings(original, settings_snapshot());
    fail_write = false; corrupt_readback = true; handle_project_save();
    assert(response_code == 500 && restarts == 0);
    same_settings(original, settings_snapshot());
    corrupt_readback = false; before = writes; handle_project_save();
    assert(response_code == 200 && writes == before + 1 && restarts == 1);
    same_settings(original, settings_snapshot()); // 回调只保存，重启前仍用旧配置

    auto expected = original;
    project_preset_apply(*pcb, expected);
    expected.checksum = calculate_crc32(&expected);
    same_settings(expected, flash_record);
    settings_storage_init(); // 模拟重启读取整套新配置
    same_settings(expected, settings_snapshot());
    assert(AMS_COUNT == 2 && BUS_RX == 10 && BUS_TX == 7 && PCF_SDA == 4 && PCF_SCL == 5);
    const int pcb_pins[8][3] = {
        {55, 54, 50}, {56, 57, 52}, {60, 61, 67}, {63, 62, 65},
        {75, 74, 70}, {76, 77, 72}, {80, 81, 87}, {83, 82, 85}
    };
    const int switch_panel_pins[4] = {0, 2, 17, 15};
    for (int ch = 0; ch < 8; ++ch) {
        assert(IO_CONFIG.motor[ch][0] == pcb_pins[ch][0] && IO_CONFIG.motor[ch][1] == pcb_pins[ch][1]);
        assert(IO_CONFIG.microswitch[ch] == pcb_pins[ch][2]);
        assert(VIRTUAL_TO_PCF8575_PIN(IO_CONFIG.microswitch[ch]) == switch_panel_pins[ch % 4]);
    }
    for (int ch = 8; ch < IO_CONFIG_MAX_CHANNELS; ++ch) {
        assert(IO_CONFIG.motor[ch][0] == PIN_NONE && IO_CONFIG.motor[ch][1] == PIN_NONE);
        assert(IO_CONFIG.microswitch[ch] == PIN_NONE && !BMCU_channel_motor_ready(ch));
    }
    assert(!BMCU_channel_motor_ready(0));
    pcf_online[0] = true;
    assert(BMCU_channel_motor_ready(0));
    assert(!BMCU_channel_motor_ready(4));
    pcf_online[1] = true;
    assert(BMCU_channel_motor_ready(4));
    assert(std::string(project_preset_match(settings_snapshot())) == pcb->id);

    // 四片引脚可独立使用；同一片同一脚仍拒绝重复，关闭 I2C 必须清掉所有扩展接线。
    const auto pcb_settings = settings_snapshot();
    AMS_COUNT = 4;
    const int channel_offsets[4][3] = {{0, 1, 2}, {3, 4, 5}, {6, 7, 10}, {11, 12, 13}};
    for (int ch = 0; ch < 16; ++ch) {
        const int base = 50 + (ch / 4) * 20;
        IO_CONFIG.motor[ch][0] = base + channel_offsets[ch % 4][0];
        IO_CONFIG.motor[ch][1] = base + channel_offsets[ch % 4][1];
        IO_CONFIG.microswitch[ch] = base + channel_offsets[ch % 4][2];
    }
    IO_CONFIG.microswitch[15] = 127; // 最后一片 P17
    assert(io_config_validate(IO_CONFIG, active_channel_count()));
    assert(io_config_pin_label(50) == "50（io扩展1 P0）");
    assert(io_config_pin_label(60) == "60（io扩展1 P10）");
    assert(io_config_pin_label(70) == "70（io扩展2 P0）");
    assert(io_config_pin_label(127) == "127（io扩展4 P17）");
    String options;
    io_config_append_pin_options(options);
    int virtual_count = 0;
    for (int pin = 50; pin < 130; ++pin) {
        const bool valid = pin % 10 < 8;
        assert(io_config_pin_available(pin) == valid);
        assert((options.find("\"id\":" + std::to_string(pin) + ",") != std::string::npos) == valid);
        virtual_count += valid;
        if (valid) continue;
        for (int field = 0; field < 3; ++field) {
            auto gap = IO_CONFIG;
            if (field < 2) gap.motor[0][field] = pin;
            else gap.microswitch[0] = pin;
            assert(!io_config_validate(gap, active_channel_count()));
        }
    }
    assert(virtual_count == 64);
    for (int pin : {-1, 49, 130, 254, 255, 256}) assert(!io_config_pin_available(pin));
    assert(io_config_pin_available(PIN_NONE, true));
    auto invalid = IO_CONFIG;
    invalid.motor[4][0] = 50;
    assert(!io_config_validate(invalid, active_channel_count()));
    invalid = IO_CONFIG;
    invalid.microswitch[15] = 128;
    assert(!io_config_validate(invalid, active_channel_count()));
    for (int device = 0; device < 4; ++device) pcf_online[device] = device % 2 == 0;
    for (int ch = 0; ch < 16; ++ch)
        assert(BMCU_channel_motor_ready(ch) == (ch / 4 % 2 == 0));
    // 电机两脚跨片时必须两片都在线，真实 GPIO 不依赖其他 PCF 芯片。
    const auto complete_io = IO_CONFIG;
    IO_CONFIG.motor[0][1] = 70;
    assert(!BMCU_channel_motor_ready(0));
    pcf_online[1] = true;
    assert(BMCU_channel_motor_ready(0));
    pcf_online[0] = false;
    assert(!BMCU_channel_motor_ready(0));
    IO_CONFIG.motor[0][0] = 0;
    assert(BMCU_channel_motor_ready(0));
    IO_CONFIG.motor[0][1] = PIN_NONE;
    assert(!BMCU_channel_motor_ready(0));
    IO_CONFIG = complete_io;
    assert(settings_save_to_flash());
    const auto four_chips = settings_snapshot();
    settings_reset_to_defaults();
    settings_storage_init();
    same_settings(four_chips, settings_snapshot());
    invalid = IO_CONFIG;
    assert(io_config_clear_invalid_pins(invalid, false, PIN_NONE, PIN_NONE) == 48);
    for (int ch = 0; ch < 16; ++ch)
        assert(invalid.motor[ch][0] == PIN_NONE && invalid.motor[ch][1] == PIN_NONE &&
               invalid.microswitch[ch] == PIN_NONE);
    PCF_SDA = PCF_SCL = PIN_NONE;
    for (int pin = 50; pin < 130; ++pin) assert(!io_config_pin_available(pin));
    settings_restore_snapshot(pcb_settings);

    // 反向切换时一次清掉扩展接线，已有非接线参数保留，并发上限收敛。
    MOTOR_LIMIT = 7;
    before = writes;
    server.params = {{"id", direct->id}}; handle_project_save();
    assert(response_code == 200 && writes == before + 1 && restarts == 2);
    settings_storage_init();
    assert(MOTOR_LIMIT == 4 && AMS_COUNT == 1 && !i2c_bus_configured());
    assert(MANUAL_PULSE_MS == 432 && AUTO_LOAD_MS == 765 && PULLBACK_TIMEOUT[0] == 9);
    for (int ams = 0; ams < AMS_IDENTITY_COUNT; ++ams) {
        assert(memcmp(CUSTOM_SERIAL[ams], original.custom_serial[ams], CUSTOM_SERIAL_LENGTH) == 0);
        assert(memcmp(AMS_VERSION[ams], original.ams_version[ams], sizeof(AMS_VERSION[ams])) == 0);
    }
    assert(CHANNEL_MODE[0] == CHANNEL_MODE_OFFLINE);
    assert(std::string(project_preset_match(settings_snapshot())) == direct->id);
    for (int ch = 4; ch < IO_CONFIG_MAX_CHANNELS; ++ch)
        assert(IO_CONFIG.motor[ch][0] == PIN_NONE && IO_CONFIG.microswitch[ch] == PIN_NONE);
    // 旧编号或单份身份布局即使引脚值仍合法、CRC 正确，也必须拒绝旧格式。
    for (uint32_t old_magic : {0x53415429u, 0x5341542Au, 0x5341542Bu}) {
        flash_record = pcb_settings;
        flash_record.magic = old_magic;
        for (int ch = 0; ch < 4; ++ch) flash_record.io_config.microswitch[ch] = 60 + ch;
        flash_record.checksum = calculate_crc32(&flash_record);
        assert(!settings_is_valid());
        before = writes;
        const int reads_before = reads;
        settings_storage_init();
        assert(writes == before + 1 && reads == reads_before + 2);
        assert(flash_record.magic == SETTINGS_MAGIC_NUMBER && settings_is_valid());
        same_settings(settings_snapshot(), flash_record);
        assert(std::string(project_preset_match(settings_snapshot())) == direct->id);
        assert(MANUAL_PULSE_MS == DEFAULT_MANUAL_PULSE_MS);
        for (int ams = 0; ams < AMS_IDENTITY_COUNT; ++ams) assert(!CUSTOM_SERIAL[ams][0]);
        settings_storage_init();
        assert(writes == before + 1);
    }
    // 坏 CRC 同样恢复 C3 默认项目。
    flash_record.checksum ^= 1;
    settings_storage_init();
    assert(std::string(project_preset_match(settings_snapshot())) == direct->id);
    test_ams_identity_http();
    std::puts("PASS: project defaults, sparse pin numbering/gaps, NVS exact length, old format reset, current settings, two-way switch, empty channels, HTTP busy/failure, CRC readback, per-AMS identity HTTP");
}
