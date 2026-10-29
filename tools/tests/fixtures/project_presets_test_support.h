// 仅模拟 Flash、HTTP 和设备运行状态；项目接线、CRC、校验与 handler 使用生产函数。
static SettingsData flash_record{};
static int writes = 0, reads = 0, restarts = 0;
static bool fail_write = false, corrupt_readback = false, busy = false, pcf_online = false;
static int response_code = 0;
static String response_body;
static size_t stored_size = sizeof(flash_record);
static bool nvs_open = true, short_read = false;

// NVS 桩只提供字节读取边界，长度检查和损坏填充使用生产 hal_storage_read。
struct Preferences {
    /** 模拟 NVS 可打开或缺失 */
    bool begin(const char *, bool) { return nvs_open; }
    /** 返回模拟记录长度 */
    size_t getBytesLength(const char *) { return stored_size; }
    /** 模拟完整读取、部分读取或回读损坏 */
    size_t getBytes(const char *, void *data, size_t size) {
        ++reads;
        const size_t count = short_read ? size - 1 : size;
        memcpy(data, &flash_record, count);
        if (corrupt_readback) static_cast<unsigned char *>(data)[0] ^= 1;
        return count;
    }
    /** 测试无需关闭真实设备 */
    void end() {}
};

/** 模拟存储写入并允许注入失败 */
static bool hal_storage_write(int, const void *data, uint32_t size) {
    ++writes;
    if (fail_write) return false;
    assert(size == sizeof(flash_record));
    memcpy(&flash_record, data, size);
    stored_size = size;
    return true;
}
/** 模拟设备忙闲条件 */
static bool BMCU_can_maintain() { return !busy; }
/** 模拟已配置扩展芯片的在线状态 */
static bool PCF8575_is_online() { return pcf_online; }
/** 提供与主循环相同的有效通道边界 */
static bool valid_channel(int ch) { return ch >= 0 && ch < active_channel_count(); }
/** 测试不做实际等待 */
static void delay(int) {}

struct TestEsp {
    /** 只计数重启；便于验证重启前运行期设置保持不变 */
    void restart() { ++restarts; }
} ESP;
struct TestServer {
    std::map<String, String> params;
    /** 读取参数存在性 */
    bool hasArg(const char *key) { return params.count(key) != 0; }
    /** 返回请求参数总数 */
    int args() { return int(params.size()); }
    /** 返回对应参数值 */
    String arg(const char *key) { return params[key]; }
    /** 捕获 HTTP 响应 */
    void send(int code, const char *, const String &body) { response_code = code; response_body = body; }
    /** 本测试不缓存响应 */
    void sendHeader(const char *, const char *) {}
} server;
