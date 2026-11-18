#include "main.h"
#include "BambuBus.h"
#include "settings_storage.h"
#include "log_bus.h"
#include "web_config.h"
#include "version.h"
#include "i2c_bus.h"
#include "env_sensor.h"
#include "hall_sensor.h"

/**
 * 初始化平台、已保存接线及业务模块
 * C3 在读取设置前停用可复用控制台，读取接线后启用空闲的日志接口，再初始化外设
 */
void setup() {
    hal_console_prepare(); // C3 读取接线前暂停 UART0，合宙同时停用原生 USB，避免驱动已分配的电机引脚
    hal_watchdog_init();
    log_bus_init(); // 日志环形缓冲，越早越好，方便抓到启动过程

    settings_storage_init(); // 初始化设置存储（在BMCU_init之前，确保变量先被加载）
    hal_prepare_gpio(); // 先释放可重分配的 USB 焊盘，再按接线启用控制台及外设
    hal_console_begin(); // 按已加载的接线决定是否启用 UART0 / 原生 USB，网页日志始终保留
    LOG_MAIN("团子 BMCU 启动，当前版本 %s，构建于 %s %s", FW_VERSION, __DATE__, __TIME__);
    settings_log_all(); // 启动时把保存的设置全部打印一遍，方便排查

    i2c_bus_begin(); // PCF8575 与温湿度模块共用 I2C 总线
    env_sensor_init(); // 扫描 AHT20/BMP280，未检测到则忽略
    BMCU_init();
    hall_sensor_init();

    web_config_init(); // ESP32: 配网 + Web 管理页面；其它平台为空实现
}

void loop() {
    while (true) {
        hal_watchdog_feed(); // 看门狗
        BMCU_run();
        BambuBus_save_run(); // 总线空闲时限频保存离线设置和耗材记录
        hall_sensor_run(); // 每轮最多一个 ADC 转换，持续保留最近一分钟的统计
        env_sensor_run(); // 非阻塞刷新 AHT20/BMP280 读数
        check_microswitch_status(); // 检查微动开关状态并更新自动模式下的通道状态
        web_config_run(); // 处理 Web 请求（非 ESP32 平台为空实现）
        yield(); // ESP32: 让出 CPU，避免饿死 IDLE 与串口事件任务
    }
}
