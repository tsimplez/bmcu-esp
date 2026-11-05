## 工作约定

- 先检查工作区改动，保留用户修改；只处理当前任务，不顺手重构
- 优先用 `rg` 按文件、函数名和中文注释查找；定位说明不用固定行号
- 保持现有风格、UTF-8 和无行尾空格；不手改 `.pio/`、`compile_commands.json` 等生成文件或第三方依赖
- 接口变更同步更新调用方、注释和文档；交付说明改动、验证结果及未验证部分
- 文字最后面后面不要加 '。' 这个句号

## 模块导航

下表省略同名头文件

| 文件 | 职责 / 入口 |
|---|---|
| `src/main.cpp`、`src/platform.cpp` | 启动与主循环 `setup()` / `loop()`；平台适配 `hal_*()` |
| `src/BMCU.cpp`、`src/Motor.cpp` | 电机调度、微动、AS5600 模拟；`BMCU_run()`、`motorCmd()` |
| `src/BambuBus.cpp`、`src/crc_bus.c` | 协议收发、状态机与 CRC；`RX_IRQ()`、`BambuBus_run()` |
| `src/io_config.cpp`、`src/project_presets.cpp` | 引脚配置、冲突校验与接线预设 |
| `src/settings_storage.cpp` | 默认设置、快照回滚、Flash/NVS 保存与回读校验 |
| `src/i2c_bus.cpp`、`src/PCF8575_PinMap.cpp`、`src/env_sensor.cpp`、`src/hall_sensor.cpp` | I2C、扩展引脚、环境传感器与霍尔采样 |
| `src/web_config.cpp`、`web/index.html`、`src/log_bus.cpp` | WiFi、REST API、网页、OTA 与日志 |
| `src/version.h`、`.github/workflows/release.yml` | 固件版本与板型；标签触发构建、Release 和 OTA 发布 |

## 编码约束

- C/C++ 函数定义前只保留一份中文 Doxygen（`/** ... */`）；网页命名 JavaScript 函数使用同样的 JSDoc。首句说明职责，参数写 `@param`，非 `void` 返回值写 `@return`，无参数或返回值不写空标签。新增函数必须有说明；修改时更新原文档块，合并重复说明中的独有行为，保留函数体内必要注释
- 平台差异放在 `src/platform.*`；Web/OTA 放在 `src/web_config.*`，用 `#if defined(ARDUINO_ARCH_ESP32)` 隔离，禁止向 RP2040 引入 ESP32 专属实现或头文件。
- 引脚使用 `IO_CONFIG` 和 `PIN_NONE`；禁止在 `BMCU.cpp` / `BambuBus.cpp` 硬编码 GPIO。物理和扩展引脚统一走 `pinMode_PCF8575()`、`digitalWrite_PCF8575()`、`digitalRead_PCF8575()`；不得放宽冲突检查或未配置电机的通道保护
- 通道数使用 `active_channel_count()`（`AMS_COUNT * 4`），电机命令走 `motorCmd()`；ISR、UART 和 Web 回调只提交运动请求，由主循环执行，禁止直接启停电机
- 设置结构变更同步处理默认值、范围校验、CRC、格式标识和网页字段；只支持当前记录格式，不匹配则恢复默认值，不做历史迁移；保留保存失败回滚及写入后的回读校验
- 协议变更同时核对收发两端、帧长、CRC8/CRC16 和长短包分支；BambuBus 校验统一使用 `src/crc_bus.c`

## 验证与发布

- 按改动范围验证；需要编译时在仓库根目录运行 `pio run -e esp32c3`，其他平台仅在明确要求时验证
- 仅注释或文档改动检查差异、格式和引用即可，无需编译；烧录和串口操作仅用于明确需要设备的任务
- 发布前更新 `src/version.h` 的 `FW_VERSION`（`YY.MM.DD[.修订号]`），推送同版本 `v` 标签；工作流发布三种 ESP32 固件及 `manifest.json` 到 GitHub Release 和 Pages
