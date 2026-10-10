#!/bin/sh
# Build only. This script never enumerates, resets, or flashes hardware.
set -eu
: "${GNU_ARM_ROOT:?Set GNU_ARM_ROOT to the extracted Ubuntu ARM toolchain root}"
fw_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
fw_build=${1:-"$fw_dir/build"}
cmake -S "$fw_dir" -B "$fw_build" \
    -DCMAKE_TOOLCHAIN_FILE="$fw_dir/cmake/arm-none-eabi.cmake" \
    -DCMAKE_BUILD_TYPE=Release
cmake --build "$fw_build" --parallel 4
python3 "$fw_dir/verify_artifact.py" "$fw_build" "$GNU_ARM_ROOT"
