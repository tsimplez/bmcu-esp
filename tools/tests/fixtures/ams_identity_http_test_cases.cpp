/**
 * 验证真实设置接口的逐台身份、错误请求和失败回滚
 * 使用生产 GET/POST、字段校验、设置快照及存储回读，电机和 HTTP 由硬件桩替代
 */
static void test_ams_identity_http() {
    settings_reset_to_defaults();
    AMS_COUNT = 4;
    assert(settings_save_to_flash());
    const auto defaults = settings_snapshot();
    const std::vector<std::map<String, String>> invalid_requests = {
        {{"serial", "7BF9E5A60A48"}},
        {{"ams_version", "1.2.3.4"}},
        {{"ams_index", "1"}},
        {{"ams_index", "-1"}, {"serial", ""}},
        {{"ams_index", "4"}, {"serial", ""}},
        {{"ams_index", ""}, {"serial", ""}},
        {{"ams_index", "1x"}, {"serial", ""}},
        {{"ams_index", "1.5"}, {"serial", ""}},
        {{"ams_index", "1"}, {"serial", "7BF9E5A60A4"}, {"ams_version", "9.8.7.6"}},
        {{"ams_index", "1"}, {"serial", "7BF9E5A60A4G"}},
        {{"ams_index", "1"}, {"serial", "7BF9E5A60A48"}, {"ams_version", "1.2.256.4"}},
    };
    for (const auto &request : invalid_requests) {
        const int writes_before = writes;
        server.params = request;
        handle_settings_save();
        assert(response_code == 400 && writes == writes_before);
        const auto current = settings_snapshot();
        assert(memcmp(&defaults, &current, sizeof(defaults)) == 0);
    }

    server.params = {{"ams_index", "1"}, {"serial", "7BF9E5A60A48"}, {"ams_version", "9.8.7.6"}};
    handle_settings_save();
    assert(response_code == 200 && std::string(CUSTOM_SERIAL[1]) == "7BF9E5A60A48");
    const int version[4] = {9, 8, 7, 6};
    assert(memcmp(AMS_VERSION[1], version, sizeof(version)) == 0);
    for (int ams : {0, 2, 3}) {
        assert(!CUSTOM_SERIAL[ams][0]);
        assert(memcmp(AMS_VERSION[ams], defaults.ams_version[ams], sizeof(version)) == 0);
    }
    const auto saved = settings_snapshot();
    settings_reset_to_defaults();
    settings_storage_init();
    auto current = settings_snapshot();
    assert(memcmp(&saved, &current, sizeof(saved)) == 0);

    server.params.clear();
    const int writes_before_get = writes;
    handle_settings_get();
    assert(response_code == 200 && writes == writes_before_get);
    assert(response_body.find("\"ams_identity\":[") != std::string::npos);
    assert(response_body.find("\"index\":1,\"serial\":\"7BF9E5A60A48\",\"effective_serial\":\"0EA17BF9E5A60A48\",\"ams_version\":\"9.8.7.6\",\"enabled\":true") != std::string::npos);
    for (int ams : {0, 2, 3}) {
        const std::string expected = "0EA" + std::to_string(ams) + "7BF9E5A60A4" + std::to_string(ams);
        assert(response_body.find(expected) != std::string::npos);
    }

    // 写入失败和回读失败都必须恢复全部内存身份；写入失败还保留原 Flash。
    for (int failure = 0; failure < 2; ++failure) {
        fail_write = failure == 0;
        corrupt_readback = failure == 1;
        server.params = {{"ams_index", "2"}, {"serial", "ABCDEF012345"}, {"ams_version", "90.1.2.3"}};
        handle_settings_save();
        assert(response_code == 500);
        current = settings_snapshot();
        assert(memcmp(&saved, &current, sizeof(saved)) == 0);
        if (fail_write) assert(memcmp(&saved, &flash_record, sizeof(saved)) == 0);
        fail_write = corrupt_readback = false;
        assert(settings_save_to_flash());
    }

    // 未启用的身份也可提前配置；数量变化不能清除它或改变其他台。
    server.params = {{"ams", "1"}}; handle_settings_save();
    assert(response_code == 200 && AMS_COUNT == 1);
    server.params = {{"ams_index", "3"}, {"serial", "123456789ABC"}, {"ams_version", "10.20.30.40"}};
    handle_settings_save();
    assert(response_code == 200 && AMS_COUNT == 1);
    server.params.clear(); handle_settings_get();
    assert(response_body.find("\"index\":3,\"serial\":\"123456789ABC\",\"effective_serial\":\"0EA3123456789ABC\",\"ams_version\":\"10.20.30.40\",\"enabled\":false") != std::string::npos);
    server.params = {{"ams", "4"}}; handle_settings_save();
    assert(response_code == 200 && AMS_COUNT == 4);
    assert(std::string(CUSTOM_SERIAL[3]) == "123456789ABC" && AMS_VERSION[3][0] == 10);
    assert(std::string(CUSTOM_SERIAL[1]) == "7BF9E5A60A48");

    server.params = {{"ams_index", "1"}, {"serial", ""}};
    handle_settings_save();
    char serial[AMS_SERIAL_LENGTH + 1] = {};
    settings_get_ams_serial(1, serial);
    assert(response_code == 200 && std::string(serial) == "0EA17BF9E5A60A41");
    assert(memcmp(AMS_VERSION[1], version, sizeof(version)) == 0);
    assert(std::string(CUSTOM_SERIAL[3]) == "123456789ABC");
}
