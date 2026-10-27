# BambuBus 与设置迁移回归测试

`test_bambu_bus.py` 编译真实 `src/BambuBus.cpp`、`src/bambu_motion_reference.h`、协议声明、平台原子接口和 `src/crc_bus.c`。电机用例另外原样提取 `src/BMCU.cpp` 的 `BMCU_motion()`、`clear_feed_stop_delay()` 和 `arm_feed_stop_delay()`；仅替换硬件与调度上下文，不复制协议或停料算法。

设置测试编译真实 `settings_storage.cpp` 和设置结构；替换存储、日志和平台接线校验边界。本组检查迁移是否保留已通过校验的接线，不能代替真实 `io_config_*_validate()` 的引脚规则测试。

## 运行

已有 GCC/Clang 时：

```powershell
python tools/test_bambu_bus.py
python tools/test_bambu_bus.py --settings
```

可通过 `CXX` 指定编译器，或使用 Zig。Windows 禁止运行新生成的 EXE 时，可用 Zig 0.15.2 或更新版本编译到 WASI，再由 Node.js 执行：

```powershell
python tools/test_bambu_bus.py --zig C:/path/to/zig.exe --wasi
python tools/test_bambu_bus.py --zig C:/path/to/zig.exe --wasi --settings
```

移除 `--wasi` 即运行本机测试。临时产物位于系统临时目录。WASI 不开放文件目录，执行 18 组协议/电机测试，明确跳过仅本机支持的 `queue_threads`。设置套件执行 6 组测试。

## 覆盖内容

| 场景 | 主要检查 |
|---|---|
| parser / receiver / long_flags | 最小载荷、帧长、CRC8/CRC16、地址和通道范围、回显过滤；04/05 两种长帧经真实 UART 收发；序号回显与地址交换 |
| queue / queue_threads | 顺序、满队列丢新帧、持有缓冲、序号回绕；本机并发用例检查 50,000 次发布中的帧完整性 |
| timeout | 启动无通信、错误帧不续期、超时锁存、时间回绕 |
| save | 队列/UART/回调/残帧/收发静默、一秒冷却、耗材记录写入和回读失败重试 |
| zero_time / motion_timing | 零起点及回绕下的 15 秒等待、交替 Cxx/Dxx 不重复计米、计米上限、取消后重新计时 |
| registration / serial_identity | 首次 0C、后续 0A、确认必须匹配 0A 加 16 字节身份；错误身份/CRC 不确认；四台 AMS 身份隔离、注册保持、后缀变更后重新报名 |
| responses / ams_version | 各类应答长度和 CRC、默认 1.0.6.87、自定义版本字节顺序 |
| motion_sequences / motion_isolation | 4 台 AMS × 4 通道的 Cxx、Dxx、交替运动流程；重复 A5 准备；无通道轮询；跨 AMS 计时和当前通道隔离 |
| tx_backpressure / tx_lifetime | 发送忙时保留队首且不改变状态；UART/RX/TX 日志期间重入和发送缓冲所有权 |
| u_compatibility | 45 帧准备回复逐字节核对；结束后恢复模板、前 5 次使用压力、Dxx 和其他 AMS 不推进本通道 Cxx 序列、坏 CRC 和 TX 忙不推进序列、打印中 FF/01 保持通道、1250000 波特率 |
| motor_stop | 真实 BMCU_motion 在 Cxx/Dxx 07/00 后立即发出停车命令，取消未到期延迟；收发处理器本身不操作电机；保留手动、辅助进料和定时退料行为 |

`bambu_u_motion_golden.h` 保存从固定 BMCU-U 源码数据构造的 45 个完整期望帧，CRC 用独立逐位算法生成。它不读取本项目生产表计算期望值，能检查整帧偏移、序号及未知区；这些样例不是本次硬件抓包。

设置迁移 6 组场景：

- `legacy_default`：旧默认变为 1.0.6.87，接线、通道、时长和 OTA 配置逐字节保留，重启不重复写入。
- `legacy_custom`：旧记录中的自定义版本保留。
- `current_custom`：新记录主动设置 90.0.0.0 时不再次迁移。
- `bad_crc`：坏记录不迁移，按原策略恢复默认并校验写入。
- `write_failure`：写入失败保留旧记录和运行参数，下次加载可重试。
- `readback_failure`：回读损坏判定失败并恢复旧运行值，不伪报迁移成功。

## 2026-10-25 验证结果与边界

- WASI：18 组协议/电机测试、6 组设置测试全部通过。
- 本机 EXE：加入最后的电机测试前，18 组测试通过，包含 `queue_threads`；后来重新生成的 EXE 被 Windows 应用控制策略拒绝（WinError 4551），最终完整功能检查使用 WASI。
- `node tools/test_web_config.cjs`：Edge 无头测试通过，覆盖设置校验、草稿、保存失败、通道增减和移动端布局。
- `pio run -e esp32c3`：通过；没有编译其他板型或烧录设备。

现有主机编译有 `String` 与 C linkage 的警告，不影响本次测试通过。电机测试停在 `motorCmd` 边界，没有验证物理输出、电机电流、RS485 响应延迟或连续打印。后台保存只检查开始写入前的总线静默，不能保证写入期间没有新报文到达。
