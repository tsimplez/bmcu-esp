#pragma once

// ============================================================
// Web 管理（仅 ESP32 有效）
//  - 上电连不上已保存的 WiFi 时打开配网热点，浏览器自动弹出配网页
//  - 连上 WiFi 后 http://<设备IP>/ 或 http://bmcu.local/ 访问管理页面
// 非 ESP32 平台（RP2040）为空实现，不影响编译
// ============================================================

#if defined(ARDUINO_ARCH_ESP32)

void web_config_init();  // setup() 中调用
void web_config_run();   // loop() 中调用

#else

static inline void web_config_init() {}
static inline void web_config_run() {}

#endif