#pragma once

// ============================================================
// Web 管理（仅 ESP32 有效）
//  - 上电连不上已保存的 WiFi 时打开配网热点，浏览器自动弹出配网页
//  - 已保存网络连接失败后等待 30 秒重试，单次限时 15 秒；连接成功后关闭热点，运行中掉线也会重连
//  - 连上 WiFi 后 http://<设备IP>/ 或 http://<mDNS名称>.local/ 访问管理页面
// 非 ESP32 平台（RP2040）为空实现，不影响编译
// ============================================================

#if defined(ARDUINO_ARCH_ESP32)

void web_config_init();  // setup() 中调用
void web_config_run();   // loop() 中调用

// HA 两项状态均有效且确认打印中时，允许当前使用通道执行霍尔停转检测
bool web_config_ha_printing();

#else

static inline void web_config_init() {}
static inline void web_config_run() {}
static inline bool web_config_ha_printing() { return false; }

#endif
