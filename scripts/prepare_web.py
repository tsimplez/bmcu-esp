"""为 ESP32 打包网页与 PWA 资源，并阻止超过应用分区的固件被烧录"""

import gzip
import hashlib
from pathlib import Path

Import("env")


def byte_array(name, data):
    """把资源字节编码为只读 C++ 数组，供固件直接从 Flash 发送"""
    rows = [", ".join(f"0x{byte:02x}" for byte in data[i:i + 16])
            for i in range(0, len(data), 16)]
    return (f"static const uint8_t {name}[] = {{\n    "
            + ",\n    ".join(rows) + "\n};\n")


def prepare_web(env):
    """打包网页和静态资源并注入缓存版本，内容不变时保留时间以支持增量编译"""
    root = Path(env.subst("$PROJECT_DIR")) / "web"
    output = Path(env.subst("$BUILD_DIR")) / "generated" / "web_page_gzip.h"
    assets = [
        ("manifest.webmanifest", "PWA_MANIFEST_GZ", "application/manifest+json; charset=utf-8", True),
        ("sw.js", "PWA_SW_GZ", "application/javascript; charset=utf-8", True),
        ("offline.html", "PWA_OFFLINE_GZ", "text/html; charset=utf-8", True),
        ("icons/icon-192.png", "PWA_ICON_192", "image/png", False),
        ("icons/icon-512.png", "PWA_ICON_512", "image/png", False),
        ("icons/apple-touch-icon.png", "PWA_APPLE_ICON", "image/png", False),
    ]
    sources = {path: (root / path).read_bytes()
               for path in ["index.html"] + [asset[0] for asset in assets]}
    digest = hashlib.sha256()
    for path, data in sources.items():
        digest.update(path.encode("utf-8") + b"\0" + data + b"\0")
    sources["sw.js"] = sources["sw.js"].replace(
        b"__BMCU_PWA_VERSION__", digest.hexdigest()[:16].encode("ascii"))
    header = ("// 由 scripts/prepare_web.py 生成，请修改 web/ 下的源文件\n"
              "#pragma once\n#include <stdint.h>\n#include <stddef.h>\n\n")
    header += byte_array("PAGE_HTML_GZ", gzip.compress(sources["index.html"], compresslevel=9, mtime=0))
    for path, name, mime, compressed in assets:
        data = gzip.compress(sources[path], compresslevel=9, mtime=0) if compressed else sources[path]
        header += "\n" + byte_array(name, data)
    header += ("\nstruct WebAsset {\n"
               "    const char *path;\n    const char *mime;\n"
               "    const uint8_t *data;\n    size_t length;\n    bool gzip;\n};\n\n"
               "static const WebAsset WEB_ASSETS[] = {\n")
    for path, name, mime, compressed in assets:
        header += (f'    {{"/{path}", "{mime}", {name}, sizeof({name}), '
                   + ("true" if compressed else "false") + "},\n")
    header += "};\n"
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
