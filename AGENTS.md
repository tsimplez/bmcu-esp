# AGENTS.md

本文件是写给 AI 编程助手看的项目说明。目标：先看功能地图找到模块，再用函数名和中文注释定位实现。

## 项目概览

- 项目：BMCU-ESP，3D 打印机自动材料更换控制单元（AMS）固件。
- 语言：C++ / C（Arduino 框架），构建系统：PlatformIO。
- 通信：BambuBus 协议，UART + RS485（支持 RXTX 与 DE 两种模式）。
- 平台：RP2040（Pico）、ESP32-S3、ESP32-C3、合宙 AirM2M CORE ESP32-C3。
- 主要能力：多通道耗材管理、电机进退料、微动开关检测、AS5600 磁编码器模拟、设置持久化、日志总线、ESP32 网页管理与 OTA。
- 第三方库：`lib/AS5600`、`lib/CRC`，属于外部依赖，非必要不要修改。

## 启动与数据流

1. `src/main.cpp`：`setup()` 初始化控制台、看门狗、日志、设置、GPIO/I2C、BMCU 和 Web。
2. `src/main.cpp`：`loop()` 持续执行 `BMCU_run()`、`check_microswitch_status()`、`web_config_run()` 并喂狗。
3. 打印机报文：RS485 -> `RX_IRQ()` 组帧 -> 双缓冲 -> `BambuBus_run()` -> `get_package_type()` -> `send_for_*()` 回复。
4. 运动控制：`BambuBus` 状态机 -> `get_filament_motion()` -> `BMCU_run()` / `BMCU_motion()` -> `motorCmd()` -> `Motor`。
5. 网页配置：HTTP handler -> `settings_*` -> 写入 Flash/NVS -> `BMCU_apply_io_config()` / `BMCU_apply_channel_modes()` 应用。
6. 掉电保存：`Bambubus_set_need_to_save()` 标记，`BambuBus_run()` 限频调用 `Bambubus_save()` 并回读校验。

## 目录与功能地图

| 路径 | 功能 | 优先查找 |
|---|---|---|
| `src/main.cpp` | 程序入口与主循环 | `setup()`、`loop()` |
| `src/platform.h` / `src/platform.cpp` | 平台适配：引脚、I2C、UART、看门狗、片内 ID、Flash/NVS 存储 | `hal_*()` |
| `src/BMCU.cpp` / `src/BMCU.h` | 核心业务：电机、手动队列、微动辅助进料、AS5600 模拟、运行调度 | `BMCU_run()`、`BMCU_motion()`、`motorCmd()`、`check_microswitch_status()` |
| `src/BambuBus.cpp` / `src/BambuBus.h` | BambuBus 协议：组帧、校验、状态机、长短包回复、耗材记录 | `BambuBus_run()`、`RX_IRQ()`、`get_package_type()`、`process_motion_state_machine()` |
| `src/settings_storage.cpp` / `.h` | 设置结构、默认值、加载/保存/快照/回滚、CRC32 | `settings_storage_init()`、`settings_save_to_flash()`、`apply_settings_values()` |
| `src/io_config.cpp` / `.h` | 引脚合法性、冲突校验、网页引脚选项 | `io_config_validate()`、`io_config_bus_validate()`、`io_config_pin_available()` |
| `src/PCF8575_PinMap.cpp` / `.h` | PCF8575 虚拟引脚映射与 GPIO 透明分发 | `PCF8575_*()`、`pinMode_PCF8575()`、`digitalRead_PCF8575()` |
| `src/logbus.cpp` / `.h` | 主板/总线两路环形日志，网页按游标增量读取 | `logbus_write()`、`logbus_read()`、`logbus_hex_typed()` |
| `src/crc_bus.c` / `.h` | BambuBus 专用 CRC8/CRC16 查表 | `bus_crc_init()`、`bus_crc8()`、`bus_crc16()` |
| `src/FIFO_buffer.c` / `.h` | 通用环形 FIFO | `FIFO_buffer_*()` |
| `src/Motor.cpp` / `.h` | 单个电机的两线方向控制 | `Motor::forward()`、`Motor::backward()`、`Motor::stop()` |
| `src/web_config.cpp` / `.h` | ESP32 WiFi 配网、REST API、日志、OTA；RP2040 为空实现 | `web_config_init()`、`handle_settings_save()`、`handle_ota_*()` |
| `web/index.html` | 网页管理界面，编译时嵌入固件 | 页面与 REST 字段 |
| `tools/make_ota.py`、`tools/OTA_SERVER.md` | OTA 清单与发布工具 | 升级服务 |
| `.pio/`、`compile_commands.json` | 构建产物/生成文件 | 不手改 |

## 关键方法定位

- 电机与通道：`BMCU_channel_motor_ready()`、`BMCU_manual_pulse()`、`BMCU_stop_motor()`、`BMCU_motor_status()`、`BMCU_can_maintain()`。
- 微动与自动进料：`check_microswitch_status()`、`autoload_blocked()`、`BMCU_apply_channel_modes()`。
- AS5600 模拟：`AS5600_init()`、`AS5600_get_raw_angle()`、`AS5600_get_speed()`、`AS5600_is_online()`。
- 协议解析：`package_check_crc16()`、`package_send_with_crc()`、`Bambubus_long_package_analysis()`、`get_package_type()`。
- 协议回复：`send_for_Cxx()`、`send_for_Dxx()`、`send_for_Fxx()`、`send_for_long_packge_*()`、`send_for_Set_filament*()`。
- 协议状态：内部枚举 `_filament_motion_internal` 与对外枚举 `_filament_motion_state_set` 的转换在 `map_internal_to_api()`、`set_filament_motion()`。
- 设置：`settings_snapshot()` 收集，`settings_restore_snapshot()` 回滚，`settings_save_to_flash()` 写入并回读校验。
- 网页接口：`handle_status()`、`handle_settings_get()`、`handle_settings_save()`、`handle_bus_save()`、`handle_pcf_save()`、`handle_manual_motor()`。
- OTA：`ota_fetch_manifest()`、`ota_flash_from_url()`、`handle_ota_check()`、`handle_ota_pull()`、`handle_ota_upload()`。

## 查找方法

- `src/` 下每个 C/C++ 函数定义前都有中文 Doxygen 注释，统一格式：`@brief`、`@param`、`@return`、``。
- `web/index.html` 的命名 JavaScript 函数使用同样的 JSDoc 中文格式；`tools/make_ota.py` 的每个函数都有中文 docstring。
- 优先按函数名搜索，例如：`rg -n "BambuBus_run|send_for_Dxx" src/BambuBus.cpp`。
- 按功能搜索注释：`rg -n "@brief.*(OTA|微动|电机|引脚|设置)" src`。
- 前端方法按声明名搜索：`rg -n "const (refreshStatus|renderBus|saveIoConfig|checkUpdate|pollLog)" web/index.html`。
- OTA 发布工具入口：`tools/make_ota.py` 的 `main()`；版本、MD5 和清单写入分别由 `read_fw_version()`、`md5_of()`、`main()` 完成。
- 行号会随改动变化，AGENTS.md 只记录文件和函数名，不记录固定行号。

## 编码约定

- 新增或修改函数必须补中文说明，参考 `src/BMCU.cpp` 中的 `AS5600_is_online()`：
  `@brief` 一句话说明职责，`@param` 说明参数，`@return` 说明返回值，`` 说明关键行为或副作用。
- 平台差异只放在 `src/platform.*`；Web/OTA 差异只放在 `src/web_config.*`，并用 `#if defined(ARDUINO_ARCH_ESP32)` 隔离。
- 引脚一律使用 `PIN_NONE` 和配置结构，不直接硬编码 GPIO；物理/PCF8575 引脚统一走 `pinMode_PCF8575()` / `digitalWrite_PCF8575()` / `digitalRead_PCF8575()`。
- 有效通道数统一使用 `active_channel_count()`（`AMS_COUNT * 4`）。
- 电机命令统一走 `motorCmd()`，不要在中断或回调中直接驱动电机。
- 修改设置结构时必须同时处理默认值、范围校验、CRC、加载兼容和网页字段。
- 修改协议帧时必须同步检查长度、CRC8/CRC16、短包/长包分支和收发两端。
- 保持现有代码风格，改动尽量小且聚焦；不要顺手重构无关模块。

## 构建命令

```bash
pio run -e pico                    # RP2040
pio run -e esp32c3                 # ESP32-C3
pio run -e esp32s3                 # ESP32-S3
pio run -e airm2m_core_esp32c3     # 合宙 C3
pio run -e esp32c3 -t upload       # 编译并烧录
pio device monitor                 # 查看串口日志
```

- 提交前至少编译 `esp32c3` 和 `pico`，分别覆盖 Web/OTA 与 RP2040 条件分支。
- 当前两个环境均已验证通过。构建警告中的 `unused` 多为既有代码，不代表本次改动失败。

## Never 规则

- Never 修改 `.pio/`、`compile_commands.json` 等生成文件；它们由构建系统维护。
- Never 为了改项目行为去修改 `lib/AS5600`、`lib/CRC` 等第三方库。
- Never 在 `BMCU.cpp` / `BambuBus.cpp` 中直接使用硬编码引脚或绕过 `IO_CONFIG`。
- Never 在 ISR、UART 回调或 Web 回调中直接启动/停止电机；只做标记，交给主循环。
- Never 跳过设置写入后的回读校验，也不要只改内存不更新 CRC。
- Never 放宽 `io_config_*_validate()` 的引脚冲突检查来“让配置先保存”。
- Never 删除 `PIN_NONE` 通道的保护；未配置电机的通道必须拒绝启动。
- Never 在 RP2040 构建中引入 ESP32 专属头文件或 Web/OTA 实现。
- Never 只改协议发送端而不改接收解析、长度或 CRC。
- Never 提交没有中文函数说明的新函数。
