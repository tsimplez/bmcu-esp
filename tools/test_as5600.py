"""验证真实 AS5600 模拟采样、时间边界和重置行为；只替换时钟与 Arduino 接口。"""
import argparse
from pathlib import Path
import subprocess
import tempfile


def main():
    """提取生产模拟代码并使用 Zig/WASI 运行，验证数值范围、统计特征和可复现性。"""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--zig', required=True, type=Path, help='Zig 0.15.2 或更新版本')
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    source = (root / 'src/BMCU.cpp').read_text(encoding='utf-8')
    simulation = source[source.index('// 随机数生成器（用于模拟传感器数据）'):source.index('/**\n * 获取模拟的AS5600原始角度值')]
    support = r'''
#include <random>
#include <cmath>
#include <cstdint>
#include <cassert>
#include <cstdlib>
#include <cstdio>
#define LOG_MAIN(...) do {} while (0)
static uint32_t test_ms=0;
/** 返回受控毫秒时钟 */
static uint32_t millis() { return test_ms; }
/** 模拟 Arduino 角度随机种子，不替换距离采样器 */
static void randomSeed(unsigned seed) { srand(seed); }
/** 模拟初始化时的随机角度 */
static long random(long low, long high) { return low+rand()%(high-low); }
'''
    cases = r'''
/** 验证距离不随调用频率增大、时间回绕、固定种子和正态采样统计 */
int main() {
    AS5600_init();
    assert(AS5600_get_distance_E()==0.0f);
    float first[65];
    for (auto &distance:first) {
        test_ms+=10;distance=AS5600_get_distance_E();
        assert(std::isfinite(distance) && distance>=0.003f && distance<=0.008f);
        assert(AS5600_get_distance_E()==0.0f);
    }
    test_ms=0;AS5600_init();
    for (float distance:first) { test_ms+=10;assert(AS5600_get_distance_E()==distance); }
    test_ms=0;AS5600_init();test_ms=100;float clamped=AS5600_get_distance_E();
    test_ms=0;AS5600_init();test_ms=10000;assert(AS5600_get_distance_E()==clamped);
    test_ms=UINT32_MAX-5;AS5600_init();test_ms+=10;assert(AS5600_get_distance_E()==first[0]);
    test_ms=0;AS5600_init();
    double sum=0, squares=0, distance_sum=0;
    constexpr int count=100000;
    for (int i=0;i<count;i++) {
        float value=as5600_distance_jitter();assert(std::isfinite(value));
        sum+=value;squares+=double(value)*value;
        test_ms+=10;float distance=AS5600_get_distance_E();
        assert(distance>=0.003f && distance<=0.008f);distance_sum+=distance;
    }
    assert(std::abs(sum/count)<0.00001);
    assert(std::abs(squares/count-0.00083*0.00083)<0.05*0.00083*0.00083);
    assert(std::abs(distance_sum/count-0.0055)<0.00002);
    std::puts("PASS: AS5600 reset, same tick, time clamp/wrap, 100000 finite samples, mean/variance and distance bounds");
}
'''
    with tempfile.TemporaryDirectory(prefix='bmcu-as5600-test-') as directory:
        cpp, wasm = Path(directory)/'test.cpp', Path(directory)/'test.wasm'
        cpp.write_text(support+simulation+cases, encoding='utf-8')
        subprocess.run([str(args.zig), 'c++', '-target', 'wasm32-wasi', '-std=c++17',
                        '-fno-exceptions', str(cpp), '-o', str(wasm)], check=True)
        script = """const fs=require('node:fs');const {WASI}=require('node:wasi');
const wasi=new WASI({version:'preview1',args:['test'],returnOnExit:true});
const m=new WebAssembly.Module(fs.readFileSync(process.argv[1]));
process.exitCode=wasi.start(new WebAssembly.Instance(m,{wasi_snapshot_preview1:wasi.wasiImport}));"""
        subprocess.run(['node','--no-warnings','-e',script,str(wasm)],check=True,timeout=30)


if __name__ == '__main__':
    main()
