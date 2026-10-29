# 工具目录

`tools` 顶层保留日常发布入口；测试脚本统一放在 `tests`，C++ 测试用例、硬件桩和期望帧放在 `tests/fixtures`。辅助文件由测试脚本读取，无需单独运行。

```text
tools/
├── make_ota.py              # OTA 清单与固件打包
├── README.md                # 本说明
└── tests/
    ├── test_*.py            # 固件逻辑的主机测试
    ├── test_*.cjs           # 网页交互测试
    └── fixtures/           # C++ 测试用例、硬件桩与期望数据
```

## OTA 发布

在仓库根目录运行 `python tools/make_ota.py --help` 查看参数。发布入口及参数保持不变，完整流程见 [OTA 发布说明](../docs/OTA_SERVER.md)。

## 测试入口

以下路径相对于本目录；运行命令时以仓库根目录为工作目录。

| 入口 | 检查内容 | 依赖 |
|---|---|---|
| `tests/test_bambu_bus.py` | 协议收发、状态机、电机调度；加 `--settings` 检查设置加载、默认值和失败回滚 | Python、GCC/Clang 或 Zig；WASI 模式另需 Node.js |
| `tests/test_as5600.py` | 模拟采样、时间边界、重置及统计特征 | Python、Zig、Node.js |
| `tests/test_ota.py` | OTA 清单、下载回退和写入失败路径 | Python、Zig、Node.js |
| `tests/test_project_presets.py` | 项目预设、接线校验、存储和 HTTP 接口 | Python、GCC/Clang |
| `tests/test_pcf8575.py` | 四片扩展器的 64 位映射、独立重试、输入输出及通道状态 | Python、GCC/Clang |
| `tests/test_web_config.cjs` | 设置、OTA 交互和移动端布局 | Node.js、Playwright、Edge |
| `tests/test_web_logs.cjs` | 双日志环形缓存、单请求轮询、虚拟滚动、下载/抓取及持续写满测试 | Node.js、Playwright、Edge |
| `tests/test_log_bus.py` | 真实日志缓冲与合并 HTTP 接口的分块、游标、覆盖和清空 | Python、Zig、Node.js |
| `tests/test_project_presets_web.cjs` | 项目下拉框、预览弹窗、应用与失败处理 | Node.js、Playwright、Edge；需上面的项目测试导出 JSON |

常用命令：

```powershell
# 已有 GCC/Clang 时；通过 CXX 环境变量选择编译器
python -B tools/tests/test_bambu_bus.py
python -B tools/tests/test_bambu_bus.py --settings

# Zig 0.15.2 或更新版本；WASI 运行需要 Node.js
python -B tools/tests/test_bambu_bus.py --zig C:/path/to/zig.exe --wasi
python -B tools/tests/test_bambu_bus.py --zig C:/path/to/zig.exe --wasi --settings
python -B tools/tests/test_as5600.py --zig C:/path/to/zig.exe
python -B tools/tests/test_ota.py --zig C:/path/to/zig.exe

# 项目网页测试使用真实后端导出的预览数据
python -B tools/tests/test_project_presets.py --cxx C:/path/to/g++.exe --json "$env:TEMP/bmcu-projects.json"
node tools/tests/test_project_presets_web.cjs "$env:TEMP/bmcu-projects.json"
node tools/tests/test_web_config.cjs
node tools/tests/test_web_logs.cjs
python -B tools/tests/test_log_bus.py --zig C:/path/to/zig.exe
```

将示例编译器路径换为本机路径。网页测试需要 Node.js 能找到 `playwright` 包，可通过 `NODE_PATH` 指向已有依赖目录；默认使用已安装的 Microsoft Edge，也可用 `BROWSER_CHANNEL` 指定 Playwright 支持且本机已安装的浏览器渠道。设置 `WEB_TEST_SCREENSHOT_DIR` 可保存项目预览截图。

测试在临时目录构建，模拟设备接口，不连接硬件。WASI 模式跳过多线程队列用例；详细覆盖与验证边界见 [测试说明](../docs/TESTING.md)，项目扩展方法见 [项目配置预设](../docs/PROJECT_PRESETS.md)，日志缓存、接口和性能验证见 [网页日志](../docs/WEB_LOGS.md)。
