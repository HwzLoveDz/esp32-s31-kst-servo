cmake_minimum_required(VERSION 3.16)
project(kst_protocol_host_tests C)
enable_testing()
add_executable(test_kst_protocol
    test_kst_protocol.c
    ../components/kst_protocol/kst_protocol.c)
target_include_directories(test_kst_protocol PRIVATE ../components/kst_protocol/include)
target_compile_features(test_kst_protocol PRIVATE c_std_99)
if(MSVC)
    target_compile_options(test_kst_protocol PRIVATE /W4 /WX)
else()
    target_compile_options(test_kst_protocol PRIVATE -Wall -Wextra -Werror -pedantic)
endif()
add_test(NAME kst_protocol COMMAND test_kst_protocol)

add_executable(test_kst_servo
    test_kst_servo.c
    ../components/kst_servo/kst_servo.c
    ../components/kst_protocol/kst_protocol.c)
target_include_directories(test_kst_servo PRIVATE
    ../components/kst_protocol/include
    ../components/kst_servo/include)
target_compile_features(test_kst_servo PRIVATE c_std_99)
if(MSVC)
    target_compile_options(test_kst_servo PRIVATE /W4 /WX)
else()
    target_compile_options(test_kst_servo PRIVATE -Wall -Wextra -Werror -pedantic)
endif()
add_test(NAME kst_servo COMMAND test_kst_servo)

add_executable(test_app_controller
    test_app_controller.c
    ../components/kst_servo/kst_servo.c
    ../components/kst_protocol/kst_protocol.c)
target_include_directories(test_app_controller PRIVATE
    fakes
    ../components/kst_protocol/include
    ../components/kst_servo/include
    ../components/kst_twai/include
    ../components/kst_usb_gs/include)
target_compile_features(test_app_controller PRIVATE c_std_11)
if(MSVC)
    target_compile_options(test_app_controller PRIVATE /W4 /WX)
else()
    target_compile_options(test_app_controller PRIVATE -Wall -Wextra -Werror -pedantic)
endif()
add_test(NAME app_controller COMMAND test_app_controller)

add_executable(test_app_controller_usb
    test_app_controller.c
    ../components/kst_servo/kst_servo.c
    ../components/kst_protocol/kst_protocol.c)
target_compile_definitions(test_app_controller_usb PRIVATE CONFIG_KST_TRANSPORT_USB_GS=1)
target_include_directories(test_app_controller_usb PRIVATE fakes
    ../components/kst_protocol/include ../components/kst_servo/include ../components/kst_usb_gs/include)
target_compile_features(test_app_controller_usb PRIVATE c_std_11)
if(MSVC)
    target_compile_options(test_app_controller_usb PRIVATE /W4 /WX)
else()
    target_compile_options(test_app_controller_usb PRIVATE -Wall -Wextra -Werror -pedantic)
endif()
add_test(NAME app_controller_usb COMMAND test_app_controller_usb)

add_executable(test_gs_usb_protocol test_gs_usb_protocol.c
    ../components/gs_usb_protocol/gs_usb_protocol.c)
target_include_directories(test_gs_usb_protocol PRIVATE ../components/gs_usb_protocol/include)
target_compile_features(test_gs_usb_protocol PRIVATE c_std_99)
if(MSVC)
    target_compile_options(test_gs_usb_protocol PRIVATE /W4 /WX)
else()
    target_compile_options(test_gs_usb_protocol PRIVATE -Wall -Wextra -Werror -pedantic)
endif()
add_test(NAME gs_usb_protocol COMMAND test_gs_usb_protocol)

add_executable(test_kst_usb_gs test_kst_usb_gs.c
    ../components/gs_usb_protocol/gs_usb_protocol.c)
target_include_directories(test_kst_usb_gs PRIVATE fakes_usb
    ../components/kst_protocol/include ../components/kst_servo/include
    ../components/kst_usb_gs/include ../components/gs_usb_protocol/include)
target_compile_features(test_kst_usb_gs PRIVATE c_std_11)
if(MSVC)
    target_compile_options(test_kst_usb_gs PRIVATE /W4 /WX)
else()
    target_compile_options(test_kst_usb_gs PRIVATE -Wall -Wextra -Werror -pedantic)
endif()
add_test(NAME kst_usb_gs COMMAND test_kst_usb_gs)
