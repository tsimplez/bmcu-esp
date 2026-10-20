#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""
把 PlatformIO 编译出来的固件打包成 OTA 发布目录

用法（在项目根目录下）：
    python tools/make_ota.py esp32s3 esp32c3 airm2m_core_esp32c3
    python tools/make_ota.py esp32s3 --notes "修复 RS485 偶发丢包" --out dist

做完以后把 dist 目录里的东西整个传到你的服务器（web 根目录或子目录都行），
在设备网页「参数 → 固件更新 → 更新源目录」里填这个目录的 URL

清单 manifest.json 是跨版本累加的：这次只发布 esp32s3，之前发过的
esp32c3 条目会原样保留，设备各自按自己的板子名找对应固件
"""

import argparse
import datetime
import hashlib
import json
import os
import re
import shutil
import sys

# 项目根目录（本脚本位于 tools/ 下）
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
# 固件版本头文件路径，打包时从其中读取 FW_VERSION
VERSION_H = os.path.join(ROOT, 'src', 'version.h')

# 环境名就是要写进清单的键名，和 version.h 里的 FW_TARGET 一一对应
KNOWN_TARGETS = ['esp32s3', 'esp32c3', 'airm2m_core_esp32c3', 'pico']


def read_fw_version():
    """读取当前固件版本号

    返回：
        src/version.h 中 FW_VERSION 宏对应的字符串
    从版本头文件中提取版本号；缺失时终止打包并提示修改位置
    """
    with open(VERSION_H, 'r', encoding='utf-8') as f:
        text = f.read()
    m = re.search(r'#define\s+FW_VERSION\s+"([^"]+)"', text)
    if not m:
        sys.exit('读不到 src/version.h 里的 FW_VERSION')
    return m.group(1)


def md5_of(path):
    """计算固件文件的 MD5 摘要

    参数：
        path: 待计算的固件文件路径
    返回：
        32 位小写十六进制 MD5 字符串
    分块读取文件，避免一次性把大固件全部载入内存
    """
    h = hashlib.md5()
    with open(path, 'rb') as f:
        for chunk in iter(lambda: f.read(1 << 16), b''):
            h.update(chunk)
    return h.hexdigest()


def validate_version(version):
    """校验 OTA 版本号格式和日期有效性

    参数：
        version: 形如 YY.MM.DD 或 YY.MM.DD.修订号 的版本字符串
    版本不合法时打印错误并终止打包，防止生成设备无法比较的版本
    """
    if not re.fullmatch(r'\d{2}\.\d{2}\.\d{2}(?:\.\d{1,3})?', version):
        sys.exit('版本号须为 YY.MM.DD 或 YY.MM.DD.修订号，例如 26.12.22.123')
    yy, mm, dd = map(int, version.split('.')[:3])
    try:
        datetime.date(2000 + yy, mm, dd)
    except ValueError:
        sys.exit('版本号中的日期无效：%s' % version)


def main():
    """生成 OTA 发布目录和跨板型累加的 manifest.json

    校验版本与编译产物，复制各环境 firmware.bin，并写入版本、大小和
    MD5；已有清单中的其他板型条目会保留
    """
    ap = argparse.ArgumentParser(description='生成 OTA 发布目录（固件 + manifest.json）')
    ap.add_argument('targets', nargs='+', help='PlatformIO 环境名，同时也是清单里的板子键名')
    ap.add_argument('--version', '-v', help='版本号，默认取 src/version.h 的 FW_VERSION')
    ap.add_argument('--notes', '-n', default='', help='更新说明（显示在设备网页上）')
    ap.add_argument('--out', '-o', default='dist', help='输出目录，默认 dist')
    args = ap.parse_args()

    for t in args.targets:
        if t not in KNOWN_TARGETS:
            print('警告：%s 不在已知板子列表里，确认 device 端 FW_TARGET 就是这个字符串' % t)

    firmware_version = read_fw_version()
    version = args.version or firmware_version
    validate_version(version)
    if version != firmware_version:
        sys.exit('--version 必须与 src/version.h 的 FW_VERSION 一致；请先修改版本并重新编译')
    out_dir = args.out if os.path.isabs(args.out) else os.path.join(ROOT, args.out)
    os.makedirs(out_dir, exist_ok=True)

    manifest_path = os.path.join(out_dir, 'manifest.json')
    manifest = {}
    if os.path.exists(manifest_path):
        try:
            with open(manifest_path, 'r', encoding='utf-8') as f:
                manifest = json.load(f)
        except ValueError:
            sys.exit('%s 不是合法 JSON，先挪走再重跑' % manifest_path)
    legacy = [key for key, value in manifest.items()
              if key in KNOWN_TARGETS and key not in args.targets
              and isinstance(value, dict) and 'version' not in value]
    if legacy:
        sys.exit('旧清单的板型条目缺少独立 version，请先一起重新发布：%s' % ', '.join(legacy))
    manifest.pop('version', None)
    manifest.pop('notes', None)
    # 清掉旧版脚本写入的冗余字段（设备端只读取下面 entry 中的 5 个字段）
    obsolete = {'sha256', 'source_commit', 'source_dirty', 'capabilities'}
    for entry in manifest.values():
        if isinstance(entry, dict):
            for key in obsolete:
                entry.pop(key, None)

    published = []
    for env in args.targets:
        src = os.path.join(ROOT, '.pio', 'build', env, 'firmware.bin')
        if not os.path.exists(src):
            sys.exit('找不到 %s，先跑一次： pio run -e %s' % (src, env))
        if os.path.getmtime(src) < os.path.getmtime(VERSION_H):
            sys.exit('%s 早于 src/version.h；请重新编译 %s' % (src, env))
        with open(src, 'rb') as f:
            if version.encode('ascii') not in f.read():
                sys.exit('%s 不包含版本号 %s；请重新编译 %s' % (src, version, env))

        name = 'firmware-%s-%s.bin' % (env, version)
        dst = os.path.join(out_dir, name)
        shutil.copyfile(src, dst)

        size = os.path.getsize(dst)
        entry = {'version': version, 'notes': args.notes,
                 'url': name, 'size': size, 'md5': md5_of(dst)}
        manifest[env] = entry
        published.append((env, name, size))

    with open(manifest_path, 'w', encoding='utf-8') as f:
        json.dump(manifest, f, ensure_ascii=False, indent=2)
        f.write('\n')

    print('版本：%s' % version)
    for env, name, size in published:
        print('  %-22s %-34s %7.1f KB' % (env, name, size / 1024.0))
    print('输出目录：%s' % out_dir)
    print('清单里的板子：%s' % ', '.join(k for k in manifest if k not in ('version', 'notes')))
    print('\n把这个目录整个上传到服务器，然后在设备网页里填它的 URL')


if __name__ == '__main__':
    main()
