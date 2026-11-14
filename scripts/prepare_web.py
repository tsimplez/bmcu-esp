"""为 ESP32 生成 gzip 网页头文件，并阻止超过应用分区的固件被烧录"""

import gzip
from pathlib import Path

Import("env")


def prepare_web(env):
    """压缩网页到当前构建目录，内容不变时保留文件时间以支持增量编译"""
    source = Path(env.subst("$PROJECT_DIR")) / "web" / "index.html"
    output = Path(env.subst("$BUILD_DIR")) / "generated" / "web_page_gzip.h"
    compressed = gzip.compress(source.read_bytes(), compresslevel=9, mtime=0)
    rows = [", ".join(f"0x{byte:02x}" for byte in compressed[i:i + 16])
            for i in range(0, len(compressed), 16)]
    header = ("// 由 scripts/prepare_web.py 生成，请修改 web/index.html\n"
              "#pragma once\n#include <stdint.h>\n\n"
              "static const uint8_t PAGE_HTML_GZ[] = {\n    "
              + ",\n    ".join(rows) + "\n};\n")
    output.parent.mkdir(parents=True, exist_ok=True)
    if not output.exists() or output.read_text(encoding="utf-8") != header:
        output.write_text(header, encoding="utf-8", newline="\n")
    env.Append(CPPPATH=[str(output.parent)])


def check_firmware_size(source, target, env):
    """按完整 bin 大小检查应用分区容量，包含段对齐和镜像头开销"""
    firmware = Path(env.subst("$BUILD_DIR/${PROGNAME}.bin"))
    size = firmware.stat().st_size
    limit = int(env.BoardConfig().get("upload.maximum_size"))
    print(f"Firmware image: {size} / {limit} bytes")
    if size > limit:
        raise RuntimeError(f"Firmware image exceeds app partition by {size - limit} bytes")


if env.PioPlatform().name == "espressif32":
    prepare_web(env)
    env.AddPostAction("$BUILD_DIR/${PROGNAME}.bin", check_firmware_size)
    env.AddPreAction("upload", check_firmware_size)
