#pragma once

// ============================================================
// Web 管理（仅 ESP32 有效）
//  - 上电连不上已保存的 WiFi 时打开配网热点，浏览器自动弹出配网页
//  - 连上 WiFi 后 http://<设备IP>/ 或 http://<mDNS名称>.local/ 访问管理页面
// 非 ESP32 平台（RP2040）为空实现，不影响编译
// ============================================================

#if defined(ARDUINO_ARCH_ESP32)

void web_config_init();  // setup() 中调用
void web_config_run();   // loop() 中调用

#else

/**
 * 非 ESP32 平台的 Web 初始化空实现
 * RP2040 不提供 Web 管理，保持空实现以维持统一调用接口
 */
static inline void web_config_init() {}
/**
 * 非 ESP32 平台的 Web 轮询空实现
 * RP2040 不处理 Web 请求，保持空实现以维持主循环结构一致
 */
static inline void web_config_run() {}

#endif
