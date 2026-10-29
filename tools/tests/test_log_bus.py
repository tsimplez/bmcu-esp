"""编译真实日志环形缓冲和 HTTP handler，检查合并响应与游标边界。"""
import argparse
from pathlib import Path
import subprocess
import tempfile

from test_bambu_bus import extract_function, strip_includes


def main():
    """在临时目录用 Zig/WASI 运行真实日志代码，不连接或修改设备。"""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--zig', type=Path, required=True, help='Zig 0.15.2 或更新版本的可执行文件')
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[2]
    source = (root / 'src/log_bus.cpp').read_text(encoding='utf-8')
    ring = source[source.index('namespace {'):source.index('// 文本输出')]
    header = strip_includes((root / 'src/log_bus.h').read_text(encoding='utf-8')).replace('#pragma once', '')
    web = (root / 'src/web_config.cpp').read_text(encoding='utf-8')
    support = (root / 'tools/tests/fixtures/log_test_support.h').read_text(encoding='utf-8')
    cases = (root / 'tools/tests/fixtures/log_test_cases.cpp').read_text(encoding='utf-8')
    unit = '\n'.join((support, header, ring, extract_function(web, 'static void handle_log()'), cases))
    with tempfile.TemporaryDirectory(prefix='bmcu-log-test-') as directory:
        cpp, wasm = Path(directory) / 'test.cpp', Path(directory) / 'test.wasm'
        cpp.write_text(unit, encoding='utf-8')
        subprocess.run([str(args.zig), 'c++', '-target', 'wasm32-wasi', '-std=c++17', '-fno-exceptions',
                        str(cpp), '-o', str(wasm)], check=True)
        runner = """const fs=require('node:fs');const {WASI}=require('node:wasi');
const wasi=new WASI({version:'preview1',args:['test'],returnOnExit:true});
const module_=new WebAssembly.Module(fs.readFileSync(process.argv[1]));
process.exitCode=wasi.start(new WebAssembly.Instance(module_,{wasi_snapshot_preview1:wasi.wasiImport}));"""
        subprocess.run(['node', '--no-warnings', '-e', runner, str(wasm)], check=True, timeout=30)


if __name__ == '__main__':
    main()
