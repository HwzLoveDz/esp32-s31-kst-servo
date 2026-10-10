#!/usr/bin/env python3
"""Static artifact checks and deterministic provenance; no hardware I/O."""
import hashlib
import json
from pathlib import Path
import struct
import subprocess
import sys

fw_dir = Path(__file__).resolve().parent
build_dir = Path(sys.argv[1]).resolve()
tool_root = Path(sys.argv[2]).resolve()
base = "zt-canable-pro-f072c8-candlelight"
binary = build_dir / (base + ".bin")
elf = build_dir / (base + ".elf")
data = binary.read_bytes()
sp, reset = struct.unpack_from("<II", data)
assert 8 <= len(data) <= 65536, "Image exceeds F072C8 64 KiB flash"
assert sp == 0x20004000, "Initial stack must match F072C8 16 KiB RAM"
assert reset & 1 and 0x08000000 <= (reset & ~1) < 0x0800F800, "Invalid reset vector"
nm = subprocess.check_output([str(tool_root / "usr/bin/arm-none-eabi-nm"),
                              "-n", str(elf)], text=True)
symbols = {}
for line in nm.splitlines():
    fields = line.split()
    if len(fields) == 3:
        symbols[fields[2]] = int(fields[0], 16)
assert symbols["flash_data_rom"] == 0x0800F800, "User ID must occupy its own erase page"
assert symbols["_Heap_Begin"] < symbols["_Heap_Limit"] <= 0x20003C00
assert symbols["_Heap_Limit"] - symbols["_Heap_Begin"] >= 4096, "Insufficient startup heap"
source_hashes = {}
for directory in ("src", "include", "libs", "ldscripts", "cmake", "tests"):
    for path in sorted((fw_dir / directory).rglob("*")):
        if path.is_file():
            source_hashes[path.relative_to(fw_dir).as_posix()] = hashlib.sha256(path.read_bytes()).hexdigest()
for name in ("CMakeLists.txt", "build.sh", "verify_artifact.py", "LICENSE.md"):
    source_hashes[name] = hashlib.sha256((fw_dir / name).read_bytes()).hexdigest()
compiler = subprocess.check_output([str(tool_root / "usr/bin/arm-none-eabi-gcc"),
                                    "--version"], text=True).splitlines()[0]
size = subprocess.check_output([str(tool_root / "usr/bin/arm-none-eabi-size"), str(elf)], text=True)
result = {
    "target": "ZT-CANable Pro V1.0 / STM32F072C8T6",
    "hardware_validation_performed_by_this_script": False,
    "device_write_performed_by_this_script": False,
    "compiler": compiler,
    "vendor_source_archive_sha256": "4fa1fe37842590e72d1084d78febf612e36a464678fd1170ee7f9cbf2cb424a6",
    "vendor_original_binary_sha256": "33e9aa08294f590c31ef775162ec24ef7027eda233a469e6dee13d37c004f8b4",
    "binary": binary.name,
    "binary_bytes": len(data),
    "binary_sha256": hashlib.sha256(data).hexdigest(),
    "initial_stack_pointer": hex(sp),
    "reset_vector": hex(reset),
    "user_data_page": hex(symbols["flash_data_rom"]),
    "heap_available_bytes": symbols["_Heap_Limit"] - symbols["_Heap_Begin"],
    "usb_vid_pid": "1d50:606f",
    "usb_product": "ZT-CANable Pro F072C8 candleLight",
    "one_shot_advertised": True,
    "one_shot_implementation": "GS_CAN_MODE_ONE_SHOT -> CAN_MCR_NART; ABOM disabled in one-shot mode",
    "session_stop": "USB reset/deinit/suspend/MODE_RESET/START/DFU and bus-off abort CAN and purge queues",
    "mailbox_busy": "Drop without echo; no software deferred resend",
    "size_output": size.strip(),
    "source_sha256": source_hashes,
}
(build_dir / "build-manifest.json").write_text(json.dumps(result, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
print(f"STATIC CHECKS PASS: {len(data)} bytes, SHA256={result['binary_sha256']}")
print("Static checks only; this script does not write devices. See docs/VALIDATION.md for bench results.")
