"""编译并运行真实 BambuBus 源码的主机回归测试；只替换硬件依赖，不复制协议逻辑"""

import argparse
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile


def strip_includes(source):
    """移除嵌入式头文件引用，由测试桩提供硬件接口，保留全部函数体和控制流"""
    return re.sub(r'^\s*#include[^\n]*\n', '', source, flags=re.MULTILINE)


def extract_function(source, signature):
    """从生产代码提取完整函数体，用真实主循环和延迟逻辑测试电机边界，不复制算法"""
    start = source.index(signature)
    opening = source.index('{', start)
    depth = 0
    for position in range(opening, len(source)):
        if source[position] == '{':
            depth += 1
        elif source[position] == '}':
            depth -= 1
            if depth == 0:
                return source[start:position + 1]
    raise ValueError('函数体不完整：' + signature)


def main():
    """在临时目录生成测试翻译单元，编译真实协议与 CRC 源码并逐个执行隔离场景"""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--zig', type=Path, help='可选的 zig 可执行文件，用其 c++ 子命令编译')
    parser.add_argument('--wasi', action='store_true', help='用 Zig 编译为 WebAssembly 并由 Node WASI 沙箱执行（不运行线程压测）')
    parser.add_argument('--settings', action='store_true', help='改为测试真实设置存储代码的当前格式加载与失败回滚')
    args = parser.parse_args()
    if args.wasi and not args.zig:
        parser.error('--wasi 需要 --zig')
    compiler = [str(args.zig), 'c++'] if args.zig else [os.environ.get('CXX', 'g++')]
    if not args.zig and not shutil.which(compiler[0]):
        parser.error('找不到 C++ 编译器，请设置 CXX 或使用 --zig 指定已有编译器')
    root = Path(__file__).resolve().parents[2]
    bus = strip_includes((root / 'src/BambuBus.cpp').read_text(encoding='utf-8'))
    reference = strip_includes((root / 'src/bambu_motion_reference.h').read_text(encoding='utf-8')).replace('#pragma once\n', '')
    header = strip_includes((root / 'src/BambuBus.h').read_text(encoding='utf-8')).replace('#pragma once\n', '')
    platform = (root / 'src/platform.h').read_text(encoding='utf-8')
    atomics = platform[platform.index('/**\n * 原子读取共享的 32 位值'):]
    atomics = atomics[:atomics.index('// ============================================================')]
    support = (root / 'tools/tests/fixtures/bambu_bus_test_support.h').read_text(encoding='utf-8')
    cases = (root / 'tools/tests/fixtures/bambu_bus_test_cases.cpp').read_text(encoding='utf-8')
    motor_source = (root / 'src/BMCU.cpp').read_text(encoding='utf-8')
    motor_support = (root / 'tools/tests/fixtures/bambu_motor_test_support.h').read_text(encoding='utf-8')
    motor_functions = '\n'.join(extract_function(motor_source, signature) for signature in (
        'static void clear_feed_stop_delay(', 'static bool arm_feed_stop_delay(', 'void BMCU_motion() {'))
    translation_unit = '\n'.join((support, header, atomics, reference, bus, motor_support, motor_functions, cases))
    scenarios = ('parser', 'queue', 'queue_threads', 'receiver', 'timeout', 'save',
                 'zero_time', 'registration', 'responses', 'motion_sequences',
                 'motion_isolation', 'motion_timing', 'serial_identity',
                 'ams_version', 'tx_backpressure', 'tx_lifetime', 'u_compatibility', 'long_flags', 'motor_stop')
    if args.settings:
        preamble = '#include <cstdint>\n#include <string>\nusing String = std::string;\n'
        io_header = strip_includes((root / 'src/io_config.h').read_text(encoding='utf-8')).replace('#pragma once\n', '')
        settings_header = strip_includes((root / 'src/settings_storage.h').read_text(encoding='utf-8')).replace('#pragma once\n', '')
        settings_source = strip_includes((root / 'src/settings_storage.cpp').read_text(encoding='utf-8'))
        settings_support = (root / 'tools/tests/fixtures/settings_test_support.h').read_text(encoding='utf-8')
        settings_cases = (root / 'tools/tests/fixtures/settings_test_cases.cpp').read_text(encoding='utf-8')
        translation_unit = '\n'.join((preamble, io_header, settings_header, settings_support, settings_source, settings_cases))
        scenarios = ('current_custom', 'bad_magic', 'bad_crc', 'empty', 'write_failure', 'readback_failure', 'default_write_failure')
    with tempfile.TemporaryDirectory(prefix='bmcu-bus-test-') as directory:
        folder = Path(directory)
        source = folder / 'test.cpp'
        binary = folder / ('test.wasm' if args.wasi else 'test.exe' if os.name == 'nt' else 'test')
        source.write_text(translation_unit, encoding='utf-8')
        target_flags = ['-target', 'wasm32-wasi', '-DBMCU_TEST_WASI=1', '-fno-exceptions'] if args.wasi else ['-pthread']
        subprocess.run(compiler + target_flags + ['-std=c++17', '-O0', '-g', '-I', str(root / 'src'),
                                   '-I', str(root / 'tools/tests/fixtures'), str(source), '-x', 'c++', str(root / 'src/crc_bus.c'),
                                   '-o', str(binary)], check=True)
        for case in scenarios:
            if args.wasi:
                if case == 'queue_threads':
                    print('SKIP queue_threads: WASI 为单线程运行环境', flush=True)
                    continue
                # 沙箱不开放文件目录，仅提供参数和标准输出
                script = """const fs = require('node:fs');
const { WASI } = require('node:wasi');
const wasi = new WASI({version:'preview1', args:['test', process.argv[2]], returnOnExit:true});
const module_ = new WebAssembly.Module(fs.readFileSync(process.argv[1]));
const instance = new WebAssembly.Instance(module_, {wasi_snapshot_preview1:wasi.wasiImport});
process.exitCode = wasi.start(instance);
"""
                subprocess.run(['node', '--no-warnings', '-e', script, str(binary), case], check=True, timeout=30)
            else:
                subprocess.run([str(binary), case], check=True, timeout=30)


if __name__ == '__main__':
    main()
