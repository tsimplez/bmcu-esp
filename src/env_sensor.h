#pragma once

// 温湿度/气压传感器的当前状态与最新读数
struct EnvSensorStatus {
    bool aht20_online;        // AHT20 是否已检测到并成功完成初始化
    bool aht20_valid;         // 最近一次 AHT20 读数是否有效
    bool bmp280_online;       // BMP280 是否已检测到并成功完成初始化
    bool bmp280_valid;        // 最近一次 BMP280 读数是否有效
    float temperature_c;      // AHT20 温度，单位摄氏度
    float humidity_pct;       // AHT20 相对湿度，单位百分比
    float bmp_temperature_c;  // BMP280 温度，单位摄氏度
    float pressure_hpa;       // BMP280 大气压，单位百帕
};

/**
 * 初始化 I2C 温湿度/气压传感器
 * 扫描 AHT20(0x38) 和 BMP280(0x76/0x77)，检测不到的设备保持离线状态。
 */
void env_sensor_init();

/**
 * 周期运行传感器采样状态机
 * 在主循环中调用；AHT20 采用非阻塞采样，缺失设备会定期重新探测。
 */
void env_sensor_run();

/**
 * 获取 AHT20 最新温湿度
 * @param temperature_c 输出温度，单位摄氏度
 * @param humidity_pct 输出湿度，单位百分比
 * @return true 表示传感器在线且读数有效
 * 任一输出参数可为空；无效时不会写入旧值。
 */
bool env_sensor_get_aht20(float *temperature_c, float *humidity_pct);

/**
 * 获取 BMP280 最新温度与大气压
 * @param temperature_c 输出温度，单位摄氏度
 * @param pressure_hpa 输出大气压，单位百帕
 * @return true 表示传感器在线且读数有效
 * 任一输出参数可为空；无效时不会写入旧值。
 */
bool env_sensor_get_bmp280(float *temperature_c, float *pressure_hpa);

/**
 * 获取传感器状态快照
 * @return 当前状态与读数的只读引用
 * 供网页状态接口显示设备在线状态和温湿度。
 */
const EnvSensorStatus &env_sensor_status();
