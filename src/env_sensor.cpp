#include "env_sensor.h"

#include <Wire.h>

#include "i2c_bus.h"
#include "log_bus.h"

// AHT20 温湿度传感器 I2C 地址
#define AHT20_ADDR 0x38
// BMP280 大气压传感器可能使用的两个 I2C 地址
#define BMP280_ADDR_PRIMARY 0x76
#define BMP280_ADDR_SECONDARY 0x77
// 环境数据采样周期（毫秒）
#define ENV_SAMPLE_INTERVAL_MS 5000
// 设备未检测到时的重新探测周期（毫秒）
#define ENV_RETRY_INTERVAL_MS 5000
// AHT20 启动测量后等待转换完成的时间（毫秒）
#define AHT20_MEASURE_WAIT_MS 90

// 当前传感器状态与最新读数
static EnvSensorStatus env_status = {};
// BMP280 当前使用的 I2C 地址；0 表示尚未检测到
static uint8_t bmp280_address = 0;
// BMP280 温度补偿中间量，压力补偿需要复用
static int32_t bmp280_t_fine = 0;
// BMP280 出厂温度校准参数
static uint16_t bmp_dig_t1 = 0;
static int16_t bmp_dig_t2 = 0;
static int16_t bmp_dig_t3 = 0;
// BMP280 出厂压力校准参数
static uint16_t bmp_dig_p1 = 0;
static int16_t bmp_dig_p2 = 0;
static int16_t bmp_dig_p3 = 0;
static int16_t bmp_dig_p4 = 0;
static int16_t bmp_dig_p5 = 0;
static int16_t bmp_dig_p6 = 0;
static int16_t bmp_dig_p7 = 0;
static int16_t bmp_dig_p8 = 0;
static int16_t bmp_dig_p9 = 0;
// AHT20 是否正在等待测量完成
static bool aht20_measuring = false;
// AHT20 本次测量启动时刻
static uint32_t aht20_measure_start_ms = 0;
// AHT20 下一次采样时刻
static uint32_t aht20_next_sample_ms = 0;
// AHT20 下一次重新探测时刻
static uint32_t aht20_next_probe_ms = 0;
// BMP280 下一次采样时刻
static uint32_t bmp280_next_sample_ms = 0;
// BMP280 下一次重新探测时刻
static uint32_t bmp280_next_probe_ms = 0;

/**
 * 从 I2C 设备连续读取寄存器
 * @param address 7 位 I2C 地址
 * @param reg 起始寄存器地址
 * @param buffer 输出缓冲区
 * @param length 读取长度
 * @return true 表示读取完整成功
 * 先写寄存器地址，再读取指定字节数。
 */
static bool i2c_read_registers(const uint8_t address, const uint8_t reg,
                               uint8_t *buffer, const uint8_t length) {
    if (!buffer || !length || !i2c_bus_ready()) return false;
    Wire.beginTransmission(address);
    Wire.write(reg);
    if (Wire.endTransmission(false) != 0) return false;
    if (Wire.requestFrom(address, length) != length) return false;
    for (uint8_t i = 0; i < length; i++) {
        if (!Wire.available()) return false;
        buffer[i] = static_cast<uint8_t>(Wire.read());
    }
    return true;
}

/**
 * 向 I2C 设备写入一个寄存器值
 * @param address 7 位 I2C 地址
 * @param reg 寄存器地址
 * @param value 写入值
 * @return true 表示写入成功
 * 用于配置 BMP280 的工作模式。
 */
static bool i2c_write_register(const uint8_t address, const uint8_t reg, const uint8_t value) {
    if (!i2c_bus_ready()) return false;
    Wire.beginTransmission(address);
    Wire.write(reg);
    Wire.write(value);
    return Wire.endTransmission() == 0;
}

/**
 * 读取 AHT20 状态字节
 * @param status 输出状态字节
 * @return true 表示读取成功
 * 状态寄存器地址为 0x71，bit7 表示测量忙。
 */
static bool aht20_read_status(uint8_t *status) {
    return i2c_read_registers(AHT20_ADDR, 0x71, status, 1);
}

/**
 * 初始化 AHT20 传感器
 * @return true 表示设备在线并完成校准检查
 * 若状态字节显示未校准，则发送 0xBE 0x08 0x00 完成初始化。
 */
static bool aht20_init() {
    if (!i2c_bus_probe(AHT20_ADDR)) return false;

    uint8_t status = 0;
    if (!aht20_read_status(&status)) return false;
    if ((status & 0x18) == 0x18) return true;

    const uint8_t command[3] = {0xBE, 0x08, 0x00};
    Wire.beginTransmission(AHT20_ADDR);
    for (uint8_t i = 0; i < sizeof(command); i++) Wire.write(command[i]);
    if (Wire.endTransmission() != 0) return false;
    delay(10);
    return aht20_read_status(&status) && (status & 0x18) == 0x18;
}

/**
 * 启动一次 AHT20 温湿度测量
 * @return true 表示测量命令发送成功
 * 发送 0xAC 0x33 0x00，转换完成后由 env_sensor_run() 读取结果。
 */
static bool aht20_start_measure() {
    if (!i2c_bus_ready()) return false;
    Wire.beginTransmission(AHT20_ADDR);
    Wire.write(0xAC);
    Wire.write(0x33);
    Wire.write(0x00);
    return Wire.endTransmission() == 0;
}

/**
 * 读取并解析 AHT20 测量结果
 * @return true 表示读数有效
 * 读取 7 字节，按 20 位原始值换算温度和湿度。
 */
static bool aht20_read_measurement() {
    if (!i2c_bus_ready()) return false;
    if (Wire.requestFrom(static_cast<int>(AHT20_ADDR), 7) != 7) return false;

    uint8_t data[7] = {};
    for (uint8_t i = 0; i < sizeof(data); i++) {
        if (!Wire.available()) return false;
        data[i] = static_cast<uint8_t>(Wire.read());
    }
    if (data[0] & 0x80) return false;  // 仍在转换中

    const uint32_t raw_humidity = (static_cast<uint32_t>(data[1]) << 12) |
                                  (static_cast<uint32_t>(data[2]) << 4) |
                                  (data[3] >> 4);
    const uint32_t raw_temperature = ((static_cast<uint32_t>(data[3]) & 0x0F) << 16) |
                                     (static_cast<uint32_t>(data[4]) << 8) | data[5];
    env_status.humidity_pct = raw_humidity * 100.0f / 1048576.0f;
    env_status.temperature_c = raw_temperature * 200.0f / 1048576.0f - 50.0f;
    env_status.aht20_valid = true;
    return true;
}

/**
 * 读取 BMP280 出厂校准参数
 * @return true 表示校准参数读取完整
 * BMP280 的温度/压力校准系数位于 0x88 开始的 24 个字节。
 */
static bool bmp280_read_calibration() {
    uint8_t data[24] = {};
    if (!i2c_read_registers(bmp280_address, 0x88, data, sizeof(data))) return false;

    bmp_dig_t1 = static_cast<uint16_t>(data[0] | (data[1] << 8));
    bmp_dig_t2 = static_cast<int16_t>(data[2] | (data[3] << 8));
    bmp_dig_t3 = static_cast<int16_t>(data[4] | (data[5] << 8));
    bmp_dig_p1 = static_cast<uint16_t>(data[6] | (data[7] << 8));
    bmp_dig_p2 = static_cast<int16_t>(data[8] | (data[9] << 8));
    bmp_dig_p3 = static_cast<int16_t>(data[10] | (data[11] << 8));
    bmp_dig_p4 = static_cast<int16_t>(data[12] | (data[13] << 8));
    bmp_dig_p5 = static_cast<int16_t>(data[14] | (data[15] << 8));
    bmp_dig_p6 = static_cast<int16_t>(data[16] | (data[17] << 8));
    bmp_dig_p7 = static_cast<int16_t>(data[18] | (data[19] << 8));
    bmp_dig_p8 = static_cast<int16_t>(data[20] | (data[21] << 8));
    bmp_dig_p9 = static_cast<int16_t>(data[22] | (data[23] << 8));
    return bmp_dig_p1 != 0;
}

/**
 * 在指定地址初始化 BMP280
 * @param address 候选 I2C 地址
 * @return true 表示芯片 ID 和校准参数均有效
 * 读取芯片 ID(0xD0) 应为 0x58，然后配置为温压连续测量模式。
 */
static bool bmp280_init_at(const uint8_t address) {
    bmp280_address = address;
    if (!i2c_bus_probe(address)) {
        bmp280_address = 0;
        return false;
    }

    uint8_t chip_id = 0;
    // 兼容 BMP280(0x58) 和常见的 BME280(0x60) 模块；两者温压校准寄存器一致。
    if (!i2c_read_registers(address, 0xD0, &chip_id, 1) ||
        (chip_id != 0x58 && chip_id != 0x60) ||
        !bmp280_read_calibration()) {
        bmp280_address = 0;
        return false;
    }

    // 温度与压力各 1 倍过采样，Normal 模式，便于周期读取。
    if (!i2c_write_register(address, 0xF4, 0x27)) {
        bmp280_address = 0;
        return false;
    }
    return true;
}

/**
 * 检测并初始化 BMP280
 * @return true 表示 0x76 或 0x77 上找到可用设备
 * 优先尝试 0x76，失败后再尝试 0x77。
 */
static bool bmp280_init() {
    if (bmp280_init_at(BMP280_ADDR_PRIMARY)) return true;
    return bmp280_init_at(BMP280_ADDR_SECONDARY);
}

/**
 * 读取并补偿 BMP280 温度与大气压
 * @return true 表示读数有效
 * 读取 0xF7 开始的 8 字节原始数据，按 Bosch 数据手册公式补偿。
 */
static bool bmp280_read_measurement() {
    if (!bmp280_address) return false;
    uint8_t data[8] = {};
    if (!i2c_read_registers(bmp280_address, 0xF7, data, sizeof(data))) return false;

    const int32_t adc_pressure = (static_cast<int32_t>(data[0]) << 12) |
                                 (static_cast<int32_t>(data[1]) << 4) | (data[2] >> 4);
    const int32_t adc_temperature = (static_cast<int32_t>(data[3]) << 12) |
                                    (static_cast<int32_t>(data[4]) << 4) | (data[5] >> 4);

    const int32_t temp_var1 =
        ((((adc_temperature >> 3) - (static_cast<int32_t>(bmp_dig_t1) << 1))) * bmp_dig_t2) >> 11;
    const int32_t temp_var2 =
        (((((adc_temperature >> 4) - static_cast<int32_t>(bmp_dig_t1)) *
            ((adc_temperature >> 4) - static_cast<int32_t>(bmp_dig_t1))) >> 12) * bmp_dig_t3) >> 14;
    bmp280_t_fine = temp_var1 + temp_var2;
    const int32_t temperature_centi_c = (bmp280_t_fine * 5 + 128) >> 8;

    int64_t pressure_var1 = static_cast<int64_t>(bmp280_t_fine) - 128000;
    int64_t pressure_var2 = pressure_var1 * pressure_var1 * bmp_dig_p6;
    pressure_var2 += (pressure_var1 * bmp_dig_p5) << 17;
    pressure_var2 += static_cast<int64_t>(bmp_dig_p4) << 35;
    pressure_var1 = ((pressure_var1 * pressure_var1 * bmp_dig_p3) >> 8) +
                    ((pressure_var1 * bmp_dig_p2) << 12);
    pressure_var1 = ((static_cast<int64_t>(1) << 47) + pressure_var1) * bmp_dig_p1 >> 33;
    if (pressure_var1 == 0) return false;

    int64_t pressure = 1048576 - adc_pressure;
    pressure = ((pressure << 31) - pressure_var2) * 3125 / pressure_var1;
    const int64_t pressure_var1b = (bmp_dig_p9 * (pressure >> 13) * (pressure >> 13)) >> 25;
    const int64_t pressure_var2b = (bmp_dig_p8 * pressure) >> 19;
    pressure = ((pressure + pressure_var1b + pressure_var2b) >> 8) +
               (static_cast<int64_t>(bmp_dig_p7) << 4);

    env_status.bmp_temperature_c = temperature_centi_c / 100.0f;
    env_status.pressure_hpa = static_cast<float>(pressure >> 8) / 100.0f;
    env_status.bmp280_valid = true;
    return true;
}

/**
 * 初始化 I2C 温湿度/气压传感器
 * 扫描 AHT20(0x38) 和 BMP280(0x76/0x77)，检测不到的设备保持离线状态。
 */
void env_sensor_init() {
    env_status = {};
    aht20_measuring = false;
    aht20_next_sample_ms = aht20_next_probe_ms = 0;
    bmp280_next_sample_ms = bmp280_next_probe_ms = 0;
    bmp280_address = 0;

    if (!i2c_bus_ready()) {
        LOG_MAIN("[i2c] 未配置 SDA/SCL，PCF8575 与温湿度模块不启用");
        return;
    }

    const uint32_t now = millis();
    env_status.aht20_online = aht20_init();
    env_status.bmp280_online = bmp280_init();
    aht20_next_sample_ms = now;
    bmp280_next_sample_ms = now;

    LOG_MAIN("[i2c] AHT20=%s，BMP280=%s（地址 0x%02X）",
             env_status.aht20_online ? "已检测" : "未检测",
             env_status.bmp280_online ? "已检测" : "未检测", bmp280_address);
}

/**
 * 周期运行传感器采样状态机
 * 在主循环中调用；AHT20 采用非阻塞采样，缺失设备会定期重新探测。
 */
void env_sensor_run() {
    if (!i2c_bus_ready() || !i2c_bus_configured()) return;
    const uint32_t now = millis();

    if (!env_status.aht20_online) {
        if (now - aht20_next_probe_ms >= ENV_RETRY_INTERVAL_MS) {
            aht20_next_probe_ms = now;
            if (aht20_init()) {
                env_status.aht20_online = true;
                aht20_next_sample_ms = now;
                LOG_MAIN("[i2c] 已重新检测到 AHT20");
            }
        }
    } else if (aht20_measuring) {
        if (now - aht20_measure_start_ms >= AHT20_MEASURE_WAIT_MS) {
            aht20_measuring = false;
            if (!aht20_read_measurement()) {
                env_status.aht20_valid = false;
                env_status.aht20_online = false;
                aht20_next_probe_ms = now;
                LOG_MAIN("[i2c] AHT20 读取失败，暂停使用并稍后重试");
            }
            aht20_next_sample_ms = now + ENV_SAMPLE_INTERVAL_MS;
        }
    } else if (static_cast<int32_t>(now - aht20_next_sample_ms) >= 0) {
        if (aht20_start_measure()) {
            aht20_measuring = true;
            aht20_measure_start_ms = now;
        } else {
            env_status.aht20_valid = false;
            env_status.aht20_online = false;
            aht20_next_probe_ms = now;
            LOG_MAIN("[i2c] AHT20 启动测量失败，暂停使用并稍后重试");
        }
    }

    if (!env_status.bmp280_online) {
        if (now - bmp280_next_probe_ms >= ENV_RETRY_INTERVAL_MS) {
            bmp280_next_probe_ms = now;
            if (bmp280_init()) {
                env_status.bmp280_online = true;
                bmp280_next_sample_ms = now;
                LOG_MAIN("[i2c] 已重新检测到 BMP280（地址 0x%02X）", bmp280_address);
            }
        }
    } else if (static_cast<int32_t>(now - bmp280_next_sample_ms) >= 0) {
        bmp280_next_sample_ms = now + ENV_SAMPLE_INTERVAL_MS;
        if (!bmp280_read_measurement()) {
            env_status.bmp280_valid = false;
            env_status.bmp280_online = false;
            bmp280_address = 0;
            bmp280_next_probe_ms = now;
            LOG_MAIN("[i2c] BMP280 读取失败，暂停使用并稍后重试");
        }
    }
}

/**
 * 获取 AHT20 最新温湿度
 * @param temperature_c 输出温度，单位摄氏度
 * @param humidity_pct 输出湿度，单位百分比
 * @return true 表示传感器在线且读数有效
 * 任一输出参数可为空；无效时不会写入旧值。
 */
bool env_sensor_get_aht20(float *temperature_c, float *humidity_pct) {
    if (!env_status.aht20_online || !env_status.aht20_valid) return false;
    if (temperature_c) *temperature_c = env_status.temperature_c;
    if (humidity_pct) *humidity_pct = env_status.humidity_pct;
    return true;
}

/**
 * 获取 BMP280 最新温度与大气压
 * @param temperature_c 输出温度，单位摄氏度
 * @param pressure_hpa 输出大气压，单位百帕
 * @return true 表示传感器在线且读数有效
 * 任一输出参数可为空；无效时不会写入旧值。
 */
bool env_sensor_get_bmp280(float *temperature_c, float *pressure_hpa) {
    if (!env_status.bmp280_online || !env_status.bmp280_valid) return false;
    if (temperature_c) *temperature_c = env_status.bmp_temperature_c;
    if (pressure_hpa) *pressure_hpa = env_status.pressure_hpa;
    return true;
}

/**
 * 获取传感器状态快照
 * @return 当前状态与读数的只读引用
 * 供网页状态接口显示设备在线状态和温湿度。
 */
const EnvSensorStatus &env_sensor_status() {
    return env_status;
}
