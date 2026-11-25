"""合并 ESP32-C3 bootloader、分区表和应用为首次烧录镜像"""

from pathlib import Path
import re

Import("env")


def merge_factory_firmware(source, target, env):
    """按芯片烧录地址生成从 0x0 开始的完整 factory 镜像"""
    build_dir = Path(env.subst("$BUILD_DIR"))
    project_dir = Path(env.subst("$PROJECT_DIR"))
    framework_dir = Path(env.PioPlatform().get_package_dir("framework-arduinoespressif32"))
    bootloader = build_dir / "bootloader.bin"
    partitions = build_dir / "partitions.bin"
    application = build_dir / f"{env.subst('$PROGNAME')}.bin"
    if not bootloader.exists():
        candidates = list((framework_dir / "tools" / "sdk" / "esp32c3" / "bin").glob("bootloader_*.bin"))
        if not candidates:
            raise RuntimeError("找不到 ESP32-C3 bootloader.bin")
        bootloader = candidates[0]
    if not partitions.exists():
        candidates = list(build_dir.glob("partitions*.bin"))
        if not candidates:
            raise RuntimeError("找不到分区表 bin")
        partitions = candidates[0]
    if not application.exists():
        raise RuntimeError(f"找不到应用固件：{application}")

    image = bytearray(b"\xff" * 0x10000)
    for path, offset in ((bootloader, 0x0), (partitions, 0x8000), (application, 0x10000)):
        data = path.read_bytes()
        if offset + len(data) > len(image):
            image.extend(b"\xff" * (offset + len(data) - len(image)))
        image[offset:offset + len(data)] = data
    version_header = (project_dir / "src" / "version.h").read_text(encoding="utf-8")
    match = re.search(r'^\s*#define\s+FW_VERSION\s+"([^"]+)"', version_header, re.MULTILINE)
    if not match:
        raise RuntimeError("无法从 src/version.h 读取 FW_VERSION")
    (build_dir / f"{env.subst('$PIOENV')}-{match[1]}.factory.bin").write_bytes(image)


if env.subst("$PIOENV") == "esp32c3_air":
    env.AddPostAction("$BUILD_DIR/${PROGNAME}.bin", merge_factory_firmware)
