# AGENTS.md

本文件供 AI 编程助手使用：先按功能地图找到模块，再按函数名和中文注释定位实现。

## 工作顺序

1. 先检查工作区已有改动，保留用户正在修改的内容。
2. 根据下方功能地图查找实现，优先使用 `rg`；涉及测试或发布时，先看 `tools/README.md` 和对应专题文档。
3. 只修改与任务相关的代码；同步更新受影响的接口、注释和文档，不顺手重构其他模块。
4. 按改动范围验证；需要编译固件时，只编译 `esp32c3`，无需遍历所有平台。
5. 交付时简要说明改动、验证结果和未验证的部分。

## 函数注释规范

- C/C++ 函数定义前只保留一份中文 Doxygen 注释，使用 `/** ... */`。
- 不要在同一个函数的文档块前后再写一份 `//` 或普通 `/* ... */` 函数说明。
- 修改已有函数时，直接更新原有文档块；若旧的 `//` 说明与文档块重复，删除旧说明。旧说明中独有的关键行为应合并进文档块，不能直接丢弃。
- 函数体内解释分支、边界条件、硬件限制的 `//` 注释可以保留；文件级说明、分组说明也不属于重复函数注释。
- 文档块首句概括职责；每个参数使用 `@param` 说明；有返回值时使用 `@return`，`void` 函数不写 `@return`，无参数函数不写 `@param`。
- 关键行为、副作用或限制按需写在同一个文档块中，不机械重复函数名或代码，也不留下空标签、空代码标记。
- `web/index.html` 中的命名 JavaScript 函数遵循同样的单份中文 JSDoc 规则；`tools/make_ota.py` 的函数使用中文 docstring。
- 新增函数必须有中文说明；修改函数时核对说明与实现一致。

示例：`src/io_config.cpp` 的默认配置函数只需要下面这一份注释：

```cpp
/**
 * 生成平台默认引脚配置
 * @param config 待填充的 IO 配置
 * 由平台选择默认项目，C3 和网页复用同一份预设接线
 */
void io_config_set_defaults(IoConfig &config) {
    project_preset_io(hal_default_project_preset(), config);
}
```

有返回值的注释可参考 `src/BMCU.cpp` 的 `AS5600_is_online()`。

## 项目概览

- 项目：BMCU-ESP，3D 打印机自动材料更换控制单元（AMS）固件。
- 语言与构建：C++ / C、Arduino 框架、PlatformIO。
- 平台：RP2040（Pico）、ESP32-S3、ESP32-C3、合宙 AirM2M CORE ESP32-C3。
- 通信：BambuBus 协议，UART + RS485，支持 RXTX 与 DE 两种模式。
- 主要能力：多通道耗材管理、电机进退料、微动开关检测、AS5600 磁编码器模拟、设置持久化、日志总线、ESP32 网页管理与 OTA。

## 目录与功能地图

| 路径 | 功能 | 优先查找 |
|---|---|---|
| `src/main.cpp` | 程序入口与主循环 | `setup()`、`loop()` |
| `src/platform.h` / `src/platform.cpp` | 平台适配：引脚、I2C、UART、看门狗、片内 ID、Flash/NVS 存储 | `hal_*()` |
| `src/i2c_bus.cpp` / `.h` | I2C 配置判断、初始化和地址探测 | `i2c_bus_configured()`、`i2c_bus_begin()`、`i2c_bus_probe()` |
| `src/env_sensor.cpp` / `.h` | AHT20 温湿度、BMP280 温度/气压采集，缺失设备定期重试 | `env_sensor_init()`、`env_sensor_run()`、`env_sensor_get_aht20()` |
| `src/BMCU.cpp` / `.h` | 电机、手动队列、微动辅助进料、AS5600 模拟、运行调度 | `BMCU_run()`、`BMCU_motion()`、`motorCmd()`、`check_microswitch_status()` |
| `src/BambuBus.cpp` / `.h` | 协议组帧、校验、状态机、长短包回复、耗材记录 | `BambuBus_run()`、`RX_IRQ()`、`get_package_type()`、`process_motion_state_machine()` |
| `src/settings_storage.cpp` / `.h` | 设置结构、默认值、加载/保存/快照/回滚、CRC32 | `settings_storage_init()`、`settings_save_to_flash()`、`apply_settings_values()` |
| `src/project_presets.h` / `.cpp` | 项目接线预设、候选校验、整套应用和当前配置匹配 | `PROJECT_PRESETS`、`project_preset_validate()`、`project_preset_apply()`、`project_preset_match()` |
| `src/io_config.cpp` / `.h` | 默认接线、引脚合法性、冲突校验、网页引脚选项 | `io_config_set_defaults()`、`io_config_validate()`、`io_config_bus_validate()`、`io_config_pin_available()` |
| `src/PCF8575_PinMap.cpp` / `.h` | 四片 PCF8575（0x20～0x23）起始编号为 50/70/90/110，每片使用 +0～7、+10～17，按片独立缓存与重试 | `PCF8575_*()`、`PCF8575_pin_is_online()`、`digitalRead_PCF8575()` |
| `src/log_bus.cpp` / `.h` | 主板/总线两路环形日志，网页按游标增量读取 | `log_bus_write()`、`log_bus_read()`、`log_bus_hex_typed()` |
| `src/crc_bus.c` / `.h` | BambuBus 专用 CRC8/CRC16 查表 | `bus_crc_init()`、`bus_crc8()`、`bus_crc16()` |
| `src/Motor.cpp` / `.h` | 单个电机的两线方向控制 | `Motor::forward()`、`Motor::backward()`、`Motor::stop()` |
| `src/web_config.cpp` / `.h` | ESP32 WiFi 配网、REST API、日志、OTA；RP2040 为空实现 | `web_config_init()`、`handle_settings_save()`、`handle_ota_*()` |
| `web/index.html` | 编译时嵌入固件的网页管理界面 | 页面、REST 字段及命名 JavaScript 函数 |
| `tools/make_ota.py` | OTA 清单与发布工具 | `main()`、`read_fw_version()`、`md5_of()` |
| `tools/tests/`、`tools/tests/fixtures/` | 主机与网页回归入口、C++ 用例、硬件桩和期望数据 | `tools/README.md`、`docs/TESTING.md` |
| `.pio/`、`compile_commands.json` | 构建产物和生成文件 | 由构建系统维护，不手改 |

专题文档：`docs/PCF8575.md`、`docs/PROJECT_PRESETS.md`、`docs/OTA_SERVER.md`、`docs/WEB_LOGS.md`、`docs/TESTING.md`。

## 关键数据流与方法

### 启动与调度

- `setup()` 初始化控制台、看门狗、日志、设置、GPIO、I2C、环境传感器、BMCU 和 Web。
- `loop()` 持续执行 `BMCU_run()`、`env_sensor_run()`、`check_microswitch_status()`、`web_config_run()` 并喂狗；运动调度后调用 `BambuBus_save_run()`。

### 协议收发与状态

- 接收链路：RS485 -> `RX_IRQ()` 组帧 -> 两槽接收队列 -> `BambuBus_run()` -> `get_package_type()` 集中校验 -> `send_for_*()` 回复 -> 释放队首；每轮最多处理一帧。
- 解析前先预占空闲发送槽，发送与 TX 日志完成后才释放；发送忙时保留队首，不改变业务状态。
- 解析与校验：`package_check_crc16()`、`package_send_with_crc()`、`BambuBus_long_package_analysis()`、`get_package_type()`。
- 回复入口：`send_for_Cxx()`、`send_for_Dxx()`、`send_for_Fxx()`、`send_for_long_packge_*()`、`send_for_Set_filament*()`。
- `map_internal_to_api()`、`set_filament_motion()` 转换内部枚举 `filament_motion_internal` 与对外枚举 `filament_motion_state_set`。
- `refresh_serial_identities()` 为各 AMS 生成统一序列号载荷，注册包复制其中的身份块；`registration_identity_matches()` 在确认前检查身份，不匹配报文按 IGNORED 处理。

### 运动控制

- 控制链路：BambuBus 状态机 -> `get_filament_motion()` -> `BMCU_run()` / `BMCU_motion()` -> `motorCmd()` -> `Motor`。
- Cxx/Dxx 共用 `process_motion_state_machine()` 和 `set_motion_response()`；计时及运动状态按 AMS、通道保存，无通道请求只影响所属 AMS，打印头仍只选中一个全局通道。
- `set_motion_response()` 统一生成当前通道、使用标志、长度、压力和在线位图；内部停车调用 `set_filament_motion()` 不解除打印机阻断。
- 电机与通道：`BMCU_channel_motor_ready()`、`BMCU_manual_pulse()`、`BMCU_stop_motor()`、`BMCU_motor_status()`、`BMCU_can_maintain()`。
- 微动与自动进料：`check_microswitch_status()`、`autoload_blocked()`、`BMCU_apply_channel_modes()`。
- AS5600 模拟：`AS5600_init()`、`AS5600_get_raw_angle()`、`AS5600_get_speed()`、`AS5600_is_online()`。

### 设置与网页

- 网页配置：HTTP handler -> `settings_*` -> 写入 Flash/NVS -> `BMCU_apply_io_config()` / `BMCU_apply_channel_modes()` 应用。
- 设置操作：`settings_snapshot()` 收集，`settings_restore_snapshot()` 回滚，`settings_save_to_flash()` 写入并回读校验。
- 项目配置：`handle_projects_get()` 返回预览；`handle_project_save()` 校验项目后通过 `settings_save_snapshot()` 写入候选设置，重启前不改运行接线。
- 默认项目：`hal_default_project_preset()` 在平台层选择；C3 复用 `PROJECT_PRESETS`，当前格式的有效设置优先加载。
- 耗材保存：`BambuBus_set_need_to_save()` 标记；`BambuBus_save_run()` 确认总线静默后限频写入并回读校验，失败保留标记。
- 网页接口：`handle_status()`、`handle_settings_get()`、`handle_settings_save()`、`handle_bus_save()`、`handle_pcf_save()`、`handle_manual_motor()`。
- OTA：`ota_fetch_manifest()`、`ota_flash_from_url()`、`handle_ota_check()`、`handle_ota_pull()`、`handle_ota_upload()`。

### I2C 与环境数据

- PCF8575、AHT20、BMP280 共用 I2C；`PCF_SDA`、`PCF_SCL` 同时有效才启用总线，不再使用运行期开关。
- `handle_pcf_save()` 保存 SDA/SCL；`handle_status()` 返回 `i2c` 设备在线状态和 `env` 温湿度读数。
- `env_sensor_run()` 采集环境数据；`send_for_Dxx()` 通过 `env_sensor_get_aht20()` 取得 AMS 温湿度：`Dxx_res[6..7]` 为 0.1℃ 小端温度，`Dxx_res[8]` 为湿度百分比。

## 查找示例

```bash
rg -n "BambuBus_run|send_for_Dxx" src/BambuBus.cpp
rg -n "OTA|微动|电机|引脚|设置" src
rg -n "const (refreshStatus|renderBus|saveIoConfig|checkUpdate|pollLog)" web/index.html
```

文档中的定位信息使用文件和函数名，不记录会随改动变化的固定行号。

## 编码边界（必须遵守）

- **改动范围**：保持现有代码风格和 UTF-8 编码，不留行尾多余空格；禁止手改 `.pio/`、`compile_commands.json` 等生成文件，禁止为改变项目行为而修改第三方库 `lib/AS5600`、`lib/CRC`。`lib/CRC` 已排除构建，BambuBus 收发统一使用 `src/crc_bus.c`。
- **平台隔离**：平台差异放在 `src/platform.*`；Web/OTA 差异放在 `src/web_config.*`，并用 `#if defined(ARDUINO_ARCH_ESP32)` 隔离。禁止向 RP2040 构建引入 ESP32 专属头文件或 Web/OTA 实现。
- **引脚访问**：使用 `PIN_NONE` 和配置结构，禁止在 `BMCU.cpp` / `BambuBus.cpp` 中硬编码 GPIO 或绕过 `IO_CONFIG`；物理/PCF8575 引脚统一走 `pinMode_PCF8575()`、`digitalWrite_PCF8575()`、`digitalRead_PCF8575()`。
- **引脚校验**：禁止放宽 `io_config_*_validate()` 的冲突检查；禁止删除 `PIN_NONE` 通道保护，未配置电机的通道必须拒绝启动。
- **通道与电机**：有效通道数统一使用 `active_channel_count()`（`AMS_COUNT * 4`）；电机命令统一走 `motorCmd()`。ISR、UART 回调、Web 回调只做标记，由主循环处理，禁止直接启动或停止电机。
- **设置一致性**：修改设置结构时同步处理默认值、范围校验、CRC、格式标识和网页字段；只支持当前记录格式，不做历史迁移，不匹配则恢复默认值。禁止跳过写入后的回读校验，也不能只改内存而不更新 CRC。
- **协议一致性**：修改协议帧时同步检查收发两端、长度、CRC8/CRC16 和短包/长包分支，禁止只改发送端。

## 构建与验证

需要编译固件时，在仓库根目录执行：

```bash
pio run -e esp32c3
```

- 默认只编译 `esp32c3` 以减少耗时；其他支持的环境为 `pico`、`esp32s3`、`airm2m_core_esp32c3`，仅在任务明确要求时使用。
- 按改动模块选择已有回归入口，命令和依赖见 `tools/README.md`，覆盖范围与验证限制见 `docs/TESTING.md`。
- 仅修改注释或文档时，检查差异、格式和引用即可，无需为此重新编译固件。
- 构建中的 `unused` 警告多为既有代码，应结合本次差异判断，不直接当作本次改动失败。
- 烧录和串口命令仅供需要设备操作的任务使用：`pio run -e esp32c3 -t upload`、`pio device monitor`。
