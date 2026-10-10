idf_component_register(SRCS "main.c" "app_controller.c" "display_port.c" "fui.c"
    INCLUDE_DIRS "." REQUIRES kst_servo kst_twai kst_usb_gs esp_board_manager esp_lvgl_port lvgl
    PRIV_REQUIRES esp_timer)
