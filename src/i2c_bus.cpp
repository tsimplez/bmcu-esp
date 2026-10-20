#include "i2c_bus.h"
#include "platform.h"

// I2C 总线是否已经完成 begin；未初始化时不访问硬件
static bool i2c_bus_initialized = false;

/**
 * 判断 I2C 总线是否已配置
 * @return true 表示 SDA、SCL 都已选择有效且不同的主控 GPIO
 * 两个引脚必须同时配置；任一为 PIN_NONE 时表示总线关闭
 */
bool i2c_bus_configured() {
    return PCF_SDA != PIN_NONE && PCF_SCL != PIN_NONE &&
           PCF_SDA >= 0 && PCF_SCL >= 0 && PCF_SDA != PCF_SCL;
}

/**
 * 按当前设置初始化 I2C 总线
 * @return true 表示总线已配置并完成初始化
 * 使用 PCF_SDA/PCF_SCL 和 400kHz 时钟；未配置时直接返回 false
 */
bool i2c_bus_begin() {
    if (!i2c_bus_configured()) {
        i2c_bus_initialized = false;
        return false;
    }

#if defined(ARDUINO_ARCH_ESP32)
    Wire.begin(PCF_SDA, PCF_SCL);
    Wire.setClock(400000);
#elif defined(ARDUINO_ARCH_RP2040)
    Wire.setSDA(PCF_SDA);
    Wire.setSCL(PCF_SCL);
    Wire.begin();
    Wire.setClock(400000);
#endif
    i2c_bus_initialized = true;
    return true;
}

/**
 * 查询 I2C 总线是否已经完成初始化
 * @return true 表示可以访问总线
 * 供 PCF8575 和温湿度模块判断当前能否发起 I2C 访问
 */
bool i2c_bus_ready() {
    return i2c_bus_initialized;
}

/**
 * 探测指定 I2C 地址是否有设备应答
 * @param address 7 位 I2C 地址
 * @return true 表示设备应答成功
 * 仅在总线已初始化时访问硬件，失败不会阻塞主循环
 */
bool i2c_bus_probe(const uint8_t address) {
    if (!i2c_bus_initialized) return false;
    Wire.beginTransmission(address);
    return Wire.endTransmission() == 0;
}
