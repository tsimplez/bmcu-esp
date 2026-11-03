/**
 * 为总微动保存生成完整通道接线请求
 * @param pin 总微动引脚的请求文本
 * @return 与当前通道配置一致的表单参数
 */
static std::map<String, String> switch_request(const char *pin) {
    std::map<String, String> request = {{"io_switch", pin}};
    for (int ch = 0; ch < active_channel_count(); ++ch) {
        request["io_mf" + String(ch)] = String(IO_CONFIG.motor[ch][0]);
        request["io_mb" + String(ch)] = String(IO_CONFIG.motor[ch][1]);
        request["io_sw" + String(ch)] = String(IO_CONFIG.microswitch[ch]);
    }
    return request;
}

/** 验证进料延迟和总微动的真实 GET/POST、冲突拒绝、忙碌保护、存储及失败回滚 */
static void test_feed_switch_http() {
    settings_reset_to_defaults();
    assert(FEED_STOP_DELAY_MS == 1000 && IO_CONFIG.switch_pin == PIN_NONE);
    assert(settings_save_to_flash());
    const auto defaults = settings_snapshot();
    for (const char *bad : {"", "-1", "30000", "1.5", "abc", "1000ms", "999999999999999999999"}) {
        server.params = {{"feed_stop_delay_ms", bad}};
        const int before = writes;
        handle_settings_save();
        const auto actual = settings_snapshot();
        assert(response_code == 400 && writes == before);
        assert(memcmp(&defaults, &actual, sizeof(actual)) == 0);
    }
    for (const char *good : {"0", "1", "29999", "1678"}) {
        server.params = {{"feed_stop_delay_ms", good}};
        handle_settings_save();
        assert(response_code == 200 && FEED_STOP_DELAY_MS == std::atoi(good));
    }
    server.params = switch_request("9");
    handle_settings_save();
    assert(response_code == 200 && IO_CONFIG.switch_pin == 9);
    assert(!*project_preset_match(settings_snapshot()));
    const auto saved = settings_snapshot();
    settings_reset_to_defaults();
    settings_storage_init();
    auto actual = settings_snapshot();
    assert(memcmp(&saved, &actual, sizeof(saved)) == 0);
    handle_settings_get();
    assert(response_body.find("\"feed_stop_delay_ms\":1678") != String::npos);
    assert(response_body.find("\"switch_pin\":9") != String::npos);

    for (const char *pin : {"0", "2", "18", "19", "14", "20", "21", "58", "66", "256", "-1", "9x"}) {
        server.params = switch_request(pin);
        server.params["feed_stop_delay_ms"] = "0";
        const int before = writes;
        handle_settings_save();
        actual = settings_snapshot();
        assert(response_code == 400 && writes == before);
        assert(memcmp(&saved, &actual, sizeof(saved)) == 0);
    }
    server.params = switch_request("255");
    busy = true;
    handle_settings_save();
    busy = false;
    assert(response_code == 409 && IO_CONFIG.switch_pin == 9);
    for (int failure : {0, 1}) {
        server.params = switch_request("255");
        server.params["feed_stop_delay_ms"] = "0";
        fail_write = failure == 0;
        corrupt_readback = failure == 1;
        handle_settings_save();
        actual = settings_snapshot();
        assert(response_code == 500 && memcmp(&saved, &actual, sizeof(saved)) == 0);
        fail_write = corrupt_readback = false;
        assert(settings_save_to_flash());
    }
    String error;
    assert(!io_config_i2c_validate(9, 8, IO_CONFIG, active_channel_count(), &error));
    assert(!io_config_bus_validate(9, BUS_TX, BUS_DE, RS485_MODE, PCF_SDA, PCF_SCL,
                                   IO_CONFIG, active_channel_count(), &error));

    auto candidate = saved;
    project_preset_apply(*project_preset_find("esp32c3-tuanzi"), candidate);
    assert(candidate.io_config.switch_pin == PIN_NONE && candidate.feed_stop_delay_ms == 1678);
    settings_restore_snapshot(candidate);
    server.params = switch_request("66");
    handle_settings_save();
    assert(response_code == 200 && IO_CONFIG.switch_pin == 66);
    assert(io_config_validate(IO_CONFIG, active_channel_count()));
    auto cleared = IO_CONFIG;
    assert(io_config_clear_invalid_pins(cleared, false, PIN_NONE, PIN_NONE) > 0);
    assert(cleared.switch_pin == PIN_NONE);
    cleared.switch_pin = 4;
    assert(io_config_clear_invalid_pins(cleared, true, 4, 5) == 1);
    assert(cleared.switch_pin == PIN_NONE);
    server.params = switch_request("255");
    handle_settings_save();
    assert(response_code == 200 && IO_CONFIG.switch_pin == PIN_NONE);
    settings_reset_to_defaults();
    assert(settings_save_to_flash());
}
