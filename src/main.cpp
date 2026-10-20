#include "main.h"
#include "BambuBus.h"
#include "settings_storage.h"
#include "log_bus.h"
#include "web_config.h"
#include "version.h"
#include "i2c_bus.h"
#include "env_sensor.h"

/**
 * Arduino 启动初始化入口
 * 依次初始化控制台、看门狗、日志、设置存储、GPIO、I2C、BMCU 核心和 Web 管理服务
 */
void setup(){
    hal_console_begin();  // 串口控制台：不 begin 的话下面所有 LOG_MAIN 都写不出去
    hal_watchdog_init();
    log_bus_init();     // 日志环形缓冲，越早越好，方便抓到启动过程

    // 初始化设置存储（在BMCU_init之前，确保变量先被加载）
    settings_storage_init();
    LOG_MAIN("团子 BMCU 启动，当前版本 %s，构建于 %s %s", FW_VERSION, __DATE__, __TIME__);
    settings_log_all();   // 启动时把保存的设置全部打印一遍，方便排查

    hal_prepare_gpio(); // 先释放可重分配的 USB 焊盘，再初始化所有外设
    i2c_bus_begin();   // PCF8575 与温湿度模块共用 I2C 总线
    env_sensor_init(); // 扫描 AHT20/BMP280，未检测到则忽略
    BMCU_init();

    web_config_init(); // ESP32: 配网 + Web 管理页面；其它平台为空实现
}

/**
 * Arduino 主循环入口
 * 持续喂狗并轮询 BMCU 业务；先完成运动调度，再尝试总线空闲保存，随后处理传感器和 Web
 */
void loop(){
    while (true) {
        // 原有核心逻辑：喂狗 → 串口调试 → BMCU业务
        hal_watchdog_feed();
        BMCU_run();      // Handles BMCU protocol and BambuBus
        BambuBus_save_run(); // 总线空闲时限频保存耗材记录
        env_sensor_run(); // 非阻塞刷新 AHT20/BMP280 读数

        // 检查微动开关状态并更新自动模式下的通道状态
        check_microswitch_status();

        web_config_run();           // 处理 Web 请求（非 ESP32 平台为空实现）

        yield();  // ESP32: 让出 CPU，避免饿死 IDLE 与串口事件任务
    }
}
