/* Compile the real controller with tiny synchronous ESP/FreeRTOS shims.
 * The real protocol and synchronous driver remain linked into this test.
 * No hardware, task, thread, SDL window, socket or wall-clock sleeps are used.
 */
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include "sdkconfig.h"
#if CONFIG_KST_TRANSPORT_USB_GS
#include "kst_usb_gs.h"
#ifndef CONFIG_KST_DISCOVER_ON_START
#define CONFIG_KST_DISCOVER_ON_START 1
#endif
#ifndef CONFIG_KST_PROBE_360_ON_START
#define CONFIG_KST_PROBE_360_ON_START 1
#endif
#else
#include "kst_twai.h"
#endif
#include "freertos/queue.h"
#include "freertos/task.h"
#include "esp_log.h"
#undef ESP_LOGI
#define ESP_LOGI(tag, ...) fake_log(tag, __VA_ARGS__)

static unsigned broadcast_raw_logs, broadcast_found_logs, broadcast_out_of_range_logs;
static void fake_log(const char *tag, const char *format, ...)
    __attribute__((format(printf, 2, 3)));
static void fake_log(const char *tag, const char *format, ...)
{
    char message[384];
    va_list args;
    va_start(args, format);
    vsnprintf(message, sizeof(message), format, args);
    va_end(args);
    if (strcmp(tag, "kst_discover") != 0) return;
    if (strstr(message, "BCAST_RAW ")) {
        ++broadcast_raw_logs;
        if (strstr(message, "id=00000580 ") || strstr(message, "id=00000600 "))
            ++broadcast_out_of_range_logs;
    }
    if (strstr(message, "BCAST_FOUND ")) ++broadcast_found_logs;
}

static unsigned checks;
#define CHECK(expression) do { \
    ++checks; \
    if (!(expression)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #expression); \
        exit(EXIT_FAILURE); \
    } \
} while (0)

struct fake_queue {
    unsigned char items[16][64];
    unsigned size, count, head;
};
static struct fake_queue action_queue;
#if CONFIG_KST_TRANSPORT_USB_GS
struct kst_usb_gs_bus { int dummy; };
static struct kst_usb_gs_bus fake_bus;
static bool fake_usb_connected, fake_usb_ready, inject_disconnect;
static uint32_t fake_generation;
static kst_usb_gs_tx_guard_t fake_tx_guard;
static void *fake_guard_ctx;
static bool inject_late_pause, inject_late_rx;
static unsigned fake_discovery_tx, fake_discovery_reply_node;
static unsigned fake_discovery_cancel_node, fake_discovery_generation_node;
static bool fake_discovery_nodes[128];
static unsigned fake_broadcast_tx;
static bool fake_broadcast_replies, fake_broadcast_cancel, fake_broadcast_generation;
#else
struct kst_twai_bus { int dummy; };
static struct kst_twai_bus fake_bus;
#endif
static uint64_t fake_time;
static kst_can_frame_t rx_frames[64];
static unsigned rx_head, rx_count, motion_sends, all_sends;
static bool auto_reply, inject_pause, fake_bus_off;
static uint8_t fake_feature_bits;
static uint8_t fake_feature_high;
static int16_t fake_position;
static int16_t fake_negative_limit, fake_positive_limit;
static char fake_version[5];

#if CONFIG_KST_TRANSPORT_USB_GS && CONFIG_KST_PROBE_360_ON_START
/* Deterministic scheduler pauses at the existing cancellation atomics. The
 * production controller needs no test hooks and no real worker/thread starts. */
#include <stdatomic.h>
static unsigned fake_atomic_fetch_add(atomic_uint *object, unsigned value);
static void fake_atomic_store_uint(atomic_uint *object, unsigned value);
static void fake_atomic_store_bool(atomic_bool *object, bool value);
#undef atomic_fetch_add
#define atomic_fetch_add(object, value) fake_atomic_fetch_add(object, value)
#undef atomic_store
#define atomic_store(object, value) _Generic((object), \
    atomic_uint *: fake_atomic_store_uint, atomic_bool *: fake_atomic_store_bool)(object, value)
#endif

#include "../main/app_controller.c"

static unsigned metadata_read_sends[METADATA_COUNT], fake_metadata_drop_mask;
static void queue_rx(uint32_t id, uint8_t len, const uint8_t data[8]);
#if CONFIG_KST_TRANSPORT_USB_GS && CONFIG_KST_PROBE_360_ON_START
enum { PROBE_ACCEPT, PROBE_REJECT_POSITIVE, PROBE_WRITE_TIMEOUT, PROBE_RESTORE_ABORT,
       PROBE_RESTORE_TIMEOUT, PROBE_DISCONNECT, PROBE_CANCEL, PROBE_UNCHANGED, PROBE_READ_TIMEOUT,
       PROBE_REJECT_READ_TIMEOUT };
static unsigned fake_probe_mode, fake_probe_candidates[2], fake_probe_restores[2], fake_probe_tx;
static unsigned fake_submit_pause_phase;
static void fake_submit_resume_via_probe(void)
{
    fake_submit_pause_phase = 0;
    cancel_if_requested();
    run_startup_limit_probe();
}
static unsigned fake_atomic_fetch_add(atomic_uint *object, unsigned value)
{
    const unsigned old = atomic_fetch_add_explicit(object, value, memory_order_seq_cst);
    if (object == &motion_epoch && fake_submit_pause_phase == 1) fake_submit_resume_via_probe();
    return old;
}
static void fake_atomic_store_uint(atomic_uint *object, unsigned value)
{ atomic_store_explicit(object, value, memory_order_seq_cst); }
static void fake_atomic_store_bool(atomic_bool *object, bool value)
{
    atomic_store_explicit(object, value, memory_order_seq_cst);
    if (object == &cancel_requested && value && fake_submit_pause_phase == 2)
        fake_submit_resume_via_probe();
}

static int fake_limit_probe_send(const kst_can_frame_t *frame)
{
    CHECK(frame->id == 0x605 && frame->len == 8);
    CHECK(frame->data[2] == 0x30 && frame->data[3] == 0);
    CHECK(frame->data[1] == 0x0a || frame->data[1] == 0x0b);
    CHECK(frame->data[0] == 0x22 || frame->data[0] == 0x40);
    ++fake_probe_tx;
    const unsigned side = frame->data[1] == 0x0b ? 0 : 1;
    int16_t *limit = side == 0 ? &fake_positive_limit : &fake_negative_limit;
    uint8_t reply[8] = {0, frame->data[1], frame->data[2], 0, 0, 0, 0, 0};
    if (frame->data[0] == 0x40) {
        CHECK(!frame->data[4] && !frame->data[5] && !frame->data[6] && !frame->data[7]);
        if (fake_probe_mode == PROBE_READ_TIMEOUT) return 0;
        if (fake_probe_mode == PROBE_REJECT_READ_TIMEOUT && fake_probe_candidates[0]) return 0;
        reply[0] = 0x4b; reply[4] = (uint8_t)(uint16_t)*limit;
        reply[5] = (uint8_t)((uint16_t)*limit >> 8);
        if (side) reply[6] = reply[7] = 0xff;
    } else {
        CHECK(!frame->data[6] && !frame->data[7]);
        const int16_t value = (int16_t)((uint16_t)frame->data[4] | ((uint16_t)frame->data[5] << 8));
        const bool candidate = value == (side ? -1800 : 1800);
        if (candidate) {
            CHECK(fake_probe_candidates[side]++ == 0);
            if (side) CHECK(fake_probe_restores[0] == 1 && fake_positive_limit == 1000);
            if (fake_probe_mode == PROBE_REJECT_POSITIVE || fake_probe_mode == PROBE_REJECT_READ_TIMEOUT) {
                CHECK(side == 0);
                reply[0] = 0x80; reply[4] = 0x31; reply[6] = 9; reply[7] = 6;
                queue_rx(0x585, 8, reply); return 0;
            }
            if (fake_probe_mode != PROBE_UNCHANGED) *limit = value;
            if (fake_probe_mode == PROBE_WRITE_TIMEOUT) return 0;
            if (fake_probe_mode == PROBE_DISCONNECT) {
                fake_usb_connected = fake_usb_ready = false; ++fake_generation; return 0;
            }
            if (fake_probe_mode == PROBE_CANCEL) {
                CHECK(!app_submit(APP_LIMIT_POSITIVE, 800, 0));
                CHECK(!app_submit(APP_SAVE, 0, 0));
                CHECK(app_submit(APP_STOP_MOTION, 0, 0));
            }
        } else {
            CHECK(value == (side ? -1000 : 1000));
            CHECK(fake_probe_restores[side]++ == 0);
            if (fake_probe_mode == PROBE_RESTORE_ABORT) {
                reply[0] = 0x80; reply[4] = 0x20; reply[7] = 8;
                queue_rx(0x585, 8, reply); return 0;
            }
            *limit = value;
            if (fake_probe_mode == PROBE_RESTORE_TIMEOUT) return 0;
        }
        reply[0] = 0x60;
    }
    queue_rx(0x585, 8, reply);
    return 0;
}
#endif

int64_t esp_timer_get_time(void) { return (int64_t)(fake_time * 1000); }

QueueHandle_t xQueueCreate(UBaseType_t count, UBaseType_t item_size)
{
    CHECK(count <= 16 && item_size <= 64);
    memset(&action_queue, 0, sizeof(action_queue));
    action_queue.size = item_size;
    return &action_queue;
}
BaseType_t xQueueSend(QueueHandle_t queue, const void *item, TickType_t wait)
{
    (void)wait;
    if (queue->count >= 16) return pdFALSE;
    memcpy(queue->items[(queue->head + queue->count) % 16], item, queue->size);
    ++queue->count;
    return pdTRUE;
}
BaseType_t xQueueReceive(QueueHandle_t queue, void *item, TickType_t wait)
{
    (void)wait;
    if (queue->count == 0) return pdFALSE;
    memcpy(item, queue->items[queue->head], queue->size);
    queue->head = (queue->head + 1) % 16;
    --queue->count;
    return pdTRUE;
}
void vQueueDelete(QueueHandle_t queue) { (void)queue; }
BaseType_t xTaskCreate(TaskFunction_t function, const char *name, uint32_t stack,
                       void *argument, UBaseType_t priority, void *handle)
{
    (void)function; (void)name; (void)stack; (void)argument; (void)priority; (void)handle;
    return pdPASS; /* Never starts the infinite controller loop. */
}
void vTaskDelay(TickType_t ticks) { fake_time += ticks; }

static void queue_rx(uint32_t id, uint8_t len, const uint8_t data[8])
{
    CHECK(rx_count < 64);
    kst_can_frame_t *frame = &rx_frames[(rx_head + rx_count) % 64];
    frame->id = id; frame->len = len;
    memcpy(frame->data, data, 8);
    ++rx_count;
}

static int fake_send(void *ctx, const kst_can_frame_t *frame, uint32_t timeout_ms)
{
    (void)ctx; (void)timeout_ms;
#if CONFIG_KST_TRANSPORT_USB_GS
    if (inject_late_pause) {
        inject_late_pause = false;
        CHECK(app_submit(APP_STOP_MOTION, 0, 0));
    }
    if (fake_tx_guard && !fake_tx_guard(fake_guard_ctx, frame, inject_late_rx)) return -1;
#endif
    ++all_sends;
#if CONFIG_KST_TRANSPORT_USB_GS && CONFIG_KST_PROBE_360_ON_START
    if (limit_probe_running) return fake_limit_probe_send(frame);
#endif
    if (frame->len != 8) return 0;
    const uint16_t index = (uint16_t)((uint16_t)frame->data[1] | ((uint16_t)frame->data[2] << 8));
    if (index == KST_INDEX_POSITION_TARGET) { ++motion_sends; return 0; }
    if (frame->data[0] == 0x40) {
        unsigned item = METADATA_COUNT;
        if (index == KST_INDEX_LIMIT_NEGATIVE) item = METADATA_NEGATIVE;
        if (index == KST_INDEX_LIMIT_POSITIVE) item = METADATA_POSITIVE;
        if (index == KST_INDEX_NODE_ID) item = METADATA_NODE;
        if (index == KST_INDEX_VERSION) item = METADATA_VERSION;
        if (item < METADATA_COUNT) {
            ++metadata_read_sends[item];
            if (fake_metadata_drop_mask & (1U << item)) return 0;
        }
    }
#if CONFIG_KST_TRANSPORT_USB_GS && CONFIG_KST_DISCOVER_ON_START
    if (discovery_running) {
        if (frame->id == 0x600) {
            static const uint8_t expected[8] = {0x40, 0, 0x30, 0, 0, 0, 0, 0};
            CHECK(memcmp(frame->data, expected, sizeof(expected)) == 0);
            CHECK(fake_broadcast_tx == 0 && fake_discovery_tx == 0);
            ++fake_broadcast_tx;
            if (fake_broadcast_cancel) CHECK(app_submit(APP_CONNECT, 42, 500000));
            if (fake_broadcast_generation) ++fake_generation;
            if (fake_broadcast_replies) {
                const uint8_t reply[8] = {0x4F, 0, 0x30, 0, 17, 0, 0, 0};
                queue_rx(0x591, 8, reply);
                queue_rx(0x591, 8, reply); /* duplicate must not increase found */
                queue_rx(0x580, 8, reply); /* outside supported node range: raw only */
                queue_rx(0x600, 8, reply);
                const uint8_t invalid[8] = {0x4F, 0, 0x30, 1, 18, 0, 0, 0};
                queue_rx(0x592, 8, invalid); /* invalid subindex must not count */
            }
            return 0;
        }
        CHECK(frame->id >= 0x601 && frame->id <= 0x67f);
        CHECK(frame->data[0] == 0x40 && index == KST_INDEX_VERSION && frame->data[3] == 0);
        unsigned node = frame->id - 0x600;
        CHECK(!fake_discovery_nodes[node]);
        fake_discovery_nodes[node] = true;
        ++fake_discovery_tx;
        if (node == fake_discovery_cancel_node)
            CHECK(app_submit(APP_CONNECT, 42, 500000)); /* queued config cancels scan */
        if (node == fake_discovery_generation_node) ++fake_generation;
        if (node == fake_discovery_reply_node) {
            const uint8_t reply[8] = {0x43, frame->data[1], frame->data[2], 0, '3', '.', '6', '1'};
            queue_rx(frame->id - 0x80, 8, reply);
        }
        return 0;
    }
#endif
    if (!auto_reply || index == KST_INDEX_SAVE) return 0;
    uint8_t reply[8] = {0, frame->data[1], frame->data[2], frame->data[3], 0, 0, 0, 0};
    if (frame->data[0] == 0x22) {
        reply[0] = 0x60;
        if (index == KST_INDEX_FEATURES) fake_feature_bits = frame->data[4];
    } else if (frame->data[0] == 0x40) {
        switch (index) {
        case KST_INDEX_FEATURES:
            reply[0] = 0x4B; reply[4] = fake_feature_bits; reply[5] = fake_feature_high; break;
        case KST_INDEX_POSITION:
            reply[0] = 0x4B; reply[4] = (uint8_t)fake_position;
            reply[5] = (uint8_t)((uint16_t)fake_position >> 8); break;
        case KST_INDEX_STATUS: reply[0] = 0x43; reply[4] = 50; reply[6] = 25; break;
        case KST_INDEX_LIMIT_NEGATIVE:
            reply[0] = 0x4B; reply[4] = (uint8_t)fake_negative_limit;
            reply[5] = (uint8_t)((uint16_t)fake_negative_limit >> 8); break;
        case KST_INDEX_LIMIT_POSITIVE:
            reply[0] = 0x4B; reply[4] = (uint8_t)fake_positive_limit;
            reply[5] = (uint8_t)((uint16_t)fake_positive_limit >> 8); break;
        case KST_INDEX_NODE_ID: reply[0] = 0x4F; reply[4] = (uint8_t)(frame->id - 0x600); break;
        case KST_INDEX_VERSION: reply[0] = 0x43; memcpy(reply + 4, fake_version, 4); break;
        default: CHECK(false);
        }
    } else CHECK(false);
    queue_rx(frame->id - 0x80, 8, reply);
    return 0;
}

static int fake_receive(void *ctx, kst_can_frame_t *frame, bool *extended,
                         bool *rtr, bool *fd, uint32_t timeout_ms)
{
    (void)ctx;
#if CONFIG_KST_TRANSPORT_USB_GS
    if (inject_disconnect) {
        inject_disconnect = false;
        fake_usb_connected = fake_usb_ready = false;
        ++fake_generation;
    }
#endif
    if (inject_pause) {
        inject_pause = false;
        CHECK(app_submit(APP_STOP_MOTION, 0, 0));
    }
    if (rx_count == 0) { fake_time += timeout_ms; return 0; }
    *frame = rx_frames[rx_head]; rx_head = (rx_head + 1) % 64; --rx_count;
    *extended = true; *rtr = false; *fd = false;
    return 1;
}
static uint64_t fake_clock(void *ctx) { (void)ctx; return fake_time; }

#if CONFIG_KST_TRANSPORT_USB_GS
esp_err_t kst_usb_gs_open(uint32_t bitrate, kst_usb_gs_bus_t **out)
{ (void)bitrate; *out = &fake_bus; return ESP_OK; }
esp_err_t kst_usb_gs_reopen(kst_usb_gs_bus_t *target_bus, uint32_t bitrate)
{ (void)target_bus; (void)bitrate; ++fake_generation; rx_count = 0; return ESP_OK; }
esp_err_t kst_usb_gs_close(kst_usb_gs_bus_t *target_bus)
{ (void)target_bus; return ESP_OK; }
esp_err_t kst_usb_gs_service(kst_usb_gs_bus_t *target_bus, uint32_t budget)
{ (void)target_bus; (void)budget; return ESP_OK; }
esp_err_t kst_usb_gs_set_tx_guard(kst_usb_gs_bus_t *target_bus, kst_usb_gs_tx_guard_t guard, void *ctx)
{ (void)target_bus; fake_tx_guard = guard; fake_guard_ctx = ctx; return ESP_OK; }
kst_transport_t kst_usb_gs_transport(kst_usb_gs_bus_t *target_bus)
{
    (void)target_bus;
    return (kst_transport_t){.send = fake_send, .receive = fake_receive, .now_ms = fake_clock};
}
void kst_usb_gs_get_stats(kst_usb_gs_bus_t *target_bus, kst_usb_gs_stats_t *out)
{
    (void)target_bus;
    *out = (kst_usb_gs_stats_t){.tx_ok = all_sends, .bus_off = fake_bus_off,
        .connected = fake_usb_connected, .ready = fake_usb_ready && !fake_bus_off,
        .generation = fake_generation, .vid = 0x1d50, .pid = 0x606f};
}
#else
esp_err_t kst_twai_open(int tx, int rx, uint32_t bitrate, kst_twai_bus_t **out)
{ (void)tx; (void)rx; (void)bitrate; *out = &fake_bus; return ESP_OK; }
esp_err_t kst_twai_reopen(kst_twai_bus_t *target_bus, uint32_t bitrate)
{ (void)target_bus; (void)bitrate; return ESP_OK; }
esp_err_t kst_twai_close(kst_twai_bus_t *target_bus) { (void)target_bus; return ESP_OK; }
kst_transport_t kst_twai_transport(kst_twai_bus_t *target_bus)
{
    (void)target_bus;
    return (kst_transport_t){.send = fake_send, .receive = fake_receive, .now_ms = fake_clock};
}
void kst_twai_get_stats(kst_twai_bus_t *target_bus, kst_twai_stats_t *out)
{
    (void)target_bus;
    *out = (kst_twai_stats_t){.tx_ok = all_sends, .bus_off = fake_bus_off};
}
#endif

static void reset_ready(void)
{
    memset(&model, 0, sizeof(model));
    memset(&published, 0, sizeof(published));
    memset(&servo, 0, sizeof(servo));
    broadcast_raw_logs = broadcast_found_logs = broadcast_out_of_range_logs = 0;
    bus = NULL;
    atomic_store(&motion_epoch, 0); atomic_store(&cancel_requested, false);
    atomic_store(&connection_epoch, 0); send_connection_epoch = 0;
#if CONFIG_KST_TRANSPORT_USB_GS
    link_seen = link_needs_refresh = false; link_generation = 0;
    fake_generation = 1; fake_usb_connected = fake_usb_ready = true; inject_disconnect = false;
    fake_tx_guard = NULL; fake_guard_ctx = NULL; inject_late_pause = inject_late_rx = false;
    fake_discovery_tx = fake_discovery_reply_node = 0;
    fake_discovery_cancel_node = fake_discovery_generation_node = 0;
    memset(fake_discovery_nodes, 0, sizeof(fake_discovery_nodes));
    fake_broadcast_tx = 0;
    fake_broadcast_replies = fake_broadcast_cancel = fake_broadcast_generation = false;
#if CONFIG_KST_DISCOVER_ON_START
    startup_discovery_done = discovery_running = false;
    discovery_queue_cutoff_valid = false;
    discovery_queue_cutoff = 0;
#endif
#if CONFIG_KST_PROBE_360_ON_START
    limit_probe_done = limit_probe_running = limit_probe_restoring = limit_probe_queue_cutoff_valid = false;
    atomic_store(&limit_probe_active, false);
    fake_submit_pause_phase = 0;
    limit_probe_generation = limit_probe_connection_epoch = limit_probe_queue_cutoff = 0;
    fake_probe_mode = PROBE_ACCEPT; fake_probe_tx = 0;
    memset(fake_probe_candidates, 0, sizeof(fake_probe_candidates));
    memset(fake_probe_restores, 0, sizeof(fake_probe_restores));
#endif
#endif
    position_time = status_time = last_event_time = 0;
    raw_position = sweep_start_position = 0;
    active_negative = -1000; active_positive = 1000; raw_position_valid = false;
    sweep_start = next_step = 0; sweep_amplitude = sweep_duration = 0;
    fake_time = 1000; rx_head = rx_count = motion_sends = all_sends = 0;
    auto_reply = true; inject_pause = fake_bus_off = false;
    fake_feature_bits = fake_feature_high = 0; fake_position = 100;
    fake_negative_limit = -1000; fake_positive_limit = 1000;
    memcpy(fake_version, "3.61", sizeof(fake_version));
    memset(metadata_read_sends, 0, sizeof(metadata_read_sends));
    fake_metadata_drop_mask = 0;
    requests = xQueueCreate(16, sizeof(request_t));
    connect_host(0x25, 500000);
    CHECK(motion_ready());
    CHECK(!model.armed && motion_sends == 0);
}

static void execute(app_action_t action, int a, int b)
{
    request_t req = {action, a, b, atomic_load(&motion_epoch), atomic_load(&connection_epoch)};
    handle(&req);
}
static void consume_one(void)
{
    request_t req;
    CHECK(xQueueReceive(requests, &req, 0) == pdTRUE);
    handle(&req);
}

static void test_pause_epoch_and_full_queue(void)
{
    reset_ready();
    CHECK(app_submit(APP_ARM, 1, 0));
    CHECK(app_submit(APP_MOVE, 50, 0));
    CHECK(app_submit(APP_STOP_MOTION, 0, 0));
    cancel_if_requested(); consume_one(); consume_one();
    CHECK(!model.armed && motion_sends == 0);
    CHECK(app_submit(APP_ARM, 1, 0)); consume_one();
    CHECK(model.armed);
    CHECK(app_submit(APP_MOVE, 50, 0)); consume_one();
    CHECK(motion_sends == 1);

    for (unsigned i = 0; i < 16; ++i) CHECK(app_submit(APP_MOVE, 100, 0));
    CHECK(!app_submit(APP_MOVE, 100, 0));
    CHECK(app_submit(APP_STOP_MOTION, 0, 0));
    cancel_if_requested();
    for (unsigned i = 0; i < 16; ++i) consume_one();
    CHECK(!model.armed && motion_sends == 1);
}

static void test_faults_discovered_during_pre_send_drain(void)
{
    reset_ready(); execute(APP_ARM, 1, 0);
    queue_rx(0x3A5, 4, (const uint8_t[8]){0x0A, 0x02, 0, 0, 0, 0, 0, 0});
    execute(APP_MOVE, 200, 0);
    CHECK(model.low_voltage_latched && !model.armed && motion_sends == 0);
    CHECK(model.ready); /* deliberate send denial must not destroy CAN state */

    reset_ready(); execute(APP_ARM, 1, 0);
    queue_rx(0x5A5, 8, (const uint8_t[8]){0x80, 0x03, 0x60, 0, 0x30, 0, 0x09, 0x06});
    execute(APP_MOVE, 200, 0);
    CHECK(model.abort_code == 0x06090030 && !model.armed && motion_sends == 0);
    CHECK(model.ready);

    reset_ready(); execute(APP_ARM, 1, 0);
    inject_pause = true;
    execute(APP_MOVE, 200, 0);
    CHECK(!model.armed && motion_sends == 0 && model.ready);
}

static void test_stale_fault_latches_and_bus_off(void)
{
    reset_ready(); execute(APP_ARM, 1, 0);
    fake_time += CONFIG_KST_STALE_MS + 1;
    execute(APP_MOVE, 100, 0);
    CHECK(!model.armed && motion_sends == 0);

    reset_ready(); execute(APP_ARM, 1, 0);
    kst_event_t uv = {.type = KST_EVENT_UNDERVOLTAGE, .node = 0x25};
    on_event(NULL, &uv);
    connect_host(0x25, 500000);
    CHECK(model.low_voltage_latched && !motion_ready());
    execute(APP_CLEAR_ALERTS, 0, 0);
    CHECK(!model.low_voltage_latched && !model.armed && motion_ready());

    reset_ready(); execute(APP_ARM, 1, 0);
    fake_bus_off = true;
    execute(APP_MOVE, 100, 0);
    CHECK(!model.armed && motion_sends == 0);
}

static void test_conservative_staged_limits_and_save(void)
{
    reset_ready();
    execute(APP_LIMIT_POSITIVE, 200, 0);
    CHECK(active_positive == 200 && model.requires_restart && model.config_dirty);
    execute(APP_LIMIT_POSITIVE, 800, 0);
    CHECK(active_positive == 200); /* wider staged range never relaxes host */
    publish();
    CHECK(model.host_negative_limit == -1000 && model.host_positive_limit == 200);
    execute(APP_ARM, 1, 0); execute(APP_MOVE, 201, 0);
    CHECK(motion_sends == 0);
    execute(APP_MOVE, 200, 0);
    CHECK(motion_sends == 1);
    execute(APP_SAVE, 0, 0);
    CHECK(model.config_dirty && model.requires_restart && !model.armed);
    execute(APP_SET_NODE, 0x26, 0);
    CHECK(model.pending_node_id == 0x26 && model.node_id == 0x25);
    execute(APP_SET_BAUD, 250000, 0);
    CHECK(model.pending_bitrate == 250000 && model.bitrate == 500000);
}

static void test_feedback_sign_requires_new_position_sample(void)
{
    reset_ready();
    CHECK(model.position_deci_deg == 100 && model.position_valid);
    const kst_event_t changed = {.type = KST_EVENT_FEATURES, .node = 0x25,
                                 .feature_bits = KST_FEATURE_REVERSE};
    on_event(NULL, &changed);
    CHECK(!model.position_valid); /* cached raw angle belongs to old sign convention */
    const kst_event_t position = {.type = KST_EVENT_POSITION, .node = 0x25,
                                  .position_deci_deg = -100};
    on_event(NULL, &position);
    CHECK(model.position_valid && model.position_deci_deg == 100);
}

static void test_reports_do_not_guess_reverse_sign_semantics(void)
{
    reset_ready();
    kst_event_t report = {.type = KST_EVENT_POSITION_REPORT, .node = 0x25,
                          .position_deci_deg = 150};
    fake_time += 10;
    on_event(NULL, &report);
    CHECK(model.position_valid && model.position_deci_deg == 150);
    CHECK(position_time == fake_time); /* confirmed REV=0 accepts reports */

    const uint64_t previous_sample_time = position_time;
    model.features_valid = false;
    raw_position_valid = model.position_valid = false;
    fake_time += 10;
    on_event(NULL, &report);
    CHECK(!model.position_valid && !raw_position_valid);
    CHECK(position_time == previous_sample_time && last_event_time == fake_time);

    const kst_event_t reversed = {.type = KST_EVENT_FEATURES, .node = 0x25,
                                  .feature_bits = KST_FEATURE_REVERSE};
    on_event(NULL, &reversed);
    fake_time += 10;
    on_event(NULL, &report);
    CHECK(!model.position_valid && !raw_position_valid);
    CHECK(position_time == previous_sample_time && last_event_time == fake_time);
    const kst_event_t read_position = {.type = KST_EVENT_POSITION, .node = 0x25,
                                       .position_deci_deg = -100};
    on_event(NULL, &read_position);
    CHECK(model.position_valid && model.position_deci_deg == 100);
    const uint64_t explicit_sample_time = position_time;
    fake_time += 10;
    report.position_deci_deg = -800;
    on_event(NULL, &report);
    CHECK(model.position_deci_deg == 100 && position_time == explicit_sample_time);
}

static void test_center_timeout_invalidates_old_coordinates(void)
{
    reset_ready();
    auto_reply = false; /* Device may apply centre even when its ACK is lost. */
    execute(APP_CENTER, 0, 0);
    CHECK(!model.position_valid && !raw_position_valid);
    execute(APP_ARM, 1, 0);
    CHECK(!model.armed);

    reset_ready();
    auto_reply = false;
    /* A pre-command queued position report must not revalidate coordinates
     * after a centre command whose ACK is lost. */
    queue_rx(0x5A5, 8, (const uint8_t[8]){0x4B, 0x02, 0x60, 0, 100, 0, 0, 0});
    execute(APP_CENTER, 0, 0);
    CHECK(!model.position_valid && !raw_position_valid && model.config_dirty);
    execute(APP_ARM, 1, 0);
    CHECK(!model.armed);
}

static void test_lost_configuration_ack_stays_conservative(void)
{
    reset_ready(); auto_reply = false;
    execute(APP_LIMIT_POSITIVE, 100, 0);
    CHECK(active_positive == 100 && model.config_dirty && model.requires_restart);
    CHECK(model.timeouts == 1 && !model.armed);
    execute(APP_LIMIT_POSITIVE, 800, 0);
    CHECK(active_positive == 100); /* timeout cannot widen the host envelope */

    reset_ready(); auto_reply = false;
    execute(APP_LIMIT_NEGATIVE, -100, 0);
    CHECK(active_negative == -100 && model.config_dirty && model.requires_restart);

    reset_ready(); auto_reply = false;
    execute(APP_SET_NODE, 0x26, 0);
    CHECK(model.pending_node_id == 0x26 && model.node_id == 0x25);
    CHECK(model.config_dirty && model.requires_restart);

    reset_ready(); auto_reply = false;
    execute(APP_SET_BAUD, 250000, 0);
    CHECK(model.pending_bitrate == 250000 && model.bitrate == 500000);
    CHECK(model.config_dirty && model.requires_restart);

    reset_ready(); auto_reply = false;
    execute(APP_REPORT_INTERVAL, 100, 0);
    CHECK(model.config_dirty && model.timeouts == 1);

    reset_ready();
    const unsigned before = all_sends;
    execute(APP_LIMIT_NEGATIVE, 100, 0);
    execute(APP_LIMIT_POSITIVE, -100, 0);
    execute(APP_SET_NODE, 0, 0);
    execute(APP_SET_BAUD, 123456, 0);
    execute(APP_REPORT_INTERVAL, 9, 0);
    CHECK(all_sends == before && !model.config_dirty && !model.requires_restart);
}

static void test_sweep_final_gate(void)
{
    reset_ready(); execute(APP_ARM, 1, 0); execute(APP_DEMO_START, 100, 4000);
    CHECK(model.demo_active);
    queue_rx(0x3A5, 4, (const uint8_t[8]){0x0A, 0x02, 0, 0, 0, 0, 0, 0});
    step_sweep();
    CHECK(!model.demo_active && !model.armed && motion_sends == 0 && model.ready);
}

static void prepare_delayed_startup_reply(void)
{
    reset_ready();
    invalidate_readback();
    auto_reply = false;
    refresh_all(false); /* All seven initial reads time out. */
    memset(metadata_read_sends, 0, sizeof(metadata_read_sends));
    const unsigned sends = all_sends;
    step_startup_metadata_recovery();
    CHECK(all_sends == sends && metadata_recovery.pending && !last_event_time);
    fake_time = 29000;
    auto_reply = true;
    execute(APP_READ_STATUS, 0, 0); /* First real reply arrives long after startup. */
    CHECK(model.status_valid && !model.node_valid && !model.version_valid);
}

static void test_delayed_startup_metadata_recovers_one_read_at_a_time(void)
{
    prepare_delayed_startup_reply();
    for (unsigned i = 0; i < METADATA_COUNT; ++i) {
        const unsigned before = all_sends;
        step_startup_metadata_recovery();
        CHECK(all_sends == before + 1 && metadata_read_sends[i] == 1);
        step_startup_metadata_recovery();
        CHECK(all_sends == before + 1); /* No second read before the interval. */
        fake_time += METADATA_RECOVERY_INTERVAL_MS;
        execute(APP_READ_STATUS, 0, 0);
    }
    step_startup_metadata_recovery();
    CHECK(!metadata_recovery.pending);
    CHECK(model.negative_limit_valid && model.positive_limit_valid && model.node_valid && model.version_valid);
    CHECK(!model.armed && !model.config_dirty && !model.requires_restart && motion_sends == 0);
    CHECK(servo.node == 0x25 && model.node_id == 0x25);
}

static void test_startup_metadata_retry_limit_and_missing_item_skip(void)
{
    prepare_delayed_startup_reply();
    fake_metadata_drop_mask = (1U << METADATA_COUNT) - 1U;
    for (unsigned i = 0; i < METADATA_COUNT * METADATA_RECOVERY_ATTEMPTS; ++i) {
        const unsigned before = all_sends;
        step_startup_metadata_recovery();
        CHECK(all_sends == before + 1);
        fake_time += METADATA_RECOVERY_INTERVAL_MS;
        execute(APP_READ_STATUS, 0, 0);
    }
    step_startup_metadata_recovery();
    CHECK(!metadata_recovery.pending);
    for (unsigned i = 0; i < METADATA_COUNT; ++i)
        CHECK(metadata_read_sends[i] == METADATA_RECOVERY_ATTEMPTS);
    const unsigned exhausted = all_sends;
    fake_time += 10000;
    step_startup_metadata_recovery();
    CHECK(all_sends == exhausted && !model.armed && motion_sends == 0);

    prepare_delayed_startup_reply();
    execute(APP_READ_LIMITS, 0, 0);
    execute(APP_READ_NODE, 0, 0);
    memset(metadata_read_sends, 0, sizeof(metadata_read_sends));
    step_startup_metadata_recovery();
    CHECK(metadata_read_sends[METADATA_VERSION] == 1);
    CHECK(metadata_read_sends[METADATA_NEGATIVE] == 0 && metadata_read_sends[METADATA_POSITIVE] == 0);
    CHECK(metadata_read_sends[METADATA_NODE] == 0);
}

static void test_startup_metadata_respects_pause_motion_and_freshness(void)
{
    prepare_delayed_startup_reply();
    const unsigned before = all_sends;
    atomic_store(&cancel_requested, true);
    step_startup_metadata_recovery();
    atomic_store(&cancel_requested, false);
    model.armed = true;
    step_startup_metadata_recovery();
    model.armed = false; model.demo_active = true;
    step_startup_metadata_recovery();
    model.demo_active = false;
    fake_time += CONFIG_KST_STALE_MS + 1;
    step_startup_metadata_recovery();
    CHECK(all_sends == before && metadata_recovery.attempts[0] == 0);
    execute(APP_READ_STATUS, 0, 0);
    step_startup_metadata_recovery();
    CHECK(all_sends == before + 2 && metadata_recovery.attempts[0] == 1);
}

static void test_features_upper_byte_keeps_raw_value_and_position_valid(void)
{
    reset_ready();
    fake_feature_high = 1;
    fake_position = 184;
    model.features_valid = model.position_valid = raw_position_valid = false;
    execute(APP_READ_POSITION, 0, 0);
    CHECK(raw_position_valid && !model.position_valid);
    execute(APP_READ_FEATURES, 0, 0);
    CHECK(model.features_valid && model.feature_bits == 0 && model.feature_raw == 0x0100);
    CHECK(model.position_valid && model.position_deci_deg == 184);
    CHECK(!model.armed && motion_sends == 0);
}

static void test_outside_position_allows_only_explicit_in_range_recovery(void)
{
    const int16_t positions[] = {-1005, 1005};
    for (unsigned i = 0; i < sizeof(positions) / sizeof(positions[0]); ++i) {
        reset_ready();
        fake_position = positions[i];
        const int target = fake_position < 0 ? -1000 : 1000;
        execute(APP_READ_POSITION, 0, 0);
        publish();
        CHECK(model.position_valid && model.position_deci_deg == fake_position);
        CHECK(model.position_outside_limits && published.position_outside_limits);
        CHECK(!model.armed && motion_sends == 0 && motion_ready());
        execute(APP_MOVE, target, 0); /* Receiving out-of-range feedback never arms. */
        CHECK(motion_sends == 0);
        execute(APP_ARM, 1, 0);
        CHECK(model.armed);
        execute(APP_DEMO_START, 100, 4000);
        CHECK(!model.demo_active && motion_sends == 0); /* Unsafe sweep start rejected. */
        execute(APP_MOVE, target < 0 ? -1001 : 1001, 0);
        CHECK(motion_sends == 0); /* No widening of target support. */
        execute(APP_MOVE, target, 0);
        CHECK(motion_sends == 1 && model.target_deci_deg == target);
        fake_position = (int16_t)target;
        execute(APP_READ_POSITION, 0, 0);
        publish();
        CHECK(model.position_valid && !model.position_outside_limits);
        invalidate_readback();
        publish();
        CHECK(!model.position_valid && !model.position_outside_limits);
    }

    reset_ready();
    active_negative = -200; /* Staged tighter host limits also define recovery. */
    fake_position = -205;
    execute(APP_READ_POSITION, 0, 0);
    publish();
    CHECK(model.position_outside_limits);
    execute(APP_ARM, 1, 0);
    execute(APP_MOVE, -201, 0);
    CHECK(motion_sends == 0);
    execute(APP_MOVE, -200, 0);
    CHECK(motion_sends == 1);
}

static void test_outside_recovery_keeps_abort_pause_and_stale_gates(void)
{
    reset_ready();
    fake_position = -1005;
    execute(APP_READ_POSITION, 0, 0);
    const kst_event_t abort = {.type = KST_EVENT_ABORT, .node = 0x25,
                               .index = KST_INDEX_POSITION_TARGET, .abort_code = 0x06090030};
    on_event(NULL, &abort);
    execute(APP_ARM, 1, 0);
    CHECK(model.abort_code == 0x06090030 && !model.armed && motion_sends == 0);
    execute(APP_CLEAR_ALERTS, 0, 0);
    CHECK(!model.abort_code && !model.armed && motion_sends == 0);
    CHECK(app_submit(APP_ARM, 1, 0));
    CHECK(app_submit(APP_MOVE, -1000, 0));
    CHECK(app_submit(APP_STOP_MOTION, 0, 0));
    cancel_if_requested(); consume_one(); consume_one();
    CHECK(!model.armed && motion_sends == 0);
    fake_time += CONFIG_KST_STALE_MS + 1;
    execute(APP_ARM, 1, 0);
    execute(APP_MOVE, -1000, 0);
    CHECK(!model.armed && motion_sends == 0);
}

#if CONFIG_KST_TRANSPORT_USB_GS
static void test_usb_generation_blocks_all_old_commands(void)
{
    reset_ready(); execute(APP_ARM, 1, 0);
    CHECK(app_submit(APP_MOVE, 50, 0));
    CHECK(app_submit(APP_SAVE, 0, 0));
    CHECK(app_submit(APP_CENTER, 0, 0));
    CHECK(app_submit(APP_SET_NODE, 38, 0));
    CHECK(app_submit(APP_CLEAR_ALERTS, 0, 0));
    const unsigned before_disconnect = all_sends;
    fake_usb_connected = fake_usb_ready = false; ++fake_generation;
    service_transport();
    CHECK(!model.ready && !model.online && !model.armed);
    CHECK(!model.position_valid && !model.status_valid && !model.features_valid);
    CHECK(!model.node_valid && !model.version_valid && !model.negative_limit_valid);
    CHECK(all_sends == before_disconnect);
    fake_usb_connected = fake_usb_ready = true; ++fake_generation;
    service_transport();
    CHECK(motion_ready() && !model.armed);
    const unsigned after_refresh = all_sends;
    model.low_voltage_latched = true; /* new-session fault must not be cleared */
    for (unsigned i = 0; i < 5; ++i) consume_one();
    CHECK(all_sends == after_refresh && motion_sends == 0);
    CHECK(model.low_voltage_latched);
    CHECK(!model.config_dirty && !model.requires_restart);
    CHECK(app_submit(APP_SAVE, 0, 0)); consume_one();
    CHECK(all_sends == after_refresh + 1); /* a fresh user action is allowed */
}

static void test_usb_fault_and_disconnect_during_drain(void)
{
    reset_ready();
    fake_usb_ready = false;
    service_transport();
    CHECK(!model.ready && model.adapter_connected && !model.position_valid);
    execute(APP_ARM, 1, 0);
    CHECK(!model.armed && motion_sends == 0);

    reset_ready(); execute(APP_ARM, 1, 0);
    inject_disconnect = true;
    execute(APP_MOVE, 50, 0);
    CHECK(!model.armed && motion_sends == 0 && !model.ready);

    reset_ready();
    const unsigned before = all_sends;
    inject_disconnect = true;
    execute(APP_SAVE, 0, 0);
    CHECK(all_sends == before && !model.ready); /* configuration final gate too */

    reset_ready(); execute(APP_ARM, 1, 0);
    inject_late_pause = true;
    execute(APP_MOVE, 50, 0);
    CHECK(motion_sends == 0 && !model.armed && model.ready);

    reset_ready(); execute(APP_ARM, 1, 0);
    inject_late_rx = true;
    execute(APP_MOVE, 50, 0);
    CHECK(motion_sends == 0 && !model.armed && model.ready);
}

static void test_startup_metadata_connection_change_resets_budget(void)
{
    prepare_delayed_startup_reply();
    inject_disconnect = true;
    const unsigned before = all_sends;
    step_startup_metadata_recovery();
    CHECK(all_sends == before && !model.ready && metadata_recovery.pending);
    CHECK(!last_event_time && metadata_recovery.next_ms == 0);
    for (unsigned i = 0; i < METADATA_COUNT; ++i) CHECK(metadata_recovery.attempts[i] == 0);
    fake_usb_connected = fake_usb_ready = true; ++fake_generation;
    service_transport();
    step_startup_metadata_recovery();
    CHECK(!metadata_recovery.pending && model.node_valid && model.version_valid);
    CHECK(!model.armed && motion_sends == 0);
}

static void test_outside_recovery_cannot_cross_usb_connection(void)
{
    reset_ready();
    fake_position = -1005;
    execute(APP_READ_POSITION, 0, 0);
    execute(APP_ARM, 1, 0);
    CHECK(app_submit(APP_MOVE, -1000, 0));
    fake_usb_connected = fake_usb_ready = false; ++fake_generation;
    service_transport();
    CHECK(!model.position_valid && !model.position_outside_limits);
    fake_usb_connected = fake_usb_ready = true; ++fake_generation;
    service_transport();
    CHECK(model.position_valid && model.position_outside_limits && !model.armed);
    consume_one();
    CHECK(motion_sends == 0 && !model.armed);
}

#if CONFIG_KST_PROBE_360_ON_START
static void prepare_limit_probe(unsigned mode)
{
    reset_ready();
    servo.node = model.node_id = model.device_node_id = 5;
    memcpy(model.version, "3.66", 5);
    memcpy(fake_version, "3.66", 5);
    limit_probe_done = false;
    fake_probe_mode = mode;
    CHECK(motion_ready() && model.node_valid && model.version_valid);
}

static void test_limit_probe_accept_restore_and_rejection(void)
{
    prepare_limit_probe(PROBE_ACCEPT);
    CHECK(app_submit(APP_ARM, 1, 0));
    const unsigned before = all_sends;
    run_startup_limit_probe();
    CHECK(fake_probe_tx == 10 && all_sends == before + 10);
    CHECK(fake_probe_candidates[0] == 1 && fake_probe_candidates[1] == 1);
    CHECK(fake_probe_restores[0] == 1 && fake_probe_restores[1] == 1);
    CHECK(fake_positive_limit == 1000 && fake_negative_limit == -1000);
    CHECK(model.positive_limit == 1000 && model.negative_limit == -1000);
    CHECK(active_positive == 1000 && active_negative == -1000);
    CHECK(!model.limit_probe_restore_unverified && !model.armed && !limit_probe_running && motion_sends == 0);
    consume_one();
    CHECK(!model.armed); /* Even a pre-probe ARM is not replayed. */
    run_startup_limit_probe();
    CHECK(fake_probe_tx == 10); /* Once only. */

    prepare_limit_probe(PROBE_REJECT_POSITIVE);
    run_startup_limit_probe();
    CHECK(fake_probe_candidates[0] == 1 && fake_probe_candidates[1] == 0);
    CHECK(fake_probe_restores[0] == 1 && fake_probe_tx == 5);
    CHECK(!model.limit_probe_restore_unverified && fake_positive_limit == 1000);

    prepare_limit_probe(PROBE_UNCHANGED);
    run_startup_limit_probe();
    CHECK(fake_probe_candidates[0] == 1 && fake_probe_candidates[1] == 0);
    CHECK(fake_probe_restores[0] == 1 && !model.limit_probe_restore_unverified);

    prepare_limit_probe(PROBE_READ_TIMEOUT);
    const uint64_t started = fake_time;
    run_startup_limit_probe();
    CHECK(fake_time == started + 200 && fake_probe_candidates[0] == 0 && fake_probe_tx == 1);
    CHECK(!model.limit_probe_restore_unverified && motion_sends == 0);
}

static void test_limit_probe_uncertain_write_and_pause_restore_original(void)
{
    prepare_limit_probe(PROBE_WRITE_TIMEOUT);
    const uint64_t started = fake_time;
    run_startup_limit_probe();
    CHECK(fake_time == started + 200);
    CHECK(fake_probe_candidates[0] == 1 && fake_probe_candidates[1] == 0);
    CHECK(fake_probe_restores[0] == 1 && fake_positive_limit == 1000);
    CHECK(!model.limit_probe_restore_unverified && motion_sends == 0);

    prepare_limit_probe(PROBE_CANCEL);
    CHECK(app_submit(APP_ARM, 1, 0));
    CHECK(app_submit(APP_REFRESH, 0, 0));
    run_startup_limit_probe();
    CHECK(fake_probe_candidates[0] == 1 && fake_probe_candidates[1] == 0);
    CHECK(fake_probe_restores[0] == 1 && fake_positive_limit == 1000);
    CHECK(!model.limit_probe_restore_unverified && atomic_load(&cancel_requested));
    const unsigned before = all_sends;
    cancel_if_requested(); consume_one(); consume_one();
    CHECK(all_sends == before && !model.armed && !model.config_dirty); /* old ARM/REFRESH discarded */
}

static void test_limit_probe_discards_submitter_paused_around_cancel(void)
{
    const app_action_t actions[] = {APP_SAVE, APP_LIMIT_POSITIVE};
    for (unsigned phase = 1; phase <= 2; ++phase) {
        for (unsigned a = 0; a < sizeof(actions) / sizeof(actions[0]); ++a) {
            prepare_limit_probe(PROBE_ACCEPT);
            fake_submit_pause_phase = phase;
            CHECK(app_submit(actions[a], 800, 0)); /* The probe runs while submitter is paused. */
            CHECK(limit_probe_done && !atomic_load(&limit_probe_active));
            CHECK(fake_probe_candidates[0] == 1 && fake_probe_candidates[1] == 1);
            const unsigned before = all_sends;
            cancel_if_requested(); consume_one();
            CHECK(all_sends == before && !model.config_dirty && motion_sends == 0);
        }
    }
}

static void test_limit_probe_restore_failures_latch_writes_but_allow_readback(void)
{
    const unsigned modes[] = {PROBE_RESTORE_ABORT, PROBE_RESTORE_TIMEOUT, PROBE_DISCONNECT,
                              PROBE_REJECT_READ_TIMEOUT};
    for (unsigned i = 0; i < sizeof(modes) / sizeof(modes[0]); ++i) {
        prepare_limit_probe(modes[i]);
        run_startup_limit_probe();
        CHECK(model.limit_probe_restore_unverified && !model.armed);
        CHECK(model.config_dirty && model.requires_restart && fake_probe_candidates[1] == 0);
        if (modes[i] == PROBE_REJECT_READ_TIMEOUT) CHECK(fake_probe_restores[0] == 1);
        if (modes[i] == PROBE_DISCONNECT) {
            CHECK(fake_probe_restores[0] == 0); /* Never restore across a new USB connection. */
            fake_usb_connected = fake_usb_ready = true; ++fake_generation;
            fake_positive_limit = 1000; /* Manual external repair does not silently clear the latch. */
            service_transport();
        }
        const unsigned before = all_sends;
        execute(APP_CLEAR_ALERTS, 0, 0);
        execute(APP_ARM, 1, 0);
        execute(APP_MOVE, 0, 0);
        execute(APP_DEMO_START, 100, 4000);
        execute(APP_CENTER, 0, 0);
        execute(APP_LIMIT_POSITIVE, 800, 0);
        execute(APP_SET_NODE, 6, 0);
        execute(APP_SET_BAUD, 500000, 0);
        execute(APP_REPORT_INTERVAL, 30, 0);
        execute(APP_SAVE, 0, 0);
        CHECK(all_sends == before && model.limit_probe_restore_unverified && motion_sends == 0);
        execute(APP_READ_POSITION, 0, 0);
        CHECK(all_sends == before + 1);
        execute(APP_CONNECT, 5, 1000000);
        CHECK(model.limit_probe_restore_unverified && !model.armed);
    }
}

static void test_limit_probe_skips_ineligible_startup(void)
{
    for (unsigned variant = 0; variant < 8; ++variant) {
        prepare_limit_probe(PROBE_ACCEPT);
        if (variant == 0) model.node_id = servo.node = 6;
        if (variant == 1) memcpy(model.version, "3.61", 5);
        if (variant == 2) model.position_valid = false;
        if (variant == 3) model.position_deci_deg = -1005;
        if (variant == 4) model.abort_code = 0x06090030;
        if (variant == 5) model.low_voltage_latched = true;
        if (variant == 6) atomic_store(&cancel_requested, true);
        if (variant == 7) fake_time += CONFIG_KST_STALE_MS + 1;
        const unsigned before = all_sends;
        run_startup_limit_probe();
        CHECK(limit_probe_done && fake_probe_tx == 0 && all_sends == before);
        CHECK(!model.limit_probe_restore_unverified);
    }
}
#endif

#if CONFIG_KST_DISCOVER_ON_START
static void prepare_discovery(void)
{
    reset_ready();
    invalidate_readback();
    auto_reply = false;
    startup_discovery_done = false;
    CHECK(app_submit(APP_ARM, 1, 0));
    CHECK(app_submit(APP_MOVE, 50, 0));
}

static void test_discovery_is_read_only_and_keeps_selected_client(void)
{
    prepare_discovery();
    fake_discovery_reply_node = 17;
    fake_broadcast_replies = true;
    const uint64_t before = fake_time;
    kst_servo_t original = servo;
    CHECK(run_startup_discovery() == 1);
    CHECK(fake_discovery_tx == 127 && !fake_discovery_nodes[0]);
    for (unsigned node = 1; node <= 127; ++node) CHECK(fake_discovery_nodes[node]);
    CHECK(fake_broadcast_tx == 1);
    CHECK(broadcast_raw_logs == 5 && broadcast_out_of_range_logs == 2);
    CHECK(broadcast_found_logs == 2); /* Duplicate valid reply, unique found count. */
    CHECK(fake_time == before + 500u + 126u * 200u);
    CHECK(memcmp(&servo, &original, sizeof(servo)) == 0 && model.node_id == 0x25);
    CHECK(startup_discovery_done && !discovery_running && !model.armed && !model.busy);
    CHECK(!model.version_valid && last_event_time == 0); /* Other-node data stays isolated. */
    const unsigned sends = all_sends;
    consume_one(); consume_one();
    CHECK(all_sends == sends && motion_sends == 0 && !model.armed);
    link_needs_refresh = true;
    service_transport();
    CHECK(fake_discovery_tx == 127 && fake_broadcast_tx == 1); /* once */
}

static void test_discovery_cancellation_invalidates_queued_connect(void)
{
    prepare_discovery();
    fake_discovery_cancel_node = 3;
    const uint64_t before = fake_time;
    CHECK(run_startup_discovery() == 0);
    CHECK(fake_discovery_tx == 3 && fake_broadcast_tx == 1 && fake_time == before + 1100);
    CHECK(atomic_load(&cancel_requested) && !model.armed && !discovery_running);
    const unsigned sends = all_sends;
    consume_one(); consume_one(); consume_one();
    CHECK(all_sends == sends && model.node_id == 0x25 && servo.node == 0x25);
    CHECK(motion_sends == 0);
}

static void test_discovery_stops_on_connection_generation_change(void)
{
    prepare_discovery();
    fake_discovery_generation_node = 2;
    CHECK(run_startup_discovery() == 0);
    CHECK(fake_discovery_tx == 2 && !fake_discovery_nodes[3]);
    CHECK(!discovery_running && !model.armed && servo.node == 0x25);
    CHECK(model.node_id == 0x25 && motion_sends == 0);
}

static void test_discovery_auto_start_requires_missing_initial_reply(void)
{
    prepare_discovery();
    fake_discovery_cancel_node = 1;
    link_needs_refresh = true;
    service_transport();
    CHECK(startup_discovery_done && fake_discovery_tx == 1 && !discovery_running);
    reset_ready(); /* Initial valid readbacks suppress automatic discovery. */
    CHECK(startup_discovery_done && fake_discovery_tx == 0 && fake_broadcast_tx == 0);
}

static void test_discovery_broadcast_cancel_and_generation_change(void)
{
    prepare_discovery();
    fake_broadcast_cancel = true;
    kst_servo_t original = servo;
    CHECK(run_startup_discovery() == 0);
    CHECK(fake_broadcast_tx == 1 && fake_discovery_tx == 0);
    CHECK(atomic_load(&cancel_requested) && !model.armed && !discovery_running);
    CHECK(memcmp(&servo, &original, sizeof(servo)) == 0);
    consume_one(); consume_one(); consume_one();
    CHECK(model.node_id == 0x25 && motion_sends == 0);

    prepare_discovery();
    fake_broadcast_generation = true;
    CHECK(run_startup_discovery() == 0);
    CHECK(fake_broadcast_tx == 1 && fake_discovery_tx == 0);
    CHECK(!model.armed && !discovery_running && servo.node == 0x25);
}

static void test_discovery_preserves_later_local_recovery_exemption(void)
{
    prepare_discovery();
    fake_discovery_cancel_node = 1;
    CHECK(run_startup_discovery() == 0);
    consume_one(); consume_one(); consume_one(); /* Old ARM/MOVE/CONNECT rejected. */
    cancel_if_requested();
    const unsigned scan_epoch = atomic_load(&connection_epoch);
    request_t local = {APP_CONNECT, 42, 500000, atomic_load(&motion_epoch), scan_epoch};
    atomic_fetch_add(&connection_epoch, 1); /* An ordinary later link change. */
    handle(&local);
    CHECK(model.node_id == 42 && servo.node == 42);
    local.action = APP_RECOVER_BUS;
    local.connection_epoch = atomic_load(&connection_epoch);
    atomic_fetch_add(&connection_epoch, 1);
    const uint32_t generation = fake_generation;
    handle(&local);
    CHECK(fake_generation > generation && model.node_id == 42);
    CHECK(fake_discovery_tx == 1 && motion_sends == 0);
}
#endif
#endif

int main(void)
{
    test_pause_epoch_and_full_queue();
    test_faults_discovered_during_pre_send_drain();
    test_stale_fault_latches_and_bus_off();
    test_conservative_staged_limits_and_save();
    test_feedback_sign_requires_new_position_sample();
    test_reports_do_not_guess_reverse_sign_semantics();
    test_center_timeout_invalidates_old_coordinates();
    test_lost_configuration_ack_stays_conservative();
    test_sweep_final_gate();
    test_delayed_startup_metadata_recovers_one_read_at_a_time();
    test_startup_metadata_retry_limit_and_missing_item_skip();
    test_startup_metadata_respects_pause_motion_and_freshness();
    test_features_upper_byte_keeps_raw_value_and_position_valid();
    test_outside_position_allows_only_explicit_in_range_recovery();
    test_outside_recovery_keeps_abort_pause_and_stale_gates();
#if CONFIG_KST_TRANSPORT_USB_GS
    test_usb_generation_blocks_all_old_commands();
    test_usb_fault_and_disconnect_during_drain();
    test_startup_metadata_connection_change_resets_budget();
    test_outside_recovery_cannot_cross_usb_connection();
#if CONFIG_KST_PROBE_360_ON_START
    test_limit_probe_accept_restore_and_rejection();
    test_limit_probe_uncertain_write_and_pause_restore_original();
    test_limit_probe_discards_submitter_paused_around_cancel();
    test_limit_probe_restore_failures_latch_writes_but_allow_readback();
    test_limit_probe_skips_ineligible_startup();
#endif
#if CONFIG_KST_DISCOVER_ON_START
    test_discovery_is_read_only_and_keeps_selected_client();
    test_discovery_cancellation_invalidates_queued_connect();
    test_discovery_stops_on_connection_generation_change();
    test_discovery_auto_start_requires_missing_initial_reply();
    test_discovery_broadcast_cancel_and_generation_change();
    test_discovery_preserves_later_local_recovery_exemption();
#endif
#endif
    printf("PASS: %u KST application controller checks\n", checks);
    return EXIT_SUCCESS;
}
