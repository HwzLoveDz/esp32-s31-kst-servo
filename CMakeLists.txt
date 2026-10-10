cmake_minimum_required(VERSION 3.16)
set(SDKCONFIG_DEFAULTS "sdkconfig.defaults;sdkconfig.defaults.esp32s31;components/gen_bmgr_codes/board_manager.defaults")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(kst_servo_fui)
