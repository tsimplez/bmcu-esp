static void same_settings(const SettingsData &expected, const SettingsData &actual);

/**
 * 为霍尔接线测试生成完整通道表单
 * @return 当前接线的全部启用通道字段，包含霍尔与五通总微动
 */
static std::map<String, String> hall_settings_request() {
    auto request = switch_request(String(IO_CONFIG.switch_pin).c_str());
    for (int ch = 0; ch < active_channel_count(); ++ch)
        request["io_h" + String(ch)] = String(IO_CONFIG.hall[ch]);
    return request;
}

/**
 * 验证真实霍尔引脚校验、设置 API、存储回滚与项目切换
 * 在团子预设上分配空闲 ADC 引脚，再验证 PCF、ADC2、重复输入和总线改线被拒绝
 */
static void test_hall_settings_http() {
    settings_reset_to_defaults();
    for (int pin : IO_CONFIG.hall) assert(pin == PIN_NONE);
    for (int pin = 0; pin <= 4; ++pin) assert(hal_adc_pin_available(pin));
    for (int pin : {-1, 5, 6, 48, 50, PIN_NONE}) assert(!hal_adc_pin_available(pin));
    auto candidate = settings_snapshot();
    project_preset_apply(*project_preset_find("esp32c3-tuanzi"), candidate);
    settings_restore_snapshot(candidate);
    assert(settings_save_to_flash());
    const auto defaults = settings_snapshot();

    handle_settings_get();
    assert(response_code == 200);
    assert(response_body.find("\"hall\":[255,255,255,255") != String::npos);
    const auto start = response_body.find("\"hall_pins\":[");
    assert(start != String::npos);
    const String choices = response_body.substr(start, response_body.find(']', start) - start + 1);
    for (int pin = 0; pin < 4; ++pin)
        assert(choices.find("\"id\":" + String(pin) + ",") != String::npos);
    for (int pin : {4, 5, 6, 18, 50, 70, 90, 110})
        assert(choices.find("\"id\":" + String(pin) + ",") == String::npos);

    server.params = hall_settings_request();
    for (int ch = 0; ch < 4; ++ch) server.params["io_h" + String(ch)] = String(ch);
    handle_settings_save();
    assert(response_code == 200);
    for (int ch = 0; ch < 4; ++ch) assert(IO_CONFIG.hall[ch] == ch);
    assert(!*project_preset_match(settings_snapshot()));
    const auto saved = settings_snapshot();
    settings_reset_to_defaults();
    settings_storage_init();
    same_settings(saved, settings_snapshot());

    // 旧客户端没有霍尔字段时保留已经保存的输入，避免普通接线表单清空传感器。
    server.params = switch_request(String(IO_CONFIG.switch_pin).c_str());
    handle_settings_save();
    assert(response_code == 200);
    same_settings(saved, settings_snapshot());
    for (const char *missing : {"io_h0", "io_h7", "io_mf0", "io_mb0", "io_sw0"}) {
        server.params = hall_settings_request();
        server.params.erase(missing);
        const int before = writes;
        handle_settings_save();
        assert(response_code == 400 && writes == before);
        same_settings(saved, settings_snapshot());
    }
    for (const char *unknown : {"io_h8", "io_h16", "io_h-1", "io-h0"}) {
        server.params = hall_settings_request();
        server.params[unknown] = "0";
        const int before = writes;
        handle_settings_save();
        assert(response_code == 400 && writes == before);
        same_settings(saved, settings_snapshot());
    }

    for (const char *pin : {"4", "5", "6", "18", "50", "70", "90", "110", "254", "256", "-1", "", "0x1", "1.5", "2x"}) {
        server.params = hall_settings_request();
        server.params["io_h0"] = pin;
        const int before = writes;
        handle_settings_save();
        assert(response_code == 400 && writes == before);
        same_settings(saved, settings_snapshot());
    }
    server.params = hall_settings_request();
    server.params["io_h4"] = "0";
    handle_settings_save();
    assert(response_code == 400);
    same_settings(saved, settings_snapshot());

    String error;
    assert(!io_config_i2c_validate(0, 8, IO_CONFIG, active_channel_count(), &error));
    assert(!error.empty());
    assert(!io_config_bus_validate(0, BUS_TX, BUS_DE, RS485_MODE, PCF_SDA, PCF_SCL,
                                  IO_CONFIG, active_channel_count(), &error));
    const IoBusConfig moved_i2c = {BUS_RX, BUS_TX, BUS_DE, RS485_MODE, 8, 9};
    assert(io_config_hall_pin_available(4, false, &moved_i2c));
    auto five_inputs = IO_CONFIG;
    five_inputs.hall[4] = 4;
    assert(io_config_validate(five_inputs, active_channel_count(), &error, &moved_i2c));
    auto conflicting = IO_CONFIG;
    conflicting.motor[0][0] = 0;
    assert(!io_config_validate(conflicting, active_channel_count(), &error));
    conflicting = IO_CONFIG;
    conflicting.microswitch[0] = 0;
    assert(!io_config_validate(conflicting, active_channel_count(), &error));
    conflicting = IO_CONFIG;
    conflicting.switch_pin = 0;
    assert(!io_config_validate(conflicting, active_channel_count(), &error));

    for (int failure = 0; failure < 3; ++failure) {
        server.params = hall_settings_request();
        server.params["io_h0"] = "255";
        busy = failure == 0;
        fail_write = failure == 1;
        corrupt_readback = failure == 2;
        handle_settings_save();
        assert(response_code == (busy ? 409 : 500));
        same_settings(saved, settings_snapshot());
        busy = fail_write = corrupt_readback = false;
        assert(settings_save_to_flash());
    }

    auto cleaned = IO_CONFIG;
    io_config_clear_invalid_pins(cleaned, true, 0, 8);
    assert(cleaned.hall[0] == PIN_NONE && cleaned.hall[1] == 1);
    cleaned = IO_CONFIG;
    io_config_clear_invalid_pins(cleaned, false, PIN_NONE, PIN_NONE);
    for (int ch = 0; ch < 4; ++ch) assert(cleaned.hall[ch] == ch);

    candidate = saved;
    project_preset_apply(*project_preset_find("esp32c3-4switch"), candidate);
    for (int pin : candidate.io_config.hall) assert(pin == PIN_NONE);
    settings_restore_snapshot(defaults);
    assert(settings_save_to_flash());
    std::puts("PASS hall_settings: ADC-only options, all pin conflicts, persistence, failures, cleanup, preset reset");
}
