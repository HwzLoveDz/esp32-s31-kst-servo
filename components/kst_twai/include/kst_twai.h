/* SPDX-License-Identifier: MIT */
#pragma once
#include "esp_err.h"
#include "kst_servo.h"

typedef struct kst_twai_bus kst_twai_bus_t;
typedef struct {
    uint32_t tx_ok, rx_frames, rx_dropped, bus_errors;
    bool bus_off;
} kst_twai_stats_t;

/* One task owns the bus/client. IRQs only copy frames into a bounded queue.
 * Classic CAN extended data frames only. Uses external 3.3V transceiver.
 * A timed-out TX destroys the controller so an old position command cannot
 * be replayed later during bus recovery. Recovery is explicit via reopen(). */
esp_err_t kst_twai_open(int tx_gpio, int rx_gpio, uint32_t bitrate,
                        kst_twai_bus_t **out);
esp_err_t kst_twai_reopen(kst_twai_bus_t *bus, uint32_t bitrate);
esp_err_t kst_twai_close(kst_twai_bus_t *bus);
kst_transport_t kst_twai_transport(kst_twai_bus_t *bus);
void kst_twai_get_stats(kst_twai_bus_t *bus, kst_twai_stats_t *out);
