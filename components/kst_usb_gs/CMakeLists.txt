idf_component_register(SRCS "kst_usb_gs.c"
    INCLUDE_DIRS "include"
    REQUIRES kst_servo gs_usb_protocol esp_timer usb)
