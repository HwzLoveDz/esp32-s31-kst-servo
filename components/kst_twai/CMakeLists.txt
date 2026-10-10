idf_component_register(SRCS "kst_twai.c" INCLUDE_DIRS "include"
    REQUIRES kst_servo esp_driver_twai esp_timer freertos)
