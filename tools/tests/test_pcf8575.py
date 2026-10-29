"""使用真实引脚映射和通道检查，模拟四片 PCF8575 的独立总线行为。"""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

from test_bambu_bus import extract_function, strip_includes


def main():
    """编译生产代码与硬件桩，分别运行完整、缺片、未配置和 PWM 场景。"""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cxx', default=os.environ.get('CXX', 'g++'))
    args = parser.parse_args()
    compiler = shutil.which(args.cxx) or args.cxx
    env = dict(os.environ)
    env['PATH'] = str(Path(compiler).resolve().parent) + os.pathsep + env.get('PATH', '')
    root = Path(__file__).resolve().parents[2]

    def read(path):
        """读取项目源码，统一换行符供函数提取使用。"""
        return (root / path).read_text(encoding='utf-8')

    unit = '\n'.join((
        '#include <cstdint>\n#include <cassert>\n#include <cstdio>\n#include <cstring>\n#include <vector>',
        strip_includes(read('src/PCF8575_PinMap.h')).replace('#pragma once', ''),
        read('tools/tests/fixtures/pcf8575_test_support.h'),
        strip_includes(read('src/PCF8575_PinMap.cpp')),
        extract_function(read('src/BMCU.cpp'), 'bool BMCU_channel_motor_ready('),
        extract_function(read('src/BMCU.cpp'), 'void BMCU_apply_channel_modes()'),
        read('tools/tests/fixtures/pcf8575_test_cases.cpp'),
    ))
    with tempfile.TemporaryDirectory(prefix='bmcu-pcf-test-') as directory:
        source = Path(directory) / 'test.cpp'
        binary = Path(directory) / ('test.exe' if os.name == 'nt' else 'test')
        source.write_text(unit, encoding='utf-8')
        subprocess.run([compiler, '-std=c++17', '-O0', '-g', str(source), '-o', str(binary)], check=True)
        for case in ('disabled', 'all', 'sparse', 'pwm'):
            subprocess.run([str(binary), case], check=True, timeout=30, env=env)


if __name__ == '__main__':
    main()
