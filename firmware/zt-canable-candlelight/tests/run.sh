#!/bin/sh
set -eu
task_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
mkdir -p "$task_dir/build-tests"
gcc -std=gnu11 -Wall -Wextra -Werror -Wno-int-to-pointer-cast \
 -Wno-pointer-to-int-cast -fsanitize=address,undefined -fno-omit-frame-pointer -g \
 -DSTM32F072xB -DBOARD=BOARD_candleLight \
 -I"$task_dir/include" \
 -I"$task_dir/libs/STM32_HAL/include" \
 -I"$task_dir/libs/STM32_HAL/include/cmsis" \
 -I"$task_dir/libs/STM32_HAL/include/cmsis/device" \
 -I"$task_dir/libs/STM32_HAL/include/stm32f0xx" \
 -I"$task_dir/libs/STM32_HAL/config" \
 -I"$task_dir/libs/STM32_USB_Device_Library/Core/Inc" \
 -I"$task_dir/libs/STM32_USB_Device_Library/config" \
 "$task_dir/tests/session_test.c" "$task_dir/src/queue.c" \
 -o "$task_dir/build-tests/session_test"
"$task_dir/build-tests/session_test"
