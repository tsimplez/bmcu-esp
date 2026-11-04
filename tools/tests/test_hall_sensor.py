"""编译真实霍尔采样模块和历史接口，使用可控 ADC 与时钟验证一分钟统计。"""

import argparse
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

from test_bambu_bus import extract_function, strip_includes


def main():
    """组合生产代码与硬件桩，运行采样、缓存、配置和时间边界场景。"""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cxx', default=os.environ.get('CXX', 'g++'))
    args = parser.parse_args()
    compiler = shutil.which(args.cxx) or args.cxx
    test_env = dict(os.environ)
    test_env['PATH'] = str(Path(compiler).resolve().parent) + os.pathsep + test_env.get('PATH', '')
    root = Path(__file__).resolve().parents[2]

    def read(path):
        """读取 UTF-8 生产源码或测试夹具。"""
        return (root / path).read_text(encoding='utf-8')

    def header(path):
        """保留真实数据布局与声明，仅移除嵌入式 include。"""
        return strip_includes(read(path)).replace('#pragma once', '')

    arduino = read('tools/tests/fixtures/ota_test_support.h').split('struct Response')[0]
    web = read('src/web_config.cpp')
    unit = '\n'.join((
        arduino,
        header('src/io_config.h'),
        header('src/hall_sensor.h'),
        read('tools/tests/fixtures/hall_test_support.h'),
        strip_includes(read('src/hall_sensor.cpp')),
        extract_function(web, 'static bool parse_hall_cursor('),
        extract_function(web, 'static void append_hall_value('),
        extract_function(web, 'static void handle_hall()'),
        read('tools/tests/fixtures/hall_test_cases.cpp'),
    ))
    with tempfile.TemporaryDirectory(prefix='bmcu-hall-test-') as directory:
        source = Path(directory) / 'test.cpp'
        binary = Path(directory) / ('test.exe' if os.name == 'nt' else 'test')
        source.write_text(unit, encoding='utf-8')
        subprocess.run([compiler, '-std=c++17', '-O0', '-g', str(source), '-o', str(binary)], check=True)
        for scenario in ('sampling', 'capacity', 'gaps', 'configuration', 'wrap', 'ownership', 'cursor_wrap', 'limits'):
            subprocess.run([str(binary), scenario], check=True, timeout=30, env=test_env)
        response = subprocess.run([str(binary), 'http'], check=True, capture_output=True,
                                  text=True, encoding='utf-8', timeout=30, env=test_env)
        packets = {}
        for line in response.stdout.splitlines():
            if not line.startswith('HTTP\t'):
                continue
            _, name, status, body = line.split('\t', 3)
            packets[name] = (int(status), json.loads(body))
        check_http_packets(packets)
        print('PASS hall_http: pagination, resets, malformed cursors, gaps, read-only cache', flush=True)


def check_http_packets(packets):
    """独立检查真实 HTTP 输出的 JSON 类型、分页位置、缺失值与游标边界。"""
    status, empty = packets['empty']
    assert status == 200 and empty['reset'] is True and empty['more'] is False
    assert empty['cursor'] == 0 and empty['interval_ms'] == 200 and empty['now_ms'] == 0
    assert len(empty['channels']) == 4
    assert empty['channels'][0]['raw'] is None and empty['channels'][0]['points'] == []
    assert empty['channels'][2]['pin'] == 255 and empty['channels'][2]['avg'] is None
    for page in range(1, 7):
        status, packet = packets[f'page{page}']
        assert status == 200 and packet['cursor'] == page * 50
        assert packet['reset'] is (page == 1) and packet['more'] is (page < 6)
        assert packet['epoch'] == empty['epoch'] and packet['now_ms'] == 60000
        for channel, value in ((0, 1000), (1, 1100)):
            entry = packet['channels'][channel]
            assert entry['raw'] == value and entry['min'] == value and entry['max'] == value
            assert entry['avg'] == value and len(entry['points']) == 50
            assert entry['points'][0] == [60000 - ((page - 1) * 50 + 1) * 200, value, value, value]
            assert entry['points'][-1] == [60000 - page * 10000, value, value, value]
        assert packet['channels'][2]['points'] == []
    status, increment = packets['increment']
    assert status == 200 and increment['cursor'] == 305 and increment['reset'] is False
    assert increment['more'] is False
    assert [point[0] for point in increment['channels'][0]['points']] == [800, 600, 400, 200, 0]
    assert packets['unchanged'][1]['channels'][0]['points'] == []
    for name in ('old_epoch', 'future', 'too_old'):
        status, packet = packets[name]
        assert status == 200 and packet['reset'] is True and packet['cursor'] == 55
    assert packets['oldest_edge'][1]['reset'] is False
    assert packets['oldest_edge'][1]['cursor'] == 55
    for name, (status, packet) in packets.items():
        if name.startswith('bad_'):
            assert status == 400 and packet['ok'] is False, name
    status, failed = packets['failed']
    assert status == 200 and failed['channels'][0]['raw'] is None
    assert any(point[1:] == [None, None, None] for point in failed['channels'][0]['points'])
    status, changed = packets['changed']
    assert status == 200 and changed['reset'] is True and changed['cursor'] == 0
    assert changed['epoch'] != empty['epoch'] and changed['channels'][0]['pin'] == 2
    assert changed['channels'][0]['raw'] is None and changed['channels'][0]['points'] == []


if __name__ == '__main__':
    main()
