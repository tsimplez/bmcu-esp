#pragma once

// ============================================================
// 固件版本信息
//   FW_VERSION 使用 YY.MM.DD[.修订号]，与当前板型清单条目的 version 对比
//   发版时改这里、重新编译，再跑 tools/make_ota.py 生成清单
//   FW_TARGET 是 OTA 清单里用来区分不同板子固件的键名
// ============================================================

#define FW_VERSION "26.10.27"

#if defined(CONFIG_IDF_TARGET_ESP32S3)
  #define FW_TARGET "esp32s3"
#elif defined(ARDUINO_AirM2M_CORE_ESP32C3)
  #define FW_TARGET "airm2m_core_esp32c3"
#elif defined(CONFIG_IDF_TARGET_ESP32C3)
  #define FW_TARGET "esp32c3"
#else
  #define FW_TARGET "pico"
#endif

// 编译时间：只有重新编译这个文件才会更新
#define FW_BUILD __DATE__ " " __TIME__
