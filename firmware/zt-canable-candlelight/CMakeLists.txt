cmake_minimum_required(VERSION 3.16)
project(zt_canable_pro_f072c8 C ASM)

add_compile_options(
    -std=gnu11 -mcpu=cortex-m0 -mthumb -Os -g3
    -Wall -Wextra -Werror -Wno-deprecated
    -fmessage-length=0 -fsigned-char -ffunction-sections -fdata-sections
    -ffreestanding -fno-move-loop-invariants
    "-ffile-prefix-map=${CMAKE_SOURCE_DIR}=zt-canable-candlelight"
    "-fdebug-prefix-map=${CMAKE_SOURCE_DIR}=zt-canable-candlelight")
add_link_options(
    -mcpu=cortex-m0 -mthumb -nostartfiles --specs=nano.specs
    -Wl,--gc-sections "-T${CMAKE_SOURCE_DIR}/ldscripts/ldscript.ld")

# Keep vendor libraries intact, but build only the F072 dependencies used below.
add_subdirectory(libs/STM32_HAL EXCLUDE_FROM_ALL)
add_subdirectory(libs/STM32_USB_Device_Library EXCLUDE_FROM_ALL)

set(FW zt-canable-pro-f072c8-candlelight)
add_executable(${FW}.elf
    src/usbd_desc.c src/usbd_gs_can.c src/usbd_conf.c
    src/can.c src/dfu.c src/flash.c src/gpio.c src/led.c src/queue.c
    src/timer.c src/util.c src/main.c src/interrupts.c)
target_include_directories(${FW}.elf PRIVATE include)
target_compile_definitions(${FW}.elf PRIVATE BOARD=BOARD_candleLight)
target_link_libraries(${FW}.elf PRIVATE
    STM32_HAL_STM32F072xB STM32_USB_Device_Library_STM32F072xB)
target_link_options(${FW}.elf PRIVATE "-Wl,-Map=${CMAKE_BINARY_DIR}/${FW}.map")
add_custom_command(TARGET ${FW}.elf POST_BUILD
    COMMAND ${CMAKE_OBJCOPY} -O binary $<TARGET_FILE:${FW}.elf> ${FW}.bin
    COMMAND ${CMAKE_SIZE} $<TARGET_FILE:${FW}.elf>
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR} VERBATIM)
# Deliberately no programming/DFU targets: this project only builds artifacts.
