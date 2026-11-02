/**
 * 构造当前格式的自定义记录
 * @return 带当前魔数、四份不同 AMS 身份、接线和通道参数的设置
 * 覆盖全部设置字段，验证重启后原样加载且不会自动改写版本或自定义后缀
 */
static SettingsData custom_record() {
    settings_reset_to_defaults();
    AMS_COUNT = 4; MOTOR_LIMIT = 3;
    MANUAL_PULSE_MS = 432; AUTO_LOAD_MS = 765;
    RS485_MODE = RS485_MODE_DE; BUS_LOG_ENABLE = 0; OTA_NOTIFY = 0;
    BUS_RX = 20; BUS_TX = 21; BUS_DE = 10;
    PCF_SDA = 4; PCF_SCL = 5;
    const char *suffixes[AMS_IDENTITY_COUNT] = {
        "123456ABCDEF", "7BF9E5A60A48", "ABCDEF012345", "00000000000F"
    };
    const int versions[AMS_IDENTITY_COUNT][4] = {
        {90, 0, 0, 0}, {1, 2, 3, 4}, {10, 50, 0, 0}, {255, 254, 253, 252}
    };
    for (int ams = 0; ams < AMS_IDENTITY_COUNT; ++ams)
        memcpy(CUSTOM_SERIAL[ams], suffixes[ams], CUSTOM_SERIAL_LENGTH + 1);
    memcpy(AMS_VERSION, versions, sizeof(versions));
    strcpy(OTA_URL, "https://example.test/custom-ota");
    for (int i = 0; i < 16; ++i) {
        PULLBACK_TIMEOUT[i] = 3 + i;
        CHANNEL_MODE[i] = i % 3;
    }
    IO_CONFIG.motor[0][0] = 6; IO_CONFIG.motor[0][1] = 7;
    IO_CONFIG.microswitch[0] = 3;
    return settings_snapshot();
}

/**
 * 验证四份 AMS 身份的生成、字段校验和启用数量变化
 * @param original 四份不同自定义身份的有效记录
 * @param defaults 平台默认记录
 * 自动值、自定义值和未启用条目均通过真实设置函数检查；结束后恢复原记录
 */
static void test_ams_identity_values(const SettingsData &original, const SettingsData &defaults) {
    assert(settings_custom_serial_valid("") && settings_custom_serial_valid("7BF9E5A60A48"));
    assert(!settings_custom_serial_valid(nullptr) && !settings_custom_serial_valid("7BF9E5A60A4"));
    assert(!settings_custom_serial_valid("7BF9E5A60A488") && !settings_custom_serial_valid("7bf9e5a60a48"));
    assert(!settings_custom_serial_valid("7BF9E5A60A4G"));
    int parsed[4] = {7, 8, 9, 10};
    assert(settings_parse_ams_version("255.0.1.255", parsed));
    const int parsed_valid[4] = {255, 0, 1, 255};
    assert(memcmp(parsed, parsed_valid, sizeof(parsed)) == 0);
    for (const char *invalid : {"256.0.0.0", "1.2.3", "1.2.3.4.5", "-1.2.3.4", "1..3.4"}) {
        assert(!settings_parse_ams_version(invalid, parsed));
        assert(memcmp(parsed, parsed_valid, sizeof(parsed)) == 0);
    }

    settings_restore_snapshot(defaults);
    const char *automatic[AMS_IDENTITY_COUNT] = {
        "0EA07BF9E5A60A40", "0EA17BF9E5A60A41", "0EA27BF9E5A60A42", "0EA37BF9E5A60A43"
    };
    const auto reads_before = read_count, writes_before = write_count;
    char serial[AMS_SERIAL_LENGTH + 1];
    for (uint8_t ams = 0; ams < AMS_IDENTITY_COUNT; ++ams) {
        assert(settings_get_ams_serial(ams, serial) && std::string(serial) == automatic[ams]);
        assert(strlen(serial) == AMS_SERIAL_LENGTH);
        assert(AMS_VERSION[ams][0] == 1 && AMS_VERSION[ams][1] == 0 &&
               AMS_VERSION[ams][2] == 6 && AMS_VERSION[ams][3] == 87);
    }
    assert(!settings_get_ams_serial(AMS_IDENTITY_COUNT, serial) && serial[0] == 0);
    strcpy(serial, "must be cleared");
    assert(!settings_get_ams_serial(255, serial) && serial[0] == 0);
    assert(!settings_get_ams_serial(0, nullptr));
    auto actual = settings_snapshot();
    assert(memcmp(&actual, &defaults, sizeof(actual)) == 0);
    assert(read_count == reads_before && write_count == writes_before);

    settings_restore_snapshot(original);
    const char *custom[AMS_IDENTITY_COUNT] = {
        "0EA0123456ABCDEF", "0EA17BF9E5A60A48", "0EA2ABCDEF012345", "0EA300000000000F"
    };
    for (uint8_t ams = 0; ams < AMS_IDENTITY_COUNT; ++ams)
        assert(settings_get_ams_serial(ams, serial) && std::string(serial) == custom[ams]);

    AMS_COUNT = 1;
    assert(settings_save_to_flash());
    settings_reset_to_defaults();
    settings_storage_init();
    actual = settings_snapshot();
    assert(AMS_COUNT == 1 && memcmp(actual.custom_serial, original.custom_serial, sizeof(actual.custom_serial)) == 0);
    assert(memcmp(actual.ams_version, original.ams_version, sizeof(actual.ams_version)) == 0);
    for (uint8_t ams = 0; ams < AMS_IDENTITY_COUNT; ++ams)
        assert(settings_get_ams_serial(ams, serial) && std::string(serial) == custom[ams]);
    AMS_COUNT = 4;
    assert(settings_save_to_flash());
    settings_reset_to_defaults();
    settings_storage_init();
    actual = settings_snapshot();
    assert(memcmp(&actual, &original, sizeof(actual)) == 0);

    memset(CUSTOM_SERIAL[2], 0, sizeof(CUSTOM_SERIAL[2]));
    assert(settings_save_to_flash());
    settings_reset_to_defaults();
    settings_storage_init();
    for (uint8_t ams = 0; ams < AMS_IDENTITY_COUNT; ++ams)
        assert(settings_get_ams_serial(ams, serial) && std::string(serial) == (ams == 2 ? automatic[2] : custom[ams]));

    SettingsData invalid = original;
    invalid.custom_serial[1][0] = 'G';
    invalid.ams_version[2][1] = 256;
    invalid.ams_version[3][3] = -1;
    invalid.checksum = calculate_crc32(&invalid);
    flash_record = invalid;
    settings_storage_init();
    actual = settings_snapshot();
    assert(actual.custom_serial[1][0] == 0 && settings_get_ams_serial(1, serial) && std::string(serial) == automatic[1]);
    assert(memcmp(actual.custom_serial[0], original.custom_serial[0], CUSTOM_SERIAL_LENGTH) == 0);
    assert(memcmp(actual.custom_serial[2], original.custom_serial[2], CUSTOM_SERIAL_LENGTH) == 0);
    assert(memcmp(actual.custom_serial[3], original.custom_serial[3], CUSTOM_SERIAL_LENGTH) == 0);
    assert(memcmp(actual.ams_version[0], original.ams_version[0], sizeof(actual.ams_version[0])) == 0);
    assert(memcmp(actual.ams_version[1], original.ams_version[1], sizeof(actual.ams_version[1])) == 0);
    assert(memcmp(actual.ams_version[2], defaults.ams_version[2], sizeof(actual.ams_version[2])) == 0);
    assert(memcmp(actual.ams_version[3], defaults.ams_version[3], sizeof(actual.ams_version[3])) == 0);
    settings_restore_snapshot(original);
    assert(settings_save_to_flash());
}

/**
 * 运行设置存储场景
 * @param argc 必须包含场景名
 * @param argv 测试程序及场景名
 * @return 0 表示通过
 * 对完整快照逐字节比较，覆盖四份独立身份、当前格式、旧格式拒绝、坏 CRC 和保存失败回滚
 */
int main(int argc, char **argv) {
    assert(argc == 2);
    const std::string scenario = argv[1];
    const auto original = custom_record();
    settings_reset_to_defaults();
    const auto defaults = settings_snapshot();
    flash_record = original;
    const bool old_pcf = scenario == "old_pcf_numbering" || scenario == "old_pcf_numbering_2a";
    const bool invalid = scenario == "bad_magic" || old_pcf || scenario == "bad_crc" || scenario == "empty" ||
                         scenario == "default_write_failure";
    if (scenario == "bad_magic") {
        flash_record.magic = 0x5341542Bu;
        flash_record.checksum = calculate_crc32(&flash_record); // CRC 正确也不能加载不同格式
    }
    if (old_pcf) {
        flash_record.magic = scenario == "old_pcf_numbering" ? 0x53415429u : 0x5341542Au;
        flash_record.io_config.motor[0][0] = 50;
        flash_record.io_config.motor[0][1] = 54;
        flash_record.io_config.microswitch[0] = 60;
        flash_record.checksum = calculate_crc32(&flash_record);
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
    if (scenario == "current_custom") test_ams_identity_values(original, defaults);
    if (scenario == "write_failure" || scenario == "readback_failure") {
        fail_write = scenario == "write_failure";
        corrupt_readback = scenario == "readback_failure";
        MANUAL_PULSE_MS = 987;
        strcpy(CUSTOM_SERIAL[2], "F0F0F0F0F0F0");
        AMS_VERSION[3][0] = 123;
        auto expected = settings_snapshot();
        assert(!settings_save_to_flash());
        assert(write_count == 1 && read_count == (fail_write ? 1u : 2u));
        settings_restore_snapshot(original);
        actual = settings_snapshot();
        assert(memcmp(&actual, &original, sizeof(actual)) == 0);
        if (fail_write) assert(memcmp(&flash_record, &original, sizeof(original)) == 0);
        fail_write = corrupt_readback = false;
        MANUAL_PULSE_MS = 987;
        strcpy(CUSTOM_SERIAL[2], "F0F0F0F0F0F0");
        AMS_VERSION[3][0] = 123;
        assert(settings_save_to_flash());
        settings_reset_to_defaults();
        settings_storage_init();
        actual = settings_snapshot();
        assert(memcmp(&actual, &expected, sizeof(actual)) == 0);
    }
    std::printf("PASS %s\n", argv[1]);
}
