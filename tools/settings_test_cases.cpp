/**
 * 构造上一版有效记录
 * @return 带旧魔数、旧默认版本和自定义接线/通道参数的设置
 * 覆盖每个保留字段，避免只验证版本变化而忽略其他配置丢失
 */
static SettingsData legacy_record() {
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
    record.magic = SETTINGS_MAGIC_PREVIOUS;
    record.ams_version[0] = 90; record.ams_version[1] = 0;
    record.ams_version[2] = 0; record.ams_version[3] = 0;
    record.checksum = calculate_crc32(&record);
    return record;
}

/**
 * 运行设置迁移场景
 * @param argc 必须包含场景名
 * @param argv 测试程序及场景名
 * @return 0 表示通过
 * 对整个快照逐字节比较，验证旧默认/自定义版本、坏 CRC、写入失败和回读损坏
 */
int main(int argc, char **argv) {
    assert(argc == 2);
    const std::string scenario = argv[1];
    auto original = legacy_record();
    if (scenario == "legacy_custom") {
        original.ams_version[0] = 11; original.ams_version[1] = 22;
        original.ams_version[2] = 33; original.ams_version[3] = 44;
    } else if (scenario == "current_custom") {
        original.magic = SETTINGS_MAGIC_NUMBER; // 新记录主动设为 90.0.0.0 时不得再次迁移
    }
    original.checksum = calculate_crc32(&original);
    flash_record = original;
    if (scenario == "bad_crc") flash_record.checksum ^= 1;
    assert(settings_is_valid() == (scenario != "bad_crc"));
    settings_reset_to_defaults();
    read_count = write_count = validation_count = 0;
    fail_write = scenario == "write_failure";
    corrupt_readback = scenario == "readback_failure";
    settings_storage_init();
    const auto actual = settings_snapshot();
    auto expected = original;
    expected.magic = SETTINGS_MAGIC_NUMBER;
    if (scenario == "legacy_default") {
        expected.ams_version[0] = 1; expected.ams_version[1] = 0;
        expected.ams_version[2] = 6; expected.ams_version[3] = 87;
    } else if (scenario == "bad_crc") {
        assert(AMS_COUNT == DEFAULT_AMS_COUNT && CUSTOM_SERIAL[0] == 0);
        assert(AMS_VERSION[0] == 1 && AMS_VERSION[1] == 0 && AMS_VERSION[2] == 6 && AMS_VERSION[3] == 87);
        assert(write_count == 1 && read_count == 2);
        assert(memcmp(&actual, &flash_record, sizeof(actual)) == 0);
        std::printf("PASS %s\n", argv[1]);
        return 0;
    }
    expected.checksum = calculate_crc32(&expected);
    assert(memcmp(&actual, &expected, sizeof(actual)) == 0);
    assert(validation_count >= 1);
    if (scenario == "current_custom") {
        assert(write_count == 0 && read_count == 1);
    } else if (fail_write) {
        assert(write_count == 1 && read_count == 1);
        assert(memcmp(&original, &flash_record, sizeof(original)) == 0);
        fail_write = false;
        settings_storage_init(); // 下一次加载重试成功，仍保留接线
        assert(AMS_VERSION[0] == 1 && AMS_VERSION[3] == 87 && AMS_COUNT == 4);
    } else if (corrupt_readback) {
        assert(write_count == 1 && read_count == 2 && validation_count == 2);
    } else {
        assert(write_count == 1 && read_count == 2);
        assert(memcmp(&expected, &flash_record, sizeof(expected)) == 0);
        settings_storage_init();
        assert(write_count == 1); // 已迁移记录不重复写入
    }
    std::printf("PASS %s\n", argv[1]);
}
