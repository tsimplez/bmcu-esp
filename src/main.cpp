#include "main.h"
#include "settings_storage.h"
#include "logbus.h"
#include "web_config.h"
#include "version.h"

/**
 * Arduino 启动初始化入口
 * 依次初始化控制台、看门狗、日志、设置存储、GPIO、I2C、BMCU 核心和 Web 管理服务。
 */
void setup(){
    hal_console_begin();  // 串口控制台：不 begin 的话下面所有 LOG_MAIN 都写不出去
    hal_watchdog_init();
    logbus_init();     // 日志环形缓冲，越早越好，方便抓到启动过程

    // 初始化设置存储（在BMCU_init之前，确保变量先被加载）
    settings_storage_init();
    LOG_MAIN("团子 BMCU 启动，当前版本 %s，构建于 %s %s", FW_VERSION, __DATE__, __TIME__);
    settings_log_all();   // 启动时把保存的设置全部打印一遍，方便排查

    hal_prepare_gpio(); // 先释放可重分配的 USB 焊盘，再初始化所有外设
    hal_i2c_begin();   // PCF8575 扩展芯片走 I2C
    BMCU_init();

    web_config_init(); // ESP32: 配网 + Web 管理页面；其它平台为空实现
}

/**
 * Arduino 主循环入口
 * 持续喂狗并轮询 BMCU 业务、微动开关和 Web 请求，最后让出 CPU。
 */
void loop(){
    while (true) {
        // 原有核心逻辑：喂狗 → 串口调试 → BMCU业务
        hal_watchdog_feed();
        BMCU_run();      // Handles BMCU protocol and BambuBus

        // 检查微动开关状态并更新自动模式下的通道状态
        check_microswitch_status();

        web_config_run();           // 处理 Web 请求（非 ESP32 平台为空实现）

        yield();  // ESP32: 让出 CPU，避免饿死 IDLE 与串口事件任务
    }
}
