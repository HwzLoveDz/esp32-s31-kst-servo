/* SPDX-License-Identifier: MIT */
#pragma once
#include "esp_err.h"
#include "kst_servo.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct kst_usb_gs_bus kst_usb_gs_bus_t;
/* Called in the owner task immediately before USB OUT submission, after any
 * wait for the preceding echo. rx_pending means fresh CAN data is queued.
 * Return false to veto this frame without faulting a healthy transport.
 * Must be bounded/nonblocking; do not recursively send/receive/service. */
typedef bool (*kst_usb_gs_tx_guard_t)(void *ctx, const kst_can_frame_t *frame,
                                     bool rx_pending);
typedef struct {
    /* tx_ok counts USB-accepted frames, NOT successful CAN bus transmissions.
     * candleLight echo is also not CAN ACK or servo execution confirmation. */
    uint32_t tx_ok, tx_echoes, rx_frames, rx_dropped, bus_errors;
    /* Diagnostic RX observations include standard/RTR frames before the KST
     * filter. TX echoes never count as observed bus replies. Last observations
     * survive a fault; generation identifies the session in which they arrived.
     * CAN flags use 0x80000000=extended, 0x40000000=RTR; usb_flags bit0=overflow.
     * last_error_id contains CAN error-class bits, excluding CAN_ERR_FLAG. */
    uint32_t rx_standard;
    uint32_t last_can_id, last_can_flags, last_can_generation;
    uint32_t last_error_id, last_error_generation;
    uint8_t last_can_dlc, last_can_usb_flags, last_can_data[8], last_error_data[8];
    bool last_can_valid, last_error_valid;
    uint32_t generation, features, bitrate, usb_errors;
    uint16_t vid, pid;
    bool bus_off, connected, ready, requires_close;
    char status[80];
} kst_usb_gs_stats_t;

/* All APIs, including stats, belong to one caller task. The component owns
 * the USB Host library and creates one library-event daemon. It cannot share
 * a preinstalled USB Host library. open succeeds without a connected adapter.
 * Only allowlisted gs_usb IDs and a compatible classic-CAN descriptor qualify.
 * Startup requires advertised ONE_SHOT capability. */
esp_err_t kst_usb_gs_open(uint32_t bitrate, kst_usb_gs_bus_t **out);
/* Advances asynchronous enumeration/configuration/cancellation. budget_ms=0
 * never waits for events. Call every worker iteration, including while !ready.
 * The budget bounds event waits, not SDK locks in port/device management.
 * Returns ESP_OK while waiting/initializing; faults are reported in stats. */
esp_err_t kst_usb_gs_service(kst_usb_gs_bus_t *bus, uint32_t budget_ms);
/* Explicitly clears a latched fault and resets the adapter before starting.
 * Asynchronous: check stats.ready; old queued CAN frames are never resubmitted. */
esp_err_t kst_usb_gs_reopen(kst_usb_gs_bus_t *bus, uint32_t bitrate);
esp_err_t kst_usb_gs_set_tx_guard(kst_usb_gs_bus_t *bus,
                                 kst_usb_gs_tx_guard_t callback, void *ctx);
/* Bounded cleanup; on timeout bus storage remains valid for close retry.
 * Never frees a transfer still owned by USB Host. */
esp_err_t kst_usb_gs_close(kst_usb_gs_bus_t *bus);
kst_transport_t kst_usb_gs_transport(kst_usb_gs_bus_t *bus);
void kst_usb_gs_get_stats(kst_usb_gs_bus_t *bus, kst_usb_gs_stats_t *out);

#ifdef __cplusplus
}
#endif
