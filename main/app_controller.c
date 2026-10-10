/* SPDX-License-Identifier: MIT
 * The CAN worker is the ONLY owner of the client and mutable model. LVGL gets
 * an atomic snapshot and submits bounded messages; callbacks never touch LVGL.
 */
#include "app_model.h"
#include "sdkconfig.h"
#if CONFIG_KST_TRANSPORT_USB_GS
#include "kst_usb_gs.h"
#else
#include "kst_twai.h"
#endif
#include "esp_timer.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <limits.h>

typedef struct { app_action_t action; int32_t a, b; unsigned epoch, connection_epoch; } request_t;
static QueueHandle_t requests;
static portMUX_TYPE model_lock = portMUX_INITIALIZER_UNLOCKED;
static app_snapshot_t model, published;
static kst_servo_t servo;
#if CONFIG_KST_TRANSPORT_USB_GS
static kst_usb_gs_bus_t *bus;
static uint32_t link_generation;
static bool link_seen, link_needs_refresh;
#if CONFIG_KST_DISCOVER_ON_START
static bool startup_discovery_done, discovery_running;
static uint32_t discovery_generation;
static unsigned discovery_connection_epoch;
static bool discovery_queue_cutoff_valid;
static unsigned discovery_queue_cutoff;
#endif
#if CONFIG_KST_PROBE_360_ON_START
static bool limit_probe_done, limit_probe_running, limit_probe_restoring, limit_probe_queue_cutoff_valid;
static atomic_bool limit_probe_active;
static uint32_t limit_probe_generation;
static unsigned limit_probe_connection_epoch, limit_probe_queue_cutoff;
#endif
#else
static kst_twai_bus_t *bus;
#endif
static kst_transport_t native_transport;
static unsigned target_epoch;
static bool target_denied;
static atomic_uint motion_epoch;
static atomic_uint connection_epoch;
static unsigned send_connection_epoch;
static atomic_bool cancel_requested;
static uint64_t position_time, status_time, last_event_time;
static int16_t raw_position, sweep_start_position, active_negative = -1000, active_positive = 1000;
static bool raw_position_valid;
enum { METADATA_NEGATIVE, METADATA_POSITIVE, METADATA_NODE, METADATA_VERSION, METADATA_COUNT };
#define METADATA_RECOVERY_ATTEMPTS 3U
#define METADATA_RECOVERY_INTERVAL_MS 1000U
static struct {
    bool pending;
    unsigned cursor, attempts[METADATA_COUNT];
    uint64_t next_ms;
} metadata_recovery;
static uint64_t sweep_start, next_step;
static int sweep_amplitude, sweep_duration;
static const char *TAG = "kst_app";

static uint64_t now_ms(void) { return (uint64_t)esp_timer_get_time() / 1000; }
static uint32_t age(uint64_t now, uint64_t then, bool valid)
{ return !valid ? UINT32_MAX : (now - then > UINT32_MAX ? UINT32_MAX : (uint32_t)(now - then)); }
static bool in_limits(int32_t target)
{ return target >= active_negative && target <= active_positive && target >= -1000 && target <= 1000; }

static void note(const char *text)
{
    for (int i = APP_LOG_LINES - 1; i > 0; --i)
        memcpy(model.log[i], model.log[i - 1], APP_LOG_WIDTH);
    snprintf(model.log[0], APP_LOG_WIDTH, "%8lu %s", (unsigned long)(now_ms() / 1000), text);
    snprintf(model.result, sizeof(model.result), "%s", text);
    ESP_LOGI(TAG, "%s", text);
}

static void disarm(void)
{
    model.armed = false;
    model.demo_active = false;
    atomic_fetch_add(&motion_epoch, 1);
}

static void invalidate_readback(void)
{
    model.position_valid = model.status_valid = model.features_valid = false;
    model.negative_limit_valid = model.positive_limit_valid = false;
    model.node_valid = model.version_valid = raw_position_valid = false;
    model.reporting_requested = false;
    last_event_time = 0;
    memset(&metadata_recovery, 0, sizeof(metadata_recovery));
    metadata_recovery.pending = true;
}

static void publish(void)
{
    model.host_negative_limit = active_negative;
    model.host_positive_limit = active_positive;
    if (bus) {
#if CONFIG_KST_TRANSPORT_USB_GS
        kst_usb_gs_stats_t stats;
        kst_usb_gs_get_stats(bus, &stats);
        if (!link_seen || link_generation != stats.generation || (!stats.ready && model.ready)) {
            atomic_fetch_add(&connection_epoch, 1);
            disarm();
            invalidate_readback();
            link_needs_refresh = true;
        }
        link_seen = true;
        link_generation = stats.generation;
        model.usb_transport = true;
        model.adapter_connected = stats.connected;
        model.ready = stats.ready;
        model.usb_vid = stats.vid;
        model.usb_pid = stats.pid;
        snprintf(model.transport_status, sizeof(model.transport_status), "%s", stats.status);
#else
        kst_twai_stats_t stats;
        kst_twai_get_stats(bus, &stats);
#endif
        model.tx_count = stats.tx_ok;
        model.rx_count = stats.rx_frames;
        model.dropped_frames = stats.rx_dropped;
        model.bus_errors = stats.bus_errors;
        model.bus_off = stats.bus_off;
    }
    uint64_t t = now_ms();
    model.position_outside_limits = model.position_valid && !in_limits(model.position_deci_deg);
    model.position_age_ms = age(t, position_time, model.position_valid);
    model.status_age_ms = age(t, status_time, model.status_valid);
    model.telemetry_stale_ms = CONFIG_KST_STALE_MS;
    model.online = model.ready && last_event_time && t - last_event_time <= CONFIG_KST_STALE_MS;
    portENTER_CRITICAL(&model_lock);
    published = model;
    portEXIT_CRITICAL(&model_lock);
}

void app_get_snapshot(app_snapshot_t *out)
{
    if (!out) return;
    portENTER_CRITICAL(&model_lock);
    *out = published;
    portEXIT_CRITICAL(&model_lock);
}

static bool is_configuration(app_action_t a)
{
    return a == APP_CENTER || a == APP_REVERSE || a == APP_UV_ALARM ||
           a == APP_LIMIT_NEGATIVE || a == APP_LIMIT_POSITIVE ||
           a == APP_SET_NODE || a == APP_SET_BAUD || a == APP_SAVE ||
           a == APP_CONNECT || a == APP_RECOVER_BUS;
}

bool app_submit(app_action_t action, int32_t value, int32_t value2)
{
    if (!requests || action < APP_REFRESH || action > APP_CLEAR_ALERTS) return false;
    /* Capture at entry: a submitter can be preempted across an entire probe.
     * Constructing its epoch after cancellation would mislabel an old action. */
    const unsigned entry_connection_epoch = atomic_load(&connection_epoch);
    const unsigned entry_motion_epoch = atomic_load(&motion_epoch);
    /* Cancellation bypasses the FIFO, including a full FIFO. It prevents queued
     * motion from re-arming after STOP. A frame already on the wire cannot be
     * recalled; this is a HOST TEST PAUSE, never a motor emergency stop. */
    bool stop = action == APP_STOP_MOTION || (action == APP_ARM && !value);
#if CONFIG_KST_TRANSPORT_USB_GS && CONFIG_KST_PROBE_360_ON_START
    if (atomic_load(&limit_probe_active) && !stop) return false;
#endif
    if (stop || is_configuration(action)) {
        atomic_fetch_add(&motion_epoch, 1);
        atomic_store(&cancel_requested, true);
    }
    if (stop) return true;
    request_t req = { action, value, value2, entry_motion_epoch, entry_connection_epoch };
    return xQueueSend(requests, &req, 0) == pdTRUE;
}

static void update_position(void)
{
    model.position_valid = raw_position_valid && model.features_valid;
    model.position_deci_deg = (model.feature_bits & KST_FEATURE_REVERSE) ? -raw_position : raw_position;
}

static void on_event(void *user, const kst_event_t *event)
{
    (void)user;
    last_event_time = now_ms();
    switch (event->type) {
    case KST_EVENT_POSITION_REPORT:
        /* The manual only specifies reversed sign for READ POSITION replies.
         * With REV enabled, reports prove activity but cannot be trusted as
         * command-axis samples until their sign semantics are bench verified. */
        if (!model.features_valid || (model.feature_bits & KST_FEATURE_REVERSE)) break;
        /* fall through */
    case KST_EVENT_POSITION:
        raw_position = event->position_deci_deg;
        raw_position_valid = true;
        position_time = last_event_time;
        update_position();
        break;
    case KST_EVENT_STATUS:
        model.current_10ma = event->current_10ma;
        model.temperature_c = event->temperature_c;
        model.status_valid = true;
        status_time = last_event_time;
        break;
    case KST_EVENT_FEATURES:
        if ((model.feature_bits ^ event->feature_bits) & KST_FEATURE_REVERSE)
            raw_position_valid = false; /* old sample belongs to the old sign convention */
        model.feature_bits = event->feature_bits;
        model.feature_raw = event->feature_raw;
        model.features_valid = true;
        update_position();
        break;
    case KST_EVENT_NODE_ID:
        model.device_node_id = event->node_id;
        model.node_valid = true;
        break;
    case KST_EVENT_LIMIT_NEGATIVE:
        model.negative_limit = event->position_deci_deg;
        model.negative_limit_valid = true;
        if (model.negative_limit > active_negative) active_negative = model.negative_limit;
        break;
    case KST_EVENT_LIMIT_POSITIVE:
        model.positive_limit = event->position_deci_deg;
        model.positive_limit_valid = true;
        if (model.positive_limit < active_positive) active_positive = model.positive_limit;
        break;
    case KST_EVENT_VERSION:
        snprintf(model.version, sizeof(model.version), "%s", event->version);
        model.version_valid = true;
        break;
    case KST_EVENT_UNDERVOLTAGE:
        model.low_voltage_latched = true;
        disarm();
        note("LOW VOLTAGE event latched; inspect supply, then acknowledge");
        break;
    case KST_EVENT_ABORT: {
        char text[100];
        model.abort_code = event->abort_code;
        disarm();
        snprintf(text, sizeof(text), "ABORT %08lX @%04X: %s", (unsigned long)event->abort_code,
                 event->index, kst_abort_description(event->abort_code));
        note(text);
        break;
    }
    default: break;
    }
}

static void result(const char *operation, kst_result_t r, bool announce)
{
    if (r == KST_SERVO_ERR_TIMEOUT) model.timeouts++;
    if (r < 0) {
        disarm();
        if (r == KST_SERVO_ERR_TRANSPORT && !target_denied) model.ready = false;
    }
    if (announce || r < 0) {
        char text[100];
        snprintf(text, sizeof(text), "%s: %s", operation,
                 target_denied ? "canceled before CAN transmit" : kst_servo_result_description(r));
        /* Avoid flooding the log every poll while a disconnected servo waits. */
        if (announce || strcmp(model.result, text)) note(text);
    }
    target_denied = false;
}

static bool motion_ready(void)
{
    publish();
    return model.ready && model.online && !model.bus_off && !model.low_voltage_latched &&
           !model.abort_code && !model.limit_probe_restore_unverified &&
           model.features_valid && model.position_valid && model.status_valid &&
           model.negative_limit_valid && model.positive_limit_valid &&
           model.position_age_ms <= CONFIG_KST_STALE_MS && model.status_age_ms <= CONFIG_KST_STALE_MS;
}

/* The client drains old frames immediately before a transaction. That drain
 * can reveal a UV/ABORT and disarm us; the final gate must be BELOW the drain.
 * It also catches PAUSE received between the UI handler and the actual send.
 * Once the transport has accepted a frame it cannot be recalled by this API. */
static bool transmit_allowed(const kst_can_frame_t *frame, bool rx_pending)
{
    target_denied = false;
    publish();
    if (send_connection_epoch != atomic_load(&connection_epoch)) {
        target_denied = true;
        return false;
    }
    if (model.limit_probe_restore_unverified &&
        (frame->len != 8 || frame->data[0] != 0x40)) {
        target_denied = true;
        return false;
    }
#if CONFIG_KST_TRANSPORT_USB_GS && CONFIG_KST_PROBE_360_ON_START
    if (limit_probe_running &&
        (frame->id != 0x605 || frame->len != 8 || frame->data[2] != 0x30 ||
         (frame->data[1] != 0x0a && frame->data[1] != 0x0b) || frame->data[3] != 0 ||
         (frame->data[0] != 0x40 && frame->data[0] != 0x22) ||
         (!limit_probe_restoring && atomic_load(&cancel_requested)))) {
        target_denied = true;
        return false;
    }
#endif
    if (frame->len == 8 && frame->data[1] == 0x03 && frame->data[2] == 0x60) {
        int16_t target = (int16_t)((uint16_t)frame->data[4] | ((uint16_t)frame->data[5] << 8));
        if (rx_pending || !model.armed || atomic_load(&cancel_requested) ||
            target_epoch != atomic_load(&motion_epoch) || !motion_ready() || !in_limits(target)) {
            target_denied = true;
            return false;
        }
    }
    return true;
}

#if CONFIG_KST_TRANSPORT_USB_GS
static bool usb_tx_guard(void *ctx, const kst_can_frame_t *frame, bool rx_pending)
{
    (void)ctx;
    /* USB may wait for the previous echo. Recheck after that wait; a new RX
     * might be UV/ABORT and must be dispatched before issuing another target. */
    return transmit_allowed(frame, rx_pending);
}
#endif

static int guarded_send(void *ctx, const kst_can_frame_t *frame, uint32_t timeout)
{
    (void)ctx;
    if (!transmit_allowed(frame, false)) return -1;
    return native_transport.send(native_transport.ctx, frame, timeout);
}

static int guarded_receive(void *ctx, kst_can_frame_t *frame, bool *ext, bool *rtr, bool *fd, uint32_t timeout)
{ (void)ctx; return native_transport.receive(native_transport.ctx, frame, ext, rtr, fd, timeout); }
static uint64_t guarded_clock(void *ctx)
{ (void)ctx; return native_transport.now_ms(native_transport.ctx); }

static void cancel_if_requested(void)
{
    if (atomic_exchange(&cancel_requested, false)) {
        /* epoch was advanced by the submitter, so do not invalidate a later
         * explicit ARM queued after the user's PAUSE action. */
        model.armed = false;
        model.demo_active = false;
        note("HOST TEST PAUSED / DISARMED; servo may still hold or move");
    }
}

static void refresh_all(bool announce)
{
    send_connection_epoch = atomic_load(&connection_epoch);
    result("Read features", kst_servo_read_features(&servo, NULL), announce);
    result("Read position", kst_servo_read_position(&servo, NULL), announce);
    result("Read status", kst_servo_read_status(&servo, NULL), announce);
    result("Read -limit", kst_servo_read_limit(&servo, false, NULL), announce);
    result("Read +limit", kst_servo_read_limit(&servo, true, NULL), announce);
    result("Read node", kst_servo_read_node(&servo, NULL), announce);
    result("Read version", kst_servo_read_version(&servo, NULL), announce);
}

/* Startup may precede the servo's first reply. Recover only the four readbacks
 * absent from normal polling: one transaction per call, three tries per item,
 * with at least one second between transactions. Never write or arm here. */
static void step_startup_metadata_recovery(void)
{
    if (!metadata_recovery.pending) return;
    publish();
    const bool valid[METADATA_COUNT] = {
        model.negative_limit_valid, model.positive_limit_valid,
        model.node_valid, model.version_valid
    };
    unsigned missing = 0, retryable = 0;
    for (unsigned i = 0; i < METADATA_COUNT; ++i) {
        if (!valid[i]) {
            missing |= 1U << i;
            if (metadata_recovery.attempts[i] < METADATA_RECOVERY_ATTEMPTS) ++retryable;
        }
    }
    if (!missing || !retryable) {
        metadata_recovery.pending = false;
        ESP_LOGI("kst_diag", "STARTUP_READBACK done missing_mask=0x%X (neg,pos,node,version); retry_limit=%u",
                 missing, METADATA_RECOVERY_ATTEMPTS);
        return;
    }
    if (!model.ready || !model.online || model.bus_off || model.armed || model.demo_active ||
        atomic_load(&cancel_requested) || now_ms() < metadata_recovery.next_ms) return;
    for (unsigned offset = 0; offset < METADATA_COUNT; ++offset) {
        const unsigned item = (metadata_recovery.cursor + offset) % METADATA_COUNT;
        if (valid[item] || metadata_recovery.attempts[item] >= METADATA_RECOVERY_ATTEMPTS) continue;
        const unsigned epoch = atomic_load(&connection_epoch);
        send_connection_epoch = epoch;
        ++metadata_recovery.attempts[item];
        metadata_recovery.cursor = (item + 1) % METADATA_COUNT;
        kst_result_t r;
        const char *label;
        switch (item) {
        case METADATA_NEGATIVE:
            label = "Recover startup -limit"; r = kst_servo_read_limit(&servo, false, NULL); break;
        case METADATA_POSITIVE:
            label = "Recover startup +limit"; r = kst_servo_read_limit(&servo, true, NULL); break;
        case METADATA_NODE:
            label = "Recover startup node"; r = kst_servo_read_node(&servo, NULL); break;
        default:
            label = "Recover startup version"; r = kst_servo_read_version(&servo, NULL); break;
        }
        result(label, r, false);
        publish();
        /* publish() resets the budget when USB changes during a transaction. */
        if (epoch != atomic_load(&connection_epoch)) return;
        metadata_recovery.next_ms = now_ms() + METADATA_RECOVERY_INTERVAL_MS;
        return;
    }
}

#if CONFIG_KST_TRANSPORT_USB_GS
#if CONFIG_KST_DISCOVER_ON_START
static bool discovery_can_continue(void)
{
    kst_usb_gs_stats_t stats;
    kst_usb_gs_get_stats(bus, &stats);
    publish();
    return !atomic_load(&cancel_requested) && stats.ready && model.ready &&
           stats.generation == discovery_generation &&
           atomic_load(&connection_epoch) == discovery_connection_epoch;
}

/* Deliberately local diagnostic exception: do not enable node zero in the
 * reusable constructors or client. This is the reference driver's READ NODE. */
static bool discovery_broadcast_read_node(bool found_nodes[128], unsigned *found)
{
    const kst_can_frame_t request = {
        .id = 0x600, .len = 8, .data = {0x40, 0x00, 0x30, 0, 0, 0, 0, 0}
    };
    if (!discovery_can_continue()) return false;
    ESP_LOGI("kst_discover", "BCAST_TX ext=1 id=00000600 dlc=8 data=40 00 30 00 00 00 00 00");
    if (guarded_send(NULL, &request, 200) != 0) return false;
    const uint64_t deadline = now_ms() + 500;
    unsigned raw_frames = 0;
    while (now_ms() < deadline) {
        if (!discovery_can_continue()) return false;
        const uint64_t now = now_ms();
        if (now >= deadline) break;
        const uint64_t remaining = deadline - now;
        const uint32_t wait = remaining > 50 ? 50 : (uint32_t)remaining;
        kst_can_frame_t frame = {0};
        bool extended = false, rtr = false, fd = false;
        const int received = guarded_receive(NULL, &frame, &extended, &rtr, &fd, wait);
        if (!discovery_can_continue()) return false;
        if (received < 0 || received > 1) return false;
        if (!received) continue;
        ++raw_frames;
        ESP_LOGI("kst_discover", "BCAST_RAW id=%08lX ext=%u rtr=%u fd=%u dlc=%u data=%02X %02X %02X %02X %02X %02X %02X %02X",
                 (unsigned long)frame.id, (unsigned)extended, (unsigned)rtr, (unsigned)fd,
                 (unsigned)frame.len, (unsigned)frame.data[0], (unsigned)frame.data[1],
                 (unsigned)frame.data[2], (unsigned)frame.data[3], (unsigned)frame.data[4],
                 (unsigned)frame.data[5], (unsigned)frame.data[6], (unsigned)frame.data[7]);
        if (frame.id >= 0x581 && frame.id <= 0x5ff) {
            const uint8_t node = (uint8_t)(frame.id - 0x580);
            kst_event_t event;
            if (kst_parse(node, &frame, extended, rtr, fd, &event) == KST_PARSE_OK &&
                event.type == KST_EVENT_NODE_ID) {
                if (!found_nodes[node]) { found_nodes[node] = true; ++*found; }
                ESP_LOGI("kst_discover", "BCAST_FOUND reply_node=%u reported_node=%u",
                         (unsigned)event.node, (unsigned)event.node_id);
            }
        }
    }
    ESP_LOGI("kst_discover", "BCAST_END raw_frames=%u found=%u collect_ms=500", raw_frames, *found);
    (void)raw_frames; /* Host test logger may compile out formatting arguments. */
    return true;
}

/* A separate temporary client prevents another node's version/ABORT replies
 * from becoming the selected servo's telemetry. The original client is untouched. */
static unsigned run_startup_discovery(void)
{
    startup_discovery_done = true;
    if (!bus || !model.ready || last_event_time || atomic_load(&cancel_requested)) return 0;
    const bool previous_busy = model.busy;
    disarm();
    atomic_fetch_add(&connection_epoch, 1);
    discovery_connection_epoch = atomic_load(&connection_epoch);
    send_connection_epoch = discovery_connection_epoch;
    kst_usb_gs_stats_t stats;
    kst_usb_gs_get_stats(bus, &stats);
    discovery_generation = stats.generation;
    discovery_running = true;
    model.busy = true;
    publish();
    ESP_LOGI("kst_discover", "START extended read-version nodes=1..127 timeout_ms=200; selected_node=%u unchanged",
             (unsigned)model.node_id);
    const kst_transport_t transport = {.send = guarded_send, .receive = guarded_receive,
                                       .now_ms = guarded_clock};
    unsigned found = 0, probed = 0;
    bool found_nodes[128] = {false};
    const char *reason = "completed";
    if (!discovery_broadcast_read_node(found_nodes, &found)) {
        reason = "broadcast canceled, USB changed or transport failure";
        goto finish_discovery;
    }
    for (unsigned node = 1; node <= 127; ++node) {
        (void)kst_usb_gs_service(bus, 0);
        if (!discovery_can_continue()) { reason = "canceled or USB changed"; break; }
        kst_servo_t probe;
        if (kst_servo_init(&probe, (uint8_t)node, &transport, 200, NULL, NULL) != KST_SERVO_OK) {
            reason = "client initialization failed";
            break;
        }
        kst_event_t version;
        ++probed;
        kst_result_t result = kst_servo_read_version(&probe, &version);
        if (!discovery_can_continue()) { reason = "canceled or USB changed"; break; }
        if (result == KST_SERVO_OK) {
            if (!found_nodes[node]) { found_nodes[node] = true; ++found; }
            ESP_LOGI("kst_discover", "FOUND node=%u (0x%02X) version=%s",
                     node, node, version.version);
        } else if (result == KST_SERVO_ERR_ABORT) {
            /* Valid node reply, even though this read was rejected. */
            if (!found_nodes[node]) { found_nodes[node] = true; ++found; }
            ESP_LOGI("kst_discover", "FOUND node=%u (0x%02X) version=unavailable abort=%08lX",
                     node, node, (unsigned long)probe.last_abort);
        } else if (result != KST_SERVO_ERR_TIMEOUT) {
            reason = "transport or RX failure";
            break;
        }
        if ((probed % 16) == 0)
            ESP_LOGI("kst_discover", "PROGRESS node=%u/127 found=%u", node, found);
    }
finish_discovery:
    discovery_running = false;
    model.busy = previous_busy;
    disarm();
    /* Includes configuration/connect requests queued while the scan owned CAN. */
    atomic_fetch_add(&connection_epoch, 1);
    send_connection_epoch = atomic_load(&connection_epoch);
    discovery_queue_cutoff = send_connection_epoch;
    discovery_queue_cutoff_valid = true;
    target_denied = false;
    publish();
    ESP_LOGI("kst_discover", "END probed=%u found=%u reason=%s; selected_node=%u unchanged",
             probed, found, reason, (unsigned)model.node_id);
    (void)reason; /* Host test logger may compile out formatting arguments. */
    return found;
}
#endif

#if CONFIG_KST_PROBE_360_ON_START
typedef enum {
    LIMIT_PROBE_OK, LIMIT_PROBE_TIMEOUT, LIMIT_PROBE_ABORT,
    LIMIT_PROBE_CANCELED, LIMIT_PROBE_LINK_CHANGED, LIMIT_PROBE_IO
} limit_probe_result_t;

static limit_probe_result_t limit_probe_check(bool restoring)
{
    publish();
    kst_usb_gs_stats_t stats;
    kst_usb_gs_get_stats(bus, &stats);
    if (!model.ready || !stats.ready || stats.generation != limit_probe_generation ||
        atomic_load(&connection_epoch) != limit_probe_connection_epoch)
        return LIMIT_PROBE_LINK_CHANGED;
    return !restoring && atomic_load(&cancel_requested) ? LIMIT_PROBE_CANCELED : LIMIT_PROBE_OK;
}

static void limit_probe_log_frame(const char *phase, const char *direction,
                                 const kst_can_frame_t *f, bool ext, bool rtr, bool fd)
{
    ESP_LOGI("kst_probe360", "%s %s id=%08lX ext=%u rtr=%u fd=%u dlc=%u data=%02X %02X %02X %02X %02X %02X %02X %02X",
             phase, direction, (unsigned long)f->id, (unsigned)ext, (unsigned)rtr,
             (unsigned)fd, (unsigned)f->len, (unsigned)f->data[0], (unsigned)f->data[1],
             (unsigned)f->data[2], (unsigned)f->data[3], (unsigned)f->data[4],
             (unsigned)f->data[5], (unsigned)f->data[6], (unsigned)f->data[7]);
    (void)phase; (void)direction; (void)f; (void)ext; (void)rtr; (void)fd;
}

/* Private raw whitelist, not a relaxed constructor: only node 5's two limit
 * objects. Every exchange has one absolute 200 ms deadline and 64-frame caps.
 * Candidate readbacks never enter on_event() or expand the normal host limits. */
static limit_probe_result_t limit_probe_exchange(const char *phase, bool positive,
                                                bool write, int16_t value, bool restoring,
                                                int16_t *readback, bool *attempted)
{
    const uint16_t index = positive ? KST_INDEX_LIMIT_POSITIVE : KST_INDEX_LIMIT_NEGATIVE;
    if (attempted) *attempted = false;
    if (write && (positive ? value < 0 || value > 1800 : value < -1800 || value > 0))
        return LIMIT_PROBE_IO;
    const uint64_t deadline = now_ms() + 200;
    bool drained = false;
    for (unsigned i = 0; i < 64; ++i) {
        limit_probe_result_t state = limit_probe_check(restoring);
        if (state != LIMIT_PROBE_OK) return state;
        if (now_ms() >= deadline) return LIMIT_PROBE_TIMEOUT;
        kst_can_frame_t old = {0}; bool ext = false, rtr = false, fd = false;
        const int received = guarded_receive(NULL, &old, &ext, &rtr, &fd, 0);
        if (received < 0 || received > 1) return LIMIT_PROBE_IO;
        if (!received) { drained = true; break; }
        limit_probe_log_frame(phase, "DRAIN", &old, ext, rtr, fd);
    }
    if (!drained) return LIMIT_PROBE_IO;
    limit_probe_result_t state = limit_probe_check(restoring);
    if (state != LIMIT_PROBE_OK) return state;
    kst_can_frame_t request = {.id = 0x605, .len = 8,
        .data = {write ? 0x22 : 0x40, (uint8_t)index, (uint8_t)(index >> 8), 0,
                 write ? (uint8_t)(uint16_t)value : 0,
                 write ? (uint8_t)((uint16_t)value >> 8) : 0, 0, 0}};
    const uint64_t before_send = now_ms();
    if (before_send >= deadline) return LIMIT_PROBE_TIMEOUT;
    limit_probe_log_frame(phase, "TX_REQUEST", &request, true, false, false);
    limit_probe_restoring = restoring;
    if (attempted) *attempted = true; /* A send error cannot prove non-delivery. */
    if (guarded_send(NULL, &request, (uint32_t)(deadline - before_send)) != 0) return LIMIT_PROBE_IO;
    for (unsigned i = 0; i < 64; ++i) {
        state = limit_probe_check(restoring);
        if (state != LIMIT_PROBE_OK) return state;
        const uint64_t now = now_ms();
        if (now >= deadline) return LIMIT_PROBE_TIMEOUT;
        const uint64_t remaining = deadline - now;
        kst_can_frame_t f = {0}; bool ext = false, rtr = false, fd = false;
        const int received = guarded_receive(NULL, &f, &ext, &rtr, &fd,
                                             remaining > 20 ? 20 : (uint32_t)remaining);
        state = limit_probe_check(restoring);
        if (state != LIMIT_PROBE_OK) return state;
        if (received < 0 || received > 1) return LIMIT_PROBE_IO;
        if (!received) continue;
        limit_probe_log_frame(phase, "RX", &f, ext, rtr, fd);
        if (!ext || rtr || fd || f.id != 0x585 || f.len != 8 || f.data[1] != (uint8_t)index ||
            f.data[2] != (uint8_t)(index >> 8) || f.data[3] != 0) continue;
        if (f.data[0] == 0x80) {
            ESP_LOGI("kst_probe360", "%s ABORT wire=%02X %02X %02X %02X", phase,
                     (unsigned)f.data[4], (unsigned)f.data[5], (unsigned)f.data[6], (unsigned)f.data[7]);
            return LIMIT_PROBE_ABORT;
        }
        if (write) {
            if (f.data[0] == 0x60 && !f.data[4] && !f.data[5] && !f.data[6] && !f.data[7])
                return LIMIT_PROBE_OK;
        } else if (f.data[0] == 0x4B) {
            const uint16_t bits = (uint16_t)((uint16_t)f.data[4] | ((uint16_t)f.data[5] << 8));
            const int32_t decoded = (bits & 0x8000U) ? (int32_t)bits - 65536 : bits;
            const bool tail = (!f.data[6] && !f.data[7]) ||
                (!positive && decoded < 0 && f.data[6] == 0xff && f.data[7] == 0xff);
            if (tail && (positive ? decoded >= 0 && decoded <= 1800 : decoded >= -1800 && decoded <= 0)) {
                if (readback) *readback = (int16_t)decoded;
                return LIMIT_PROBE_OK;
            }
        }
    }
    return LIMIT_PROBE_IO;
}

static bool limit_probe_direction(bool positive)
{
    int16_t original = 0, observed = 0, verified = 0;
    const int16_t candidate = positive ? 1800 : -1800;
    const char *side = positive ? "POSITIVE" : "NEGATIVE";
    limit_probe_result_t r = limit_probe_exchange("ORIGINAL", positive, false, 0, false, &original, NULL);
    if (r != LIMIT_PROBE_OK || !in_limits(original)) {
        ESP_LOGI("kst_probe360", "%s SKIP original unavailable/unexpected result=%u value=%d", side, (unsigned)r, (int)original);
        return false;
    }
    ESP_LOGI("kst_probe360", "%s ORIGINAL=%d candidate=%d (0.1deg)", side, (int)original, (int)candidate);
    bool attempted = false;
    const limit_probe_result_t written = limit_probe_exchange("CANDIDATE", positive, true, candidate,
                                                             false, NULL, &attempted);
    if (!attempted) return false;
    /* PAUSE prevents new experiments, but same-connection cleanup must finish. */
    const limit_probe_result_t read = limit_probe_exchange("READBACK", positive, false, 0, true, &observed, NULL);
    /* Even ACK followed by the old readback may mean READ exposes active data
     * while WRITE changed a staged value. Explicitly overwrite every attempted
     * candidate with the original, including ABORT/timeout/unchanged readback. */
    const limit_probe_result_t restore = limit_probe_exchange("RESTORE", positive, true, original, true, NULL, NULL);
    const limit_probe_result_t verify = limit_probe_exchange("VERIFY_ORIGINAL", positive, false, 0, true, &verified, NULL);
    const bool restored = restore == LIMIT_PROBE_OK && verify == LIMIT_PROBE_OK && verified == original;
    ESP_LOGI("kst_probe360", "%s RESTORE result=%u verify=%u expected=%d actual=%d verified=%u", side,
             (unsigned)restore, (unsigned)verify, (int)original, (int)verified, (unsigned)restored);
    if (!restored) {
        model.limit_probe_restore_unverified = true;
        model.config_dirty = model.requires_restart = true;
        note("PROBE RESTORE UNVERIFIED: motion/parameter writes/SAVE locked; manually verify original limits");
    }
    const bool accepted = written == LIMIT_PROBE_OK && read == LIMIT_PROBE_OK && observed == candidate;
    ESP_LOGI("kst_probe360", "%s END write=%u read=%u observed=%d accepted=%u restored=%u", side,
             (unsigned)written, (unsigned)read, (int)observed, (unsigned)accepted, (unsigned)restored);
    (void)side;
    return accepted && restored && limit_probe_check(false) == LIMIT_PROBE_OK;
}

static void run_startup_limit_probe(void)
{
    if (limit_probe_done) return;
    limit_probe_done = true;
    publish();
    if (!bus || servo.node != 5 || model.node_id != 5 || !model.node_valid || model.device_node_id != 5 ||
        !model.version_valid || strcmp(model.version, "3.66") || !motion_ready() ||
        !in_limits(model.position_deci_deg) || model.config_dirty || model.requires_restart ||
        atomic_load(&cancel_requested)) {
        ESP_LOGI("kst_probe360", "SKIP: require node5/3.66 complete fresh readbacks, clean config, no alerts, in-range position");
        return;
    }
    atomic_store(&limit_probe_active, true);
    disarm();
    const bool previous_busy = model.busy;
    model.busy = true;
    atomic_fetch_add(&connection_epoch, 1);
    limit_probe_connection_epoch = atomic_load(&connection_epoch);
    send_connection_epoch = limit_probe_connection_epoch;
    kst_usb_gs_stats_t stats;
    kst_usb_gs_get_stats(bus, &stats);
    limit_probe_generation = stats.generation;
    limit_probe_running = true;
    publish();
    ESP_LOGI("kst_probe360", "START one trial per side; no motion, center, features or SAVE; original limits remain host bounds");
    const bool positive = limit_probe_direction(true);
    const bool negative = positive && limit_probe_direction(false);
    limit_probe_running = false;
    limit_probe_restoring = false;
    disarm();
    model.busy = previous_busy;
    atomic_fetch_add(&connection_epoch, 1);
    send_connection_epoch = atomic_load(&connection_epoch);
    limit_probe_queue_cutoff = send_connection_epoch;
    limit_probe_queue_cutoff_valid = true;
    atomic_store(&limit_probe_active, false);
    target_denied = false;
    publish();
    ESP_LOGI("kst_probe360", "END positive_accepted_restored=%u negative_accepted_restored=%u restore_unverified=%u; acceptance is not mechanical-travel proof",
             (unsigned)positive, (unsigned)negative, (unsigned)model.limit_probe_restore_unverified);
    (void)negative;
}
#endif

/* Also runs while no USB adapter is present. Only the CAN worker owns USB
 * client calls. A reconnect refreshes readbacks but never replays UI commands. */
static void service_transport(void)
{
    if (!bus) return;
    kst_usb_gs_service(bus, 10);
    publish();
    if (model.ready && link_needs_refresh) {
        link_needs_refresh = false;
        note("USB-CAN ready; fresh readbacks required; motion disarmed");
        refresh_all(false);
#if CONFIG_KST_DISCOVER_ON_START
        if (!startup_discovery_done) (void)run_startup_discovery();
#endif
#if CONFIG_KST_PROBE_360_ON_START
        run_startup_limit_probe();
#endif
    }
}
#endif

static bool valid_bitrate(int32_t value, kst_baudrate_t *out)
{
    for (int i = KST_BAUD_20K; i <= KST_BAUD_1000K; ++i)
        if (kst_baudrate_bps((kst_baudrate_t)i) == (uint32_t)value) {
            if (out) *out = (kst_baudrate_t)i;
            return true;
        }
    return false;
}

static void connect_host(int node, int bitrate)
{
    if (node < 1 || node > 127 || !valid_bitrate(bitrate, NULL)) { note("Invalid local node / bitrate"); return; }
    disarm();
#if CONFIG_KST_TRANSPORT_USB_GS
    if (bus) {
        kst_usb_gs_stats_t state;
        kst_usb_gs_get_stats(bus, &state);
        if (state.requires_close) {
            model.ready = false;
            if (kst_usb_gs_close(bus) != ESP_OK) {
                note("USB shutdown pending; retry CONNECT after disconnecting adapter");
                return;
            }
            bus = NULL;
        }
    }
    esp_err_t e = bus ? kst_usb_gs_reopen(bus, bitrate) : kst_usb_gs_open(bitrate, &bus);
#else
    esp_err_t e = bus ? kst_twai_reopen(bus, bitrate) :
        kst_twai_open(CONFIG_KST_TX_GPIO, CONFIG_KST_RX_GPIO, bitrate, &bus);
#endif
    if (e != ESP_OK) { model.ready = false; note("CAN controller unavailable; reconnect required"); return; }
#if CONFIG_KST_TRANSPORT_USB_GS
    if (kst_usb_gs_set_tx_guard(bus, usb_tx_guard, NULL) != ESP_OK) {
        model.ready = false;
        note("USB transmit guard unavailable; reconnect required");
        return;
    }
    native_transport = kst_usb_gs_transport(bus);
#else
    native_transport = kst_twai_transport(bus);
#endif
    kst_transport_t transport = {.send = guarded_send, .receive = guarded_receive, .now_ms = guarded_clock};
    if (kst_servo_init(&servo, node, &transport, CONFIG_KST_TIMEOUT_MS, on_event, NULL) != KST_SERVO_OK) {
        model.ready = false; note("CAN client initialization failed"); return;
    }
    model.ready = true;
    model.node_id = node;
    model.bitrate = bitrate;
    invalidate_readback();
    /* Keep alert/configuration latches across host reconnect. A reconnect is
     * not proof of servo restart or nonvolatile SAVE completion. */
#if CONFIG_KST_TRANSPORT_USB_GS
    model.ready = false;
    link_seen = false;
    link_needs_refresh = true;
    note("USB Host started; connect candleLight to Korvo USB-A");
    service_transport();
#else
    note("Local CAN connected; servo startup is read-only");
    refresh_all(false);
#endif
}

static void handle(const request_t *req)
{
    app_action_t a = req->action;
    kst_result_t r = KST_SERVO_OK;
    const char *label = "Command";
    kst_baudrate_t baud;
#if CONFIG_KST_TRANSPORT_USB_GS && CONFIG_KST_PROBE_360_ON_START
    const unsigned probe_age = limit_probe_queue_cutoff - req->connection_epoch;
    if (limit_probe_queue_cutoff_valid && probe_age && probe_age <= UINT_MAX / 2) {
        note("Discarded command queued before or during limit probe");
        return;
    }
#endif
#if CONFIG_KST_TRANSPORT_USB_GS && CONFIG_KST_DISCOVER_ON_START
    /* Scan-only cutoff also catches a submitter that read its epoch during
     * discovery but was preempted before enqueue. Ordinary reconnects retain
     * the existing local CONNECT/RECOVER exemption below. */
    const unsigned scan_age = discovery_queue_cutoff - req->connection_epoch;
    if (discovery_queue_cutoff_valid && scan_age && scan_age <= UINT_MAX / 2) {
        note("Discarded command queued before or during node discovery");
        return;
    }
#endif
    if (a == APP_CONNECT) { connect_host(req->a, req->b); return; }
    if (a == APP_RECOVER_BUS) { connect_host(model.node_id, model.bitrate); return; }
    if (req->connection_epoch != atomic_load(&connection_epoch)) {
        note("Discarded command queued for a previous USB connection");
        return;
    }
    if (a == APP_CLEAR_ALERTS) {
        disarm(); model.low_voltage_latched = false; model.abort_code = 0;
        note("Alerts acknowledged; supply recovery is NOT verified"); return;
    }
    send_connection_epoch = req->connection_epoch;
    if (!model.ready) { note("CAN unavailable; use LINK / CONNECT LOCAL"); return; }
    if (model.limit_probe_restore_unverified && (a < APP_REFRESH || a > APP_READ_VERSION)) {
        disarm(); note("Limit restore unverified: only readback/local reconnect allowed; do not SAVE"); return;
    }
    if (a == APP_ARM || a == APP_MOVE || a == APP_DEMO_START) {
        if (req->epoch != atomic_load(&motion_epoch)) { note("Discarded motion queued before cancellation"); return; }
        if (!motion_ready()) { disarm(); note("Motion blocked: need fresh telemetry, limits and cleared alerts"); return; }
    }
    if (is_configuration(a)) { model.armed = false; model.demo_active = false; }
    switch (a) {
    case APP_REFRESH: refresh_all(true); return;
    case APP_READ_POSITION: label = "Read position"; r = kst_servo_read_position(&servo, NULL); break;
    case APP_READ_STATUS: label = "Read status"; r = kst_servo_read_status(&servo, NULL); break;
    case APP_READ_FEATURES: label = "Read features"; r = kst_servo_read_features(&servo, NULL); break;
    case APP_READ_LIMITS:
        result("Read -limit", kst_servo_read_limit(&servo, false, NULL), true);
        label = "Read +limit"; r = kst_servo_read_limit(&servo, true, NULL); break;
    case APP_READ_NODE: label = "Read node"; r = kst_servo_read_node(&servo, NULL); break;
    case APP_READ_VERSION: label = "Read version"; r = kst_servo_read_version(&servo, NULL); break;
    case APP_REPORT_START:
        label = "Start reports (first report)";
        r = kst_servo_set_reporting(&servo, true);
        model.reporting_requested = r == KST_SERVO_OK;
        break;
    case APP_REPORT_STOP:
        label = "Stop reports (no ACK)"; r = kst_servo_set_reporting(&servo, false);
        if (r >= 0) model.reporting_requested = false;
        break;
    case APP_REPORT_INTERVAL:
        if (req->a < 10 || req->a > 255) { note("Report interval must be 10..255 ms"); return; }
        model.config_dirty = true; /* A lost ACK cannot prove the write did not execute. */
        label = "Set report interval"; r = kst_servo_set_report_interval(&servo, req->a);
        if (r >= 0) { model.report_interval_ms = req->a; model.config_dirty = true; }
        break;
    case APP_ARM:
        model.armed = req->a != 0; note(model.armed ? "HOST ARMED; explicit target required" : "HOST DISARMED"); return;
    case APP_MOVE:
        /* A fresh measured position may be outside the legal target range.
         * An explicit move back inside remains allowed; only the target is
         * range checked here. The sweep separately requires an in-range start. */
        if (!model.armed || !in_limits(req->a)) { note("Target rejected: disarmed or outside conservative travel limits"); return; }
        model.demo_active = false;
        target_epoch = req->epoch;
        label = "Target (no ACK)"; r = kst_servo_set_position(&servo, req->a);
        if (r >= 0) model.target_deci_deg = req->a;
        break;
    case APP_DEMO_START:
        if (!model.armed || req->a < 0 || req->a > 1000 || req->b < 4000 || req->b > 60000 ||
            !in_limits(req->a) || !in_limits(-req->a) || !in_limits(model.position_deci_deg)) {
            note("Sweep rejected: ARM, valid amplitude / travel / 4..60 s required"); return;
        }
        sweep_amplitude = req->a; sweep_duration = req->b;
        sweep_start_position = model.position_deci_deg;
        sweep_start = next_step = now_ms(); model.demo_active = true;
        note("Finite sweep: current > 0 > +A > -A > 0; PAUSE cancels future targets"); return;
    case APP_CENTER:
        if (!motion_ready() || model.position_deci_deg < -100 || model.position_deci_deg > 100) {
            note("Center blocked: fresh feedback within +/-10 degrees required"); return;
        }
        raw_position_valid = model.position_valid = false;
        model.config_dirty = true;
        label = "Set current center"; r = kst_servo_set_center(&servo);
        /* The driver's pre-send drain may have delivered a pre-calibration
         * sample. Invalidate again even if the write ACK was lost. */
        raw_position_valid = model.position_valid = false;
        if (r >= 0) {
            model.config_dirty = true; raw_position_valid = model.position_valid = false;
            /* Re-read physical feedback; do not invent a zero reading. */
            result("Read centered position", kst_servo_read_position(&servo, NULL), false);
        }
        break;
    case APP_REVERSE:
    case APP_UV_ALARM: {
        uint8_t mask = a == APP_REVERSE ? KST_FEATURE_REVERSE : KST_FEATURE_UNDERVOLTAGE;
        model.config_dirty = true;
        label = a == APP_REVERSE ? "Feedback sign (readback)" : "Low voltage reporting (readback)";
        r = kst_servo_set_features(&servo, mask, req->a ? mask : 0);
        if (r >= 0) model.config_dirty = true;
        else model.features_valid = false; /* uncertain partial write: fail closed */
        raw_position_valid = model.position_valid = false;
        break;
    }
    case APP_LIMIT_NEGATIVE:
    case APP_LIMIT_POSITIVE:
        if (req->a < -1000 || req->a > 1000 ||
            (a == APP_LIMIT_NEGATIVE && req->a > 0) ||
            (a == APP_LIMIT_POSITIVE && req->a < 0)) { note("Invalid negative / positive limit"); return; }
        model.config_dirty = model.requires_restart = true;
        if (a == APP_LIMIT_NEGATIVE && req->a > active_negative) active_negative = req->a;
        if (a == APP_LIMIT_POSITIVE && req->a < active_positive) active_positive = req->a;
        label = "Stage limit; SAVE + servo restart";
        r = kst_servo_set_limit(&servo, a == APP_LIMIT_POSITIVE, req->a);
        if (r >= 0) {
            model.config_dirty = model.requires_restart = true;
            /* Only tighten host limits in this run, never presume a staged
             * wider device limit is already active. */
            if (a == APP_LIMIT_NEGATIVE && req->a > active_negative) active_negative = req->a;
            if (a == APP_LIMIT_POSITIVE && req->a < active_positive) active_positive = req->a;
        }
        break;
    case APP_SET_NODE:
        if (req->a < 1 || req->a > 127) { note("Node must be 1..127"); return; }
        model.pending_node_id = req->a; model.config_dirty = model.requires_restart = true;
        label = "Stage node; SAVE + servo restart"; r = kst_servo_set_node_id(&servo, req->a);
        if (r >= 0) { model.pending_node_id = req->a; model.config_dirty = model.requires_restart = true; }
        break;
    case APP_SET_BAUD:
        if (!valid_bitrate(req->a, &baud)) { note("Unsupported bitrate"); return; }
        model.pending_bitrate = req->a; model.config_dirty = model.requires_restart = true;
        label = "Stage bitrate; SAVE + servo restart"; r = kst_servo_set_baudrate(&servo, baud);
        if (r >= 0) { model.pending_bitrate = req->a; model.config_dirty = model.requires_restart = true; }
        break;
    case APP_SAVE:
        label = "SAVE sent; persistence / completion unverified";
        r = kst_servo_save(&servo);
        /* No application ACK or flash completion time exists. Retain dirty. */
        break;
    default: note("Unsupported application action"); return;
    }
    result(label, r, true);
}

static void step_sweep(void)
{
    uint64_t t = now_ms();
    if (!model.demo_active || t < next_step) return;
    if (!model.armed || atomic_load(&cancel_requested) || !motion_ready()) { disarm(); return; }
    uint64_t elapsed = t - sweep_start;
    int target = 0;
    if (elapsed < (uint64_t)sweep_duration) {
        double progress = (double)elapsed * 4.0 / sweep_duration;
        int segment = (int)progress;
        double u = progress - segment;
        double ease = u * u * (3.0 - 2.0 * u);
        const int points[] = { sweep_start_position, 0, sweep_amplitude, -sweep_amplitude, 0 };
        target = (int)(points[segment] + (points[segment + 1] - points[segment]) * ease);
    }
    if (!in_limits(target)) { disarm(); note("Sweep canceled: host travel constraint"); return; }
    target_epoch = atomic_load(&motion_epoch);
    kst_result_t r = kst_servo_set_position(&servo, target);
    result("Sweep target", r, false);
    if (r >= 0) model.target_deci_deg = target;
    next_step = t + 50;
    if (elapsed >= (uint64_t)sweep_duration && r >= 0) {
        model.demo_active = false; note("Sweep targets complete; inspect measured response (servo holds target)");
    }
}

#if CONFIG_KST_DIAGNOSTIC_LOG
/* Passive snapshots only: no extra CAN requests and no change to note()/FUI. */
static void diagnostic_log(void)
{
    ESP_LOGI("kst_diag",
             "KST ready=%u online=%u armed=%u host_node=%u device_node=%u node_v=%u "
             "pos_v=%u pos_ddeg=%d pos_age_ms=%lu status_v=%u current_10ma=%u "
             "temp_c=%d status_age_ms=%lu features_v=%u features_raw=0x%04X "
             "version_v=%u version=%s neg_v=%u neg_ddeg=%d poslim_v=%u poslim_ddeg=%d",
             (unsigned)model.ready, (unsigned)model.online, (unsigned)model.armed,
             (unsigned)model.node_id, (unsigned)model.device_node_id,
             (unsigned)model.node_valid, (unsigned)model.position_valid,
             (int)model.position_deci_deg, (unsigned long)model.position_age_ms,
             (unsigned)model.status_valid, (unsigned)model.current_10ma,
             (int)model.temperature_c, (unsigned long)model.status_age_ms,
             (unsigned)model.features_valid, (unsigned)model.feature_raw,
             (unsigned)model.version_valid, model.version_valid ? model.version : "-",
             (unsigned)model.negative_limit_valid, (int)model.negative_limit,
             (unsigned)model.positive_limit_valid, (int)model.positive_limit);
#if CONFIG_KST_TRANSPORT_USB_GS
    if (bus) {
        kst_usb_gs_stats_t stats;
        kst_usb_gs_get_stats(bus, &stats);
        ESP_LOGI("kst_diag",
                 "USB vidpid=%04X:%04X gen=%lu connected=%u ready=%u bitrate=%lu "
                 "tx_usb=%lu echo=%lu rx=%lu rx_std=%lu dropped=%lu buserr=%lu usb_err=%lu "
                 "busoff=%u status=%s",
                 (unsigned)stats.vid, (unsigned)stats.pid,
                 (unsigned long)stats.generation, (unsigned)stats.connected,
                 (unsigned)stats.ready, (unsigned long)stats.bitrate, (unsigned long)stats.tx_ok,
                 (unsigned long)stats.tx_echoes, (unsigned long)stats.rx_frames,
                 (unsigned long)stats.rx_standard,
                 (unsigned long)stats.rx_dropped, (unsigned long)stats.bus_errors,
                 (unsigned long)stats.usb_errors, (unsigned)stats.bus_off,
                 stats.status);
        ESP_LOGI("kst_diag",
                 "CAN_LAST v=%u gen=%lu id=%08lX flags=%08lX usb_flags=%02X dlc=%u "
                 "data=%02X%02X%02X%02X%02X%02X%02X%02X "
                 "ERR_LAST v=%u gen=%lu id=%08lX data=%02X%02X%02X%02X%02X%02X%02X%02X",
                 (unsigned)stats.last_can_valid, (unsigned long)stats.last_can_generation,
                 (unsigned long)stats.last_can_id, (unsigned long)stats.last_can_flags,
                 (unsigned)stats.last_can_usb_flags, (unsigned)stats.last_can_dlc,
                 (unsigned)stats.last_can_data[0], (unsigned)stats.last_can_data[1],
                 (unsigned)stats.last_can_data[2], (unsigned)stats.last_can_data[3],
                 (unsigned)stats.last_can_data[4], (unsigned)stats.last_can_data[5],
                 (unsigned)stats.last_can_data[6], (unsigned)stats.last_can_data[7],
                 (unsigned)stats.last_error_valid, (unsigned long)stats.last_error_generation,
                 (unsigned long)stats.last_error_id,
                 (unsigned)stats.last_error_data[0], (unsigned)stats.last_error_data[1],
                 (unsigned)stats.last_error_data[2], (unsigned)stats.last_error_data[3],
                 (unsigned)stats.last_error_data[4], (unsigned)stats.last_error_data[5],
                 (unsigned)stats.last_error_data[6], (unsigned)stats.last_error_data[7]);
    }
#endif
}
#endif

static void controller_task(void *arg)
{
    (void)arg;
    connect_host(CONFIG_KST_NODE_ID, CONFIG_KST_BITRATE);
    uint64_t next_position = 0, next_status = 0, next_features = 0;
#if CONFIG_KST_DIAGNOSTIC_LOG
    uint64_t next_diagnostic = now_ms() + 2000;
#endif
    for (;;) {
        cancel_if_requested();
#if CONFIG_KST_TRANSPORT_USB_GS
        service_transport();
#endif
        request_t req;
        if (xQueueReceive(requests, &req, 0) == pdTRUE) {
            model.busy = true; publish(); handle(&req); model.busy = false;
        }
        cancel_if_requested();
        if (model.ready) {
            send_connection_epoch = atomic_load(&connection_epoch);
            /* Bound drain work even at the shortest 10 ms report period. Empty
             * nonblocking polls are not transaction timeout alarms. */
            for (int i = 0; i < 8; ++i) {
                kst_result_t r = kst_servo_poll(&servo, 0, NULL);
                if (r == KST_SERVO_ERR_TIMEOUT) break;
                if (r < 0) { result("Receive", r, false); break; }
            }
            uint64_t t = now_ms();
            if (t >= next_features && !model.demo_active) {
                kst_result_t r = kst_servo_read_features(&servo, NULL);
                if (r < 0) model.features_valid = false;
                result("Read features", r, false); next_features = now_ms() + 2000;
            }
            if (model.ready && t >= next_position) {
                if (!model.position_valid || now_ms() - position_time >= 100)
                    result("Read position", kst_servo_read_position(&servo, NULL), false);
                next_position = now_ms() + 100;
            }
            if (model.ready && t >= next_status) {
                result("Read status", kst_servo_read_status(&servo, NULL), false);
                next_status = now_ms() + 250;
            }
        }
        cancel_if_requested();
        step_startup_metadata_recovery();
        if (model.armed && !motion_ready()) { disarm(); note("Disarmed: stale telemetry or CAN / servo fault"); }
        step_sweep(); publish();
#if CONFIG_KST_DIAGNOSTIC_LOG
        uint64_t diagnostic_time = now_ms();
        if (diagnostic_time >= next_diagnostic) {
            next_diagnostic = diagnostic_time + 2000;
            diagnostic_log();
        }
#endif
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

int app_controller_start(void)
{
    if (requests) return ESP_ERR_INVALID_STATE;
    model.node_id = CONFIG_KST_NODE_ID; model.bitrate = CONFIG_KST_BITRATE;
#if CONFIG_KST_TRANSPORT_USB_GS
    model.usb_transport = true;
    snprintf(model.transport_status, sizeof(model.transport_status), "USB Host starting");
#endif
    model.report_interval_ms = 50; /* Documented factory default, not a readback. */
    note("KST CAN V3.61B / hardware readings pending"); publish();
    requests = xQueueCreate(16, sizeof(request_t));
    if (!requests) return ESP_ERR_NO_MEM;
    if (xTaskCreate(controller_task, "kst_can", 8192, NULL, 5, NULL) != pdPASS) {
        vQueueDelete(requests); requests = NULL; return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
