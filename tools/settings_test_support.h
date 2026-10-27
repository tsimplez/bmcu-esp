// 设置测试只替换存储与接线校验边界；记录布局、CRC、迁移和回滚使用生产源码。
#include <cassert>
#include <cstring>
#include <cstdio>
#include <algorithm>
using std::min;
static constexpr int PIN_NONE = 255, PIN_BUS_RX = 6, PIN_BUS_TX = 7, PIN_BUS_DE = 10;
static constexpr int PIN_I2C_SDA = 4, PIN_I2C_SCL = 5, HAL_REGION_SETTINGS = 0;
extern int BUS_RX, BUS_TX, BUS_DE, PCF_SDA, PCF_SCL;
static SettingsData flash_record{};
static unsigned read_count = 0, write_count = 0, validation_count = 0;
static bool fail_write = false, corrupt_readback = false;
#define LOG_MAIN(...) do {} while (0)

/** 模拟读取完整记录，仅对写入后的回读注入损坏 */
static void hal_storage_read(int region, void *out, uint32_t size) {
    assert(region == HAL_REGION_SETTINGS && size == sizeof(flash_record));
    ++read_count;
    memcpy(out, &flash_record, size);
    if (corrupt_readback && write_count) static_cast<uint8_t *>(out)[0] ^= 1;
}
/** 模拟持久化写入，可拒绝写入以验证旧记录和运行参数保留 */
static bool hal_storage_write(int region, const void *data, uint32_t size) {
    assert(region == HAL_REGION_SETTINGS && size == sizeof(flash_record));
    ++write_count;
    if (fail_write) return false;
    memcpy(&flash_record, data, size);
    return true;
}
/** 测试默认不分配电机或微动引脚 */
void io_config_set_defaults(IoConfig &config) {
    for (auto &motor : config.motor) motor[0] = motor[1] = PIN_NONE;
    for (auto &pin : config.microswitch) pin = PIN_NONE;
}
/** 模拟已经通过平台验证的总线接线，不在此重复测试引脚校验实现 */
bool io_config_bus_validate(int, int, int, int, int, int, const IoConfig &, int, String *) {
    ++validation_count;
    return true;
}
/** 模拟已经通过平台验证的 I2C 接线 */
bool io_config_i2c_validate(int, int, const IoConfig &, int, String *) { return true; }
/** 模拟已经通过平台验证的通道接线 */
bool io_config_validate(const IoConfig &, int, String *) { return true; }
/** 为测试中的日志分支提供引脚名称 */
String io_config_pin_label(int pin) { return std::to_string(pin); }
/** 模拟两根有效引脚配置的 I2C 总线 */
static bool i2c_bus_configured() { return PCF_SDA != PIN_NONE && PCF_SCL != PIN_NONE && PCF_SDA != PCF_SCL; }
