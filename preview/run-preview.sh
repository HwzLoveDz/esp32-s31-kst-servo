#!/usr/bin/env bash
set -euo pipefail
preview_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
: "${LVGL_SOURCE_DIR:?Set LVGL_SOURCE_DIR to the LVGL 9.5.0 source tree}"
build_dir="${KST_PREVIEW_BUILD_DIR:-$HOME/.cache/kst-fui-build}"
cmake -S "$preview_dir" -B "$build_dir" -DLVGL_SOURCE_DIR="$LVGL_SOURCE_DIR" -DCMAKE_BUILD_TYPE=Debug
cmake --build "$build_dir" --parallel "${KST_PREVIEW_JOBS:-8}"
"$build_dir/kst_fui_preview" "$preview_dir/screens"
python3 "$preview_dir/convert.py" "$preview_dir/screens"
