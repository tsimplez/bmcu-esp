"""编译真实项目、接线校验、NVS 读取、设置存储和 HTTP handler，隔离硬件后验证切换。"""
import argparse
import os
import shutil
from pathlib import Path
import subprocess
import tempfile

from test_bambu_bus import extract_function, strip_includes


def main():
    """组合生产函数与硬件桩，用主机 C++ 编译器验证项目切换、存储长度及失败路径。"""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cxx', default=os.environ.get('CXX', 'g++'))
    parser.add_argument('--json', type=Path, help='导出真实 GET 响应，供网页测试使用')
    args = parser.parse_args()
    compiler = shutil.which(args.cxx) or args.cxx
    test_env = dict(os.environ)
    test_env['PATH'] = str(Path(compiler).resolve().parent) + os.pathsep + test_env.get('PATH', '')
    root = Path(__file__).resolve().parents[2]

    def read(path):
        """读取项目内 UTF-8 源码。"""
        return (root / path).read_text(encoding='utf-8')

    def header(path):
        """只移除嵌入式 include 和单次包含标记，保留真实声明及常量。"""
        return strip_includes(read(path)).replace('#pragma once', '')

    arduino = read('tools/tests/fixtures/ota_test_support.h').split('struct Response')[0]
    preamble = '''
using std::min;
#define CONFIG_IDF_TARGET_ESP32C3 1
#define C3_BOARD_USB_TTL 1
#define ARDUINO_USB_CDC_ON_BOOT 0
#define PIN_NONE 255
#define PIN_SWITCH PIN_NONE
#define HAL_REGION_SETTINGS 0
#define LOG_MAIN(...) do {} while (0)
extern int BUS_RX, BUS_TX, BUS_DE, PCF_SDA, PCF_SCL;
struct ProjectPreset;
const char *hal_project_target();
const ProjectPreset &hal_default_project_preset();
'''
    pcf = read('src/PCF8575_PinMap.h')
    pcf = pcf[pcf.index('#define PCF8575_VIRTUAL_PIN_BASE'):pcf.index('/**\n * 初始化PCF8575')]
    platform = read('src/platform.cpp')
    web = read('src/web_config.cpp')
    unit = '\n'.join((arduino, preamble, header('src/io_config.h'), header('src/settings_storage.h'),
        header('src/project_presets.h'), pcf, read('tools/tests/fixtures/project_presets_test_support.h'),
        extract_function(platform, 'const char *hal_project_target()'),
        extract_function(platform, 'const ProjectPreset &hal_default_project_preset()'),
        extract_function(platform, 'static const char *region_ns('),
        extract_function(platform, 'void hal_storage_read('),
        extract_function(read('src/i2c_bus.cpp'), 'bool i2c_bus_configured()'),
        strip_includes(read('src/io_config.cpp')), strip_includes(read('src/project_presets.cpp')),
        strip_includes(read('src/settings_storage.cpp')),
        extract_function(web, 'static String json_escape('),
        extract_function(web, 'static bool maintenance_required()'),
        extract_function(web, 'static void handle_projects_get()'),
        extract_function(web, 'static void handle_project_save()'),
        extract_function(read('src/BMCU.cpp'), 'bool BMCU_channel_motor_ready('),
        read('tools/tests/fixtures/project_presets_test_cases.cpp')))
    with tempfile.TemporaryDirectory(prefix='bmcu-project-test-') as directory:
        source = Path(directory) / 'test.cpp'
        binary = Path(directory) / ('test.exe' if os.name == 'nt' else 'test')
        source.write_text(unit, encoding='utf-8')
        subprocess.run([compiler, '-std=c++17', '-O0', '-g', str(source), '-o', str(binary)], check=True)
        subprocess.run([str(binary)], check=True, timeout=30, env=test_env)
        if args.json:
            result = subprocess.run([str(binary), '--json'], capture_output=True, check=True, timeout=30, env=test_env)
            args.json.write_bytes(result.stdout)


if __name__ == '__main__':
    main()
