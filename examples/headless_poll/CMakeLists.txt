cmake_minimum_required(VERSION 3.16)
set(EXTRA_COMPONENT_DIRS "../../components/kst_protocol" "../../components/kst_servo" "../../components/kst_twai")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(kst_headless_poll)
