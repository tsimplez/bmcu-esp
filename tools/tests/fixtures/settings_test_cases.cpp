/**
 * 构造当前格式的自定义记录
 * @return 带当前魔数、自定义 AMS 版本、接线和通道参数的设置
 * 覆盖全部设置字段，验证重启后原样加载且不会自动改写版本
 */
static SettingsData custom_record() {
    settings_reset_to_defaults();
    AMS_COUNT = 4; MOTOR_LIMIT = 3;
    MANUAL_PULSE_MS = 432; AUTO_LOAD_MS = 765;
    RS485_MODE = RS485_MODE_DE; BUS_LOG_ENABLE = 0; OTA_NOTIFY = 0;
    BUS_RX = 20; BUS_TX = 21; BUS_DE = 10;
    PCF_SDA = 4; PCF_SCL = 5;
    memcpy(CUSTOM_SERIAL, "123456ABCDEF", 13);
    strcpy(OTA_URL, "https://example.test/custom-ota");
    for (int i = 0; i < 16; ++i) {
        PULLBACK_TIMEOUT[i] = 3 + i;
        CHANNEL_MODE[i] = i % 3;
    }
    IO_CONFIG.motor[0][0] = 6; IO_CONFIG.motor[0][1] = 7;
    IO_CONFIG.microswitch[0] = 3;
    SettingsData record = settings_snapshot();
    record.ams_version[0] = 90; record.ams_version[1] = 0;
    record.ams_version[2] = 0; record.ams_version[3] = 0;
    record.checksum = calculate_crc32(&record);
    return record;
}

/**
 * 运行设置存储场景
 * @param argc 必须包含场景名
 * @param argv 测试程序及场景名
 * @return 0 表示通过
 * 对完整快照逐字节比较，覆盖当前格式、不匹配魔数、空片、坏 CRC 和保存失败回滚
 */
int main(int argc, char **argv) {
    assert(argc == 2);
    const std::string scenario = argv[1];
    const auto original = custom_record();
    settings_reset_to_defaults();
    const auto defaults = settings_snapshot();
    flash_record = original;
    const bool invalid = scenario == "bad_magic" || scenario == "bad_crc" || scenario == "empty" ||
                         scenario == "default_write_failure";
    if (scenario == "bad_magic") {
        flash_record.magic = SETTINGS_MAGIC_NUMBER ^ 1u;
        flash_record.checksum = calculate_crc32(&flash_record); // CRC 正确也不能加载不同格式
    }
    if (scenario == "bad_crc") flash_record.checksum ^= 1;
    if (scenario == "empty" || scenario == "default_write_failure")
        memset(&flash_record, 0xFF, sizeof(flash_record));
    assert(settings_is_valid() == !invalid);
    read_count = write_count = validation_count = 0;
    fail_write = scenario == "default_write_failure";
    settings_storage_init();
    auto actual = settings_snapshot();
    if (invalid) {
        assert(memcmp(&actual, &defaults, sizeof(actual)) == 0);
        assert(write_count == 1 && read_count == (fail_write ? 1u : 2u));
        if (fail_write) {
            assert(!settings_is_valid());
            fail_write = false;
            settings_storage_init(); // 下一次启动重新尝试写入默认值
            assert(write_count == 2);
        }
        assert(memcmp(&defaults, &flash_record, sizeof(defaults)) == 0);
        const auto written = write_count;
        settings_storage_init();
        assert(write_count == written); // 有效记录不再重复写入
        std::printf("PASS %s\n", argv[1]);
        return 0;
    }
    assert(memcmp(&actual, &original, sizeof(actual)) == 0);
    assert(write_count == 0 && read_count == 1 && validation_count == 1);
    if (scenario == "write_failure" || scenario == "readback_failure") {
        fail_write = scenario == "write_failure";
        corrupt_readback = scenario == "readback_failure";
        MANUAL_PULSE_MS = 987;
        auto expected = settings_snapshot();
        assert(!settings_save_to_flash());
        assert(write_count == 1 && read_count == (fail_write ? 1u : 2u));
        settings_restore_snapshot(original);
        actual = settings_snapshot();
        assert(memcmp(&actual, &original, sizeof(actual)) == 0);
        if (fail_write) assert(memcmp(&flash_record, &original, sizeof(original)) == 0);
        fail_write = corrupt_readback = false;
        MANUAL_PULSE_MS = 987;
        assert(settings_save_to_flash());
        settings_reset_to_defaults();
        settings_storage_init();
        actual = settings_snapshot();
        assert(memcmp(&actual, &expected, sizeof(actual)) == 0);
    }
    std::printf("PASS %s\n", argv[1]);
}
