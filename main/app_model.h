/* SPDX-License-Identifier: MIT */
#pragma once
#include <stdbool.h>
#include <stdint.h>

#define APP_LOG_LINES 7
#define APP_LOG_WIDTH 100

typedef enum {
    APP_REFRESH, APP_READ_POSITION, APP_READ_STATUS, APP_READ_FEATURES,
    APP_READ_LIMITS, APP_READ_NODE, APP_READ_VERSION,
    APP_REPORT_START, APP_REPORT_STOP, APP_REPORT_INTERVAL,
    APP_ARM, APP_MOVE, APP_DEMO_START, APP_STOP_MOTION,
    APP_CENTER, APP_REVERSE, APP_UV_ALARM,
    APP_LIMIT_NEGATIVE, APP_LIMIT_POSITIVE,
    APP_SET_NODE, APP_SET_BAUD, APP_SAVE, APP_CONNECT,
    APP_RECOVER_BUS, APP_CLEAR_ALERTS
} app_action_t;

typedef struct {
    bool ready, online, position_valid, status_valid, features_valid;
    bool position_outside_limits; /* Valid readback vs legal target range; check age separately. */
    bool limit_probe_restore_unverified; /* Latched: permit readback/local reconnect only. */
    bool negative_limit_valid, positive_limit_valid, node_valid, version_valid;
    bool armed, demo_active, reporting_requested, config_dirty, requires_restart;
    bool low_voltage_latched, bus_off, preview, busy;
    bool usb_transport, adapter_connected;
    uint16_t usb_vid, usb_pid;
    uint8_t node_id, device_node_id, pending_node_id, feature_bits;
    uint16_t feature_raw; /* Full readback; unknown upper byte blocks flag writes. */
    uint32_t bitrate, pending_bitrate, report_interval_ms;
    uint32_t tx_count, rx_count, dropped_frames, timeouts, bus_errors;
    uint32_t position_age_ms, status_age_ms, abort_code;
    uint32_t telemetry_stale_ms;
    int16_t position_deci_deg, target_deci_deg, negative_limit, positive_limit;
    int16_t host_negative_limit, host_positive_limit;
    uint16_t current_10ma;
    int8_t temperature_c;
    char version[24];
    char transport_status[80];
    char result[100];
    char log[APP_LOG_LINES][APP_LOG_WIDTH];
} app_snapshot_t;

/* Nonblocking: UI only queues actions. A single worker owns CAN and the client. */
bool app_submit(app_action_t action, int32_t value, int32_t value2);
void app_get_snapshot(app_snapshot_t *out);
/* Call before display startup. No persistent state is erased or initialized. */
int app_controller_start(void);
