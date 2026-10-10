/* SPDX-License-Identifier: MIT */
#include "app_model.h"
#include "esp_check.h"
#include "esp_log.h"

esp_err_t display_port_start(void);

void app_main(void)
{
    /* No NVS init/erase, no camera, no Wi-Fi, no startup motion or parameter save. */
    ESP_LOGI("kst_fui", "KST V3.61B / classic extended CAN / ESP32-S31 Korvo");
    ESP_ERROR_CHECK(app_controller_start());
    ESP_ERROR_CHECK(display_port_start());
}
