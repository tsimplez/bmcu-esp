/**
 * 发起一次合并读取并检查传输长度
 * @param main_seq 主板游标
 * @param bus_seq 总线游标
 * @param clear_mask 清空位图
 * @param boot 客户端启动标识
 */
static void request_logs(uint32_t main_seq, uint32_t bus_seq, unsigned clear_mask = 0, const char *boot = nullptr) {
    server.reset();
    server.args["both"] = "1";
    server.args["main_seq"] = String(main_seq);
    server.args["bus_seq"] = String(bus_seq);
    if (clear_mask) server.args["clear_mask"] = String(clear_mask);
    if (boot) server.args["boot"] = boot;
    handle_log();
    assert(server.status == 200);
    assert(server.content_type == "application/octet-stream");
    assert(server.content_length == server.body.size());
    assert(server.headers["Cache-Control"] == "no-store");
    assert(server.headers["X-Uptime-Ms"] == "12345");
    assert(server.body.size() == static_cast<size_t>(server.headers["X-Log-Main-Length"].toInt() + server.headers["X-Log-Bus-Length"].toInt()));
}

/**
 * 验证真实环形缓冲和 HTTP 读取、覆盖、清空、启动标识及兼容接口
 * @return 测试状态
 */
int main() {
    log_bus_init();
    const char main_data[] = "中文主板\n\0保留零字节\n";
    const char bus_data[] = "[1] type=0 RX 01\n";
    log_bus_write(LOG_CH_MAIN, main_data, sizeof(main_data) - 1);
    log_bus_write(LOG_CH_BUS, bus_data, sizeof(bus_data) - 1);
    request_logs(0, 0);
    assert(server.body == std::string(main_data, sizeof(main_data) - 1) + bus_data);
    const auto identity = server.headers["X-Log-Boot"];
    assert(server.headers["X-Log-Boundary"].toInt() == sizeof(bus_data) - 1);
    assert(server.headers["X-Log-Main-Dropped"] == "0");
    request_logs(sizeof(main_data) - 1, sizeof(bus_data) - 1, 0, identity.c_str());
    assert(server.body.empty());

    log_bus_init();
    const std::string main_bulk(9000, 'M'), bus_bulk(17000, 'B');
    log_bus_write(LOG_CH_MAIN, main_bulk.data(), main_bulk.size());
    log_bus_write(LOG_CH_BUS, bus_bulk.data(), bus_bulk.size());
    request_logs(0, 0);
    assert(server.body == std::string(8192, 'M') + std::string(8192, 'B'));
    assert(server.headers["X-Log-Main-Seq"] == "8192");
    assert(server.headers["X-Log-Bus-Seq"] == "8192");
    request_logs(8192, 8192);
    assert(server.body == std::string(808, 'M') + std::string(8192, 'B'));
    request_logs(9000, 16384);
    assert(server.body == std::string(616, 'B'));
    request_logs(0, 0, 1);
    assert(server.headers["X-Log-Main-Length"] == "0");
    assert(server.headers["X-Log-Main-Seq"] == "9000");
    assert(server.body == std::string(8192, 'B'));
    request_logs(9000, 17000, 2);
    assert(server.body.empty());

    log_bus_init();
    const std::string overflow(HAL_LOG_MAIN_CAPACITY + 7, 'X');
    log_bus_write(LOG_CH_MAIN, overflow.data(), overflow.size());
    request_logs(0, 0);
    assert(server.headers["X-Log-Main-Dropped"] == "7");
    assert(server.body.find("较早日志已滚动丢弃") != std::string::npos);
    uint32_t seq = 0, dropped = 0;
    char buffer[1024];
    log_bus_read(LOG_CH_MAIN, &seq, buffer, sizeof(buffer), &dropped);
    assert(dropped == 7);
    log_bus_read(LOG_CH_MAIN, &seq, buffer, sizeof(buffer), &dropped);
    assert(dropped == 0);

    log_bus_init();
    log_bus_write(LOG_CH_MAIN, "new\n", 4);
    request_logs(99999, 99999, 0, "older-device-boot");
    assert(server.body == "new\n");
    assert(server.headers["X-Log-Main-Seq"] == "4");
    server.reset();server.args["ch"] = "0";server.args["seq"] = "0";handle_log();
    assert(server.body == "new\n");assert(server.headers["X-Log-Seq"] == "4");
    server.reset();server.args["ch"] = "0";server.args["clear"] = "1";handle_log();
    assert(server.body.empty());
    request_logs(4, 0);assert(server.body.empty());
    puts("PASS: 合并传输、二进制/中文、独立游标、8 KiB 分块、缺口计数、清空、启动标识、旧接口");
    return 0;
}
