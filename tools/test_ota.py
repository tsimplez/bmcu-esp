"""编译真实 OTA 代码进行主机回归；只替换网络、Flash 和 Arduino 接口。"""
import argparse
from pathlib import Path
import subprocess
import tempfile


def main():
    """提取生产 OTA 函数，用 Zig/WASI 编译后执行，不访问设备或改写 Flash。"""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--zig', required=True, type=Path, help='Zig 0.15.2 或更新版本的可执行文件')
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    source = (root / 'src/web_config.cpp').read_text(encoding='utf-8')
    manifest = source[source.index('static String g_ota_url;'):source.index('/**\n * 生成 OTA 错误 JSON')]
    download = source[source.index('static bool ota_flash_attempt('):source.index('/**\n * 处理在线升级请求')]
    unit = '\n'.join(((root / 'tools/ota_test_support.h').read_text(encoding='utf-8'),
                      manifest, download, (root / 'tools/ota_test_cases.cpp').read_text(encoding='utf-8')))
    with tempfile.TemporaryDirectory(prefix='bmcu-ota-test-') as directory:
        folder = Path(directory)
        cpp, wasm = folder / 'test.cpp', folder / 'test.wasm'
        cpp.write_text(unit, encoding='utf-8')
        subprocess.run([str(args.zig), 'c++', '-target', 'wasm32-wasi', '-std=c++17',
                        '-fno-exceptions', str(cpp), '-o', str(wasm)], check=True)
        script = """const fs=require('node:fs');const {WASI}=require('node:wasi');
const wasi=new WASI({version:'preview1',args:['test'],returnOnExit:true});
const m=new WebAssembly.Module(fs.readFileSync(process.argv[1]));
process.exitCode=wasi.start(new WebAssembly.Instance(m,{wasi_snapshot_preview1:wasi.wasiImport}));"""
        subprocess.run(['node', '--no-warnings', '-e', script, str(wasm)], check=True, timeout=30)


if __name__ == '__main__':
    main()
