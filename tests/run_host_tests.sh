#!/bin/sh
# Synchronous native Linux test. Never invokes Windows executables from WSL.
set -eu
task_test_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
task_build_dir="$task_test_dir/build-host-linux"
mkdir -p "$task_build_dir"
"${CC:-cc}" -std=c99 -Wall -Wextra -Werror -pedantic \
    -I"$task_test_dir/../components/kst_protocol/include" \
    "$task_test_dir/test_kst_protocol.c" \
    "$task_test_dir/../components/kst_protocol/kst_protocol.c" \
    -o "$task_build_dir/test_kst_protocol"
"$task_build_dir/test_kst_protocol"
"${CC:-cc}" -std=c99 -Wall -Wextra -Werror -pedantic \
    -I"$task_test_dir/../components/kst_protocol/include" \
    -I"$task_test_dir/../components/kst_servo/include" \
    "$task_test_dir/test_kst_servo.c" \
    "$task_test_dir/../components/kst_servo/kst_servo.c" \
    "$task_test_dir/../components/kst_protocol/kst_protocol.c" \
    -o "$task_build_dir/test_kst_servo"
"$task_build_dir/test_kst_servo"
"${CC:-cc}" -std=c11 -Wall -Wextra -Werror -pedantic \
    -I"$task_test_dir/fakes" \
    -I"$task_test_dir/../components/kst_protocol/include" \
    -I"$task_test_dir/../components/kst_servo/include" \
    -I"$task_test_dir/../components/kst_twai/include" \
    "$task_test_dir/test_app_controller.c" \
    "$task_test_dir/../components/kst_servo/kst_servo.c" \
    "$task_test_dir/../components/kst_protocol/kst_protocol.c" \
    -o "$task_build_dir/test_app_controller"
"$task_build_dir/test_app_controller"
"${CC:-cc}" -std=c11 -Wall -Wextra -Werror -pedantic \
    -DCONFIG_KST_TRANSPORT_USB_GS=1 \
    -I"$task_test_dir/fakes" \
    -I"$task_test_dir/../components/kst_protocol/include" \
    -I"$task_test_dir/../components/kst_servo/include" \
    -I"$task_test_dir/../components/kst_usb_gs/include" \
    "$task_test_dir/test_app_controller.c" \
    "$task_test_dir/../components/kst_servo/kst_servo.c" \
    "$task_test_dir/../components/kst_protocol/kst_protocol.c" \
    -o "$task_build_dir/test_app_controller_usb"
"$task_build_dir/test_app_controller_usb"
"${CC:-cc}" -std=c99 -Wall -Wextra -Werror -pedantic \
    -I"$task_test_dir/../components/gs_usb_protocol/include" \
    "$task_test_dir/test_gs_usb_protocol.c" \
    "$task_test_dir/../components/gs_usb_protocol/gs_usb_protocol.c" \
    -o "$task_build_dir/test_gs_usb_protocol"
"$task_build_dir/test_gs_usb_protocol"
"${CC:-cc}" -std=c11 -Wall -Wextra -Werror -pedantic \
    -I"$task_test_dir/fakes_usb" \
    -I"$task_test_dir/../components/kst_protocol/include" \
    -I"$task_test_dir/../components/kst_servo/include" \
    -I"$task_test_dir/../components/kst_usb_gs/include" \
    -I"$task_test_dir/../components/gs_usb_protocol/include" \
    "$task_test_dir/test_kst_usb_gs.c" \
    "$task_test_dir/../components/gs_usb_protocol/gs_usb_protocol.c" \
    -o "$task_build_dir/test_kst_usb_gs"
"$task_build_dir/test_kst_usb_gs"
