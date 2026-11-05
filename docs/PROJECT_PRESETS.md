# 项目配置预设

网页右上角选择项目后弹窗显示接线预览，点击弹窗内的“应用并重启”才提交。点击“取消”或按 Esc 关闭弹窗并恢复当前项目，保存期间禁止关闭。默认 C3 项目为 `esp32c3(4色4微动)`；团子 PCB 项目保留 2 个 AMS，通道5～8全部留空。

## 添加项目

在 `src/project_presets.h` 的 `PROJECT_PRESETS` 中追加一条记录，再编译更新固件。网页通过 `GET /api/projects` 自动读取列表，无需修改 HTML 或单独增加保存分支。

每条记录按顺序包含：

1. 稳定且唯一的 ID（建议小写英文字母、数字和连字符）。
2. 网页显示名称。
3. 芯片标识：`esp32c3`、`esp32s3` 或 `pico`。当前网页仅在 ESP32 上提供。
4. AMS 数量，范围 1～4。
5. 总线参数 `{RX, TX, DE, RS485模式, SDA, SCL}`。
6. 已定义接线的通道数，不得超过 AMS 数量 × 4。
7. 按通道排列的 `{进料, 退料, 微动}` 数组；中间留空的通道须显式填写三个 `PIN_NONE`。
8. 可选 `switch_pin`：五通总微动输入，未填写时使用平台 `PIN_SWITCH` 默认值；支持真实或扩展引脚，必须与总线及启用通道接线互不冲突。
9. 可选 `hall[16]`：各通道 49E 的主控 ADC 引脚，默认全部 `PIN_NONE`；只允许平台支持的模拟输入，参与整套接线冲突校验。网页预览的 `pins` 第四列显示霍尔引脚，详情见 [霍尔采样说明](HALL_SENSOR.md)。

未定义的后续通道统一清为 `PIN_NONE`，包含当前未启用的通道。RS485 模式1的 DE 填 `PIN_NONE`；I2C 不启用时 SDA/SCL 同时填 `PIN_NONE`。PCF8575 首片使用 50～57、60～67 虚拟编号，后续每片起始编号加 20，尾数 8、9 留空，并须同时配置有效的 I2C 接线；四片完整映射见 [PCF8575 扩展说明](PCF8575.md)。团子预设的微动为 62～65，对应首片 P12～P15。

不兼容当前芯片的项目不列出；同芯片下使用系统保留引脚的项目显示为不可用。后端仍会校验请求，不能绕过引脚冲突检查。

## 默认值与保存规则

- `platform.cpp` 的 `hal_default_project_preset()` 选择平台默认项目。C3 使用表中第一项，原生 USB 板在平台层适配 UART；其他平台保留原有默认接线。
- C3 的电机、微动和总线项目接线只在预设表维护；I2C 只保存 SDA/SCL。
- 首次启动、记录格式不匹配、CRC 错误或恢复默认时采用平台默认项目；当前格式的有效设置优先加载。
- 设置仅保留一个 `SETTINGS_MAGIC_NUMBER`，不迁移历史字段或 AMS 版本。当前格式 `0x5341542E` 包含进料停止延迟、五通总微动和每通道霍尔引脚；不匹配的旧记录恢复整份默认值，接线与参数需要重新配置。
- 项目覆盖总线、I2C、AMS 数量、全部通道和五通总微动接线。通道模式、进料停止延迟等时长、序列号、OTA 等保留，电机并发上限约束到新通道数。现有 C3 预设的五通总微动均为“无”；Pico 默认也不配置，避免与 UART0 TX 的 GPIO0 冲突，S3 默认 GPIO42。
- `POST /api/projects` 只接受 `id`，设备忙时拒绝保存。候选配置通过校验后一次写入、重算 CRC 并回读，成功后重启；重启前运行期仍使用原设置。
- 不保存额外的项目 ID。根据实际接线匹配名称，总微动和所有通道的电机、微动、霍尔都参与匹配，模式1忽略未使用的 DE。切换项目会重置霍尔到该项目的默认接线；手动保存接线后不匹配任何预设时显示“自定义”。

## 验证

主机测试使用生产项目表、引脚校验、设置存储和 HTTP handler，只模拟硬件边界。网页测试读取主机测试导出的真实 API 响应。

```powershell
python tools/tests/test_project_presets.py --cxx <g++路径> --json "$env:TEMP/bmcu-projects.json"
node tools/tests/test_project_presets_web.cjs "$env:TEMP/bmcu-projects.json"
python tools/tests/test_bambu_bus.py --settings
node tools/tests/test_web_config.cjs
pio run -e esp32c3
```

Node 测试需要可解析的 `playwright` 包和 Microsoft Edge；可通过 `NODE_PATH` 指向现有依赖目录。设置存储测试通过 `CXX` 选择 C++ 编译器。设置 `WEB_TEST_SCREENSHOT_DIR` 可保存项目预览的桌面与手机截图。
