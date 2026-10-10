/* SPDX-License-Identifier: MIT */
#include "kst_twai.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static void feedback(void *user, const kst_event_t *e)
{
    (void)user;
    if (e->type == KST_EVENT_POSITION || e->type == KST_EVENT_POSITION_REPORT)
        ESP_LOGI("servo", "raw position = %d x 0.1 deg", e->position_deci_deg);
    else if (e->type == KST_EVENT_STATUS)
        ESP_LOGI("servo", "current = %u x 10 mA; temperature = %d C", e->current_10ma, e->temperature_c);
    else if (e->type == KST_EVENT_UNDERVOLTAGE)
        ESP_LOGW("servo", "UNDERVOLTAGE event (no voltage value)");
    else if (e->type == KST_EVENT_ABORT)
        ESP_LOGE("servo", "ABORT %08lx: %s", (unsigned long)e->abort_code, kst_abort_description(e->abort_code));
}

void app_main(void)
{
    kst_twai_bus_t *bus = NULL;
    ESP_ERROR_CHECK(kst_twai_open(46, 47, 500000, &bus));
    kst_transport_t transport = kst_twai_transport(bus);
    kst_servo_t client;
    if (kst_servo_init(&client, 0x25, &transport, 200, feedback, NULL) != KST_SERVO_OK) {
        ESP_ERROR_CHECK(kst_twai_close(bus));
        return;
    }
    /* This program is deliberately read-only. The full FUI shows explicitly
     * initiated writes, motion gates, reports, calibration and persistence. */
    kst_result_t r = kst_servo_read_features(&client, NULL);
    ESP_LOGI("servo", "Read features: %s", kst_servo_result_description(r));
    while (r >= 0) {
        r = kst_servo_read_position(&client, NULL);
        if (r >= 0) r = kst_servo_read_status(&client, NULL);
        if (r >= 0) vTaskDelay(pdMS_TO_TICKS(250));
    }
    ESP_LOGE("servo", "Stopped polling: %s; inspect bus before restarting", kst_servo_result_description(r));
    ESP_ERROR_CHECK(kst_twai_close(bus));
}
