#include "kst_servo.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned checks;
#define CHECK(expression) do { \
    ++checks; \
    if (!(expression)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #expression); \
        exit(EXIT_FAILURE); \
    } \
} while (0)

typedef struct {
    kst_can_frame_t frame;
    unsigned after_send;
    uint64_t at_ms;
    bool extended, rtr, fd, used;
} queued_frame_t;

typedef struct {
    uint64_t now;
    queued_frame_t queue[96];
    unsigned queue_size;
    kst_can_frame_t sent[16];
    unsigned send_count;
    unsigned callback_count;
    uint8_t expected_node;
    kst_event_t last_event;
    unsigned send_latency_ms;
    bool send_error;
    bool receive_error;
    kst_servo_t *reenter_client;
    kst_result_t reenter_result;
} fake_t;

static int fake_send(void *ctx, const kst_can_frame_t *frame, uint32_t timeout_ms)
{
    fake_t *fake = ctx;
    CHECK(fake->send_count < sizeof(fake->sent) / sizeof(fake->sent[0]));
    CHECK(timeout_ms == 25);
    fake->sent[fake->send_count++] = *frame;
    fake->now += fake->send_latency_ms;
    return fake->send_error ? -1 : 0;
}

static int fake_receive(void *ctx, kst_can_frame_t *frame, bool *extended,
                         bool *rtr, bool *fd, uint32_t timeout_ms)
{
    fake_t *fake = ctx;
    if (fake->receive_error) return -1;
    for (unsigned i = 0; i < fake->queue_size; ++i) {
        queued_frame_t *entry = &fake->queue[i];
        if (!entry->used && entry->after_send <= fake->send_count &&
            entry->at_ms <= fake->now + timeout_ms) {
            if (entry->at_ms > fake->now) fake->now = entry->at_ms;
            entry->used = true;
            *frame = entry->frame;
            *extended = entry->extended;
            *rtr = entry->rtr;
            *fd = entry->fd;
            return 1;
        }
    }
    fake->now += timeout_ms;
    return 0;
}

static uint64_t fake_now(void *ctx) { return ((fake_t *)ctx)->now; }

static void callback(void *ctx, const kst_event_t *event)
{
    fake_t *fake = ctx;
    CHECK(event->node == fake->expected_node);
    fake->last_event = *event;
    ++fake->callback_count;
    if (fake->reenter_client != NULL) {
        fake->reenter_result = kst_servo_read_position(fake->reenter_client, NULL);
    }
}

static void init_node(kst_servo_t *client, fake_t *fake, uint8_t node)
{
    memset(fake, 0, sizeof(*fake));
    fake->expected_node = node;
    const kst_transport_t transport = {
        .ctx = fake, .send = fake_send, .receive = fake_receive, .now_ms = fake_now
    };
    CHECK(kst_servo_init(client, node, &transport, 25, callback, fake) == KST_SERVO_OK);
}

static void init(kst_servo_t *client, fake_t *fake)
{ init_node(client, fake, 0x25); }

static queued_frame_t *enqueue(fake_t *fake, unsigned after_send, uint64_t at_ms,
                               uint32_t id, uint8_t len, const uint8_t data[8])
{
    CHECK(fake->queue_size < sizeof(fake->queue) / sizeof(fake->queue[0]));
    queued_frame_t *entry = &fake->queue[fake->queue_size++];
    entry->frame.id = id;
    entry->frame.len = len;
    memcpy(entry->frame.data, data, 8);
    entry->after_send = after_send;
    entry->at_ms = at_ms;
    entry->extended = true;
    return entry;
}

static void enqueue_ack(fake_t *fake, unsigned after_send, uint64_t at_ms, uint16_t index)
{
    enqueue(fake, after_send, at_ms, 0x5A5, 8,
            (const uint8_t[8]){0x60, (uint8_t)index, (uint8_t)(index >> 8), 0, 0, 0, 0, 0});
}

static void enqueue_position(fake_t *fake, unsigned after_send, uint64_t at_ms, int16_t angle)
{
    enqueue(fake, after_send, at_ms, 0x5A5, 8,
            (const uint8_t[8]){0x4B, 0x02, 0x60, 0, (uint8_t)angle,
                               (uint8_t)((uint16_t)angle >> 8), 0, 0});
}

static void enqueue_report(fake_t *fake, unsigned after_send, uint64_t at_ms, int16_t angle)
{
    enqueue(fake, after_send, at_ms, 0x5A5, 4,
            (const uint8_t[8]){(uint8_t)angle, (uint8_t)((uint16_t)angle >> 8), 0, 0, 0, 0, 0, 0});
}

static void enqueue_features(fake_t *fake, unsigned after_send, uint8_t bits)
{
    enqueue(fake, after_send, 0, 0x5A5, 8,
            (const uint8_t[8]){0x4B, 0x0F, 0x30, 0, bits, 0, 0, 0});
}

static void test_interleaved_telemetry_and_stale_queue(void)
{
    kst_servo_t client;
    fake_t fake;
    init(&client, &fake);
    enqueue_position(&fake, 0, 0, -100); /* stale but still delivered to callback */
    enqueue_report(&fake, 1, 2, 200); /* cannot satisfy explicit position read */
    enqueue(&fake, 1, 3, 0x5A5, 8, (const uint8_t[8]){0x43, 0x05, 0x60, 0, 0x32, 0, 0xF6, 0});
    enqueue(&fake, 1, 4, 0x5A6, 8, (const uint8_t[8]){0x4B, 0x02, 0x60, 0, 0, 0, 0, 0});
    queued_frame_t *bad = enqueue(&fake, 1, 5, 0x5A5, 8,
                                  (const uint8_t[8]){0x4B, 0x02, 0x60, 0, 0, 0, 0, 0});
    bad->extended = false;
    enqueue_position(&fake, 1, 6, -109);
    kst_event_t event;
    CHECK(kst_servo_read_position(&client, &event) == KST_SERVO_OK);
    CHECK(event.type == KST_EVENT_POSITION && event.position_deci_deg == -109);
    CHECK(fake.send_count == 1 && fake.callback_count == 4 && fake.now == 6);
    CHECK(client.stats.drained_frames == 1 && client.stats.invalid_frames == 1);
    CHECK(client.stats.ignored_frames == 1 && client.stats.received_frames == 6);
    CHECK(!client.busy);
}

static void test_matching_ack_and_absolute_timeout(void)
{
    kst_servo_t client;
    fake_t fake;
    init(&client, &fake);
    enqueue_ack(&fake, 1, 2, KST_INDEX_REPORT_INTERVAL);
    enqueue_ack(&fake, 1, 3, KST_INDEX_CENTER);
    CHECK(kst_servo_set_center(&client) == KST_SERVO_OK);
    CHECK(fake.callback_count == 2 && fake.send_count == 1);

    init(&client, &fake);
    enqueue_ack(&fake, 0, 0, KST_INDEX_CENTER); /* stale correct ACK must be drained */
    enqueue_ack(&fake, 1, 5, KST_INDEX_REPORT_INTERVAL);
    enqueue_report(&fake, 1, 15, 0);
    enqueue_ack(&fake, 1, 24, KST_INDEX_NODE_ID);
    CHECK(kst_servo_set_center(&client) == KST_SERVO_ERR_TIMEOUT);
    CHECK(fake.now == 25 && fake.send_count == 1);
    CHECK(client.stats.timeouts == 1 && client.stats.drained_frames == 1);
    CHECK(!client.busy);

    init(&client, &fake);
    fake.send_latency_ms = 10;
    enqueue_position(&fake, 1, 30, 0);
    CHECK(kst_servo_read_position(&client, NULL) == KST_SERVO_ERR_TIMEOUT);
    CHECK(fake.now == 25); /* send time belongs to the same 25-ms transaction */
}

static void test_abort_and_no_ack_commands(void)
{
    kst_servo_t client;
    fake_t fake;
    init(&client, &fake);
    enqueue(&fake, 1, 0, 0x5A5, 8, (const uint8_t[8]){0x80, 0x09, 0x30, 0, 0x20, 0, 0, 0x08});
    CHECK(kst_servo_set_center(&client) == KST_SERVO_ERR_ABORT);
    CHECK(client.last_abort == 0x08000020 && client.last_abort_index == KST_INDEX_CENTER);
    CHECK(client.stats.aborts == 1 && fake.send_count == 1);

    init_node(&client, &fake, 5);
    enqueue(&fake, 1, 0, 0x585, 8, (const uint8_t[8]){0x80, 0x0B, 0x30, 0, 0x06, 0x09, 0, 0x30});
    CHECK(kst_servo_set_limit(&client, true, 1000) == KST_SERVO_ERR_ABORT);
    CHECK(client.last_abort == 0x06090030 && client.last_abort_index == KST_INDEX_LIMIT_POSITIVE);
    CHECK(fake.last_event.abort_wire_le == 0x30000906 && fake.last_event.abort_byte_swapped);
    CHECK(client.stats.aborts == 1 && fake.send_count == 1 && !client.busy);

    init(&client, &fake);
    enqueue(&fake, 1, 0, 0x5A5, 8, (const uint8_t[8]){0x80, 0x00, 0x22, 0, 0x30, 0, 0x09, 0x06});
    enqueue_position(&fake, 1, 1, 10);
    CHECK(kst_servo_read_position(&client, NULL) == KST_SERVO_OK); /* another object's ABORT */
    CHECK(fake.callback_count == 2);

    init(&client, &fake);
    CHECK(kst_servo_set_position(&client, -109) == KST_SERVO_SENT_UNCONFIRMED);
    CHECK(kst_servo_save(&client) == KST_SERVO_SENT_UNCONFIRMED);
    CHECK(kst_servo_set_reporting(&client, false) == KST_SERVO_SENT_UNCONFIRMED);
    CHECK(fake.send_count == 3 && fake.now == 0 && client.stats.timeouts == 0);
    CHECK(fake.sent[0].data[4] == 0x93 && fake.sent[0].data[5] == 0xFF);
    CHECK(fake.sent[1].data[3] == 1 && fake.sent[1].data[4] == 's');
    CHECK(fake.sent[2].id == 0x25 && fake.sent[2].data[0] == 0x02);

    init(&client, &fake);
    enqueue_report(&fake, 1, 3, 20);
    CHECK(kst_servo_set_reporting(&client, true) == KST_SERVO_OK);
    CHECK(fake.callback_count == 1 && fake.now == 3);
}

static void test_features_read_modify_verify(void)
{
    kst_servo_t client;
    fake_t fake;
    init(&client, &fake);
    enqueue_features(&fake, 1, 0x9F);
    enqueue_ack(&fake, 2, 0, KST_INDEX_FEATURES);
    enqueue_features(&fake, 3, 0xBF);
    CHECK(kst_servo_set_features(&client, 0x20, 0x20) == KST_SERVO_OK);
    CHECK(fake.send_count == 3 && fake.callback_count == 3);
    CHECK(fake.sent[0].data[0] == 0x40 && fake.sent[1].data[0] == 0x22 && fake.sent[2].data[0] == 0x40);
    CHECK(fake.sent[1].data[4] == 0xBF); /* all six reserved bits preserved */

    init(&client, &fake);
    enqueue_features(&fake, 1, 0x9F);
    enqueue_ack(&fake, 2, 0, KST_INDEX_FEATURES);
    enqueue_features(&fake, 3, 0x9F);
    CHECK(kst_servo_set_features(&client, 0x20, 0x20) == KST_SERVO_ERR_VERIFY);
    CHECK(fake.send_count == 3 && !client.busy);

    init(&client, &fake);
    CHECK(kst_servo_set_features(&client, 0x20, 0x20) == KST_SERVO_ERR_TIMEOUT);
    CHECK(fake.send_count == 1); /* failed prerequisite read prevents write */
    CHECK(kst_servo_set_features(&client, 1, 0) == KST_SERVO_ERR_ARGUMENT);
    CHECK(kst_servo_set_features(&client, 0x20, 0x40) == KST_SERVO_ERR_ARGUMENT);
    CHECK(fake.send_count == 1);
}

static void test_unknown_feature_high_byte_read_and_write_guard(void)
{
    kst_servo_t client;
    fake_t fake;
    kst_event_t event;
    init_node(&client, &fake, 5);
    enqueue(&fake, 1, 0, 0x585, 8,
            (const uint8_t[8]){0x4B, 0x0F, 0x30, 0, 0x90, 0x5C, 0, 0});
    CHECK(kst_servo_read_features(&client, &event) == KST_SERVO_OK);
    CHECK(event.type == KST_EVENT_FEATURES && event.feature_raw == 0x5C90 && event.feature_bits == 0x90);
    CHECK(fake.send_count == 1 && fake.callback_count == 1 && !client.busy);
    CHECK(fake.last_event.feature_raw == 0x5C90 && client.stats.invalid_frames == 0);
    CHECK(fake.sent[0].id == 0x605 && fake.sent[0].len == 8);
    CHECK(memcmp(fake.sent[0].data, (const uint8_t[8]){0x40, 0x0F, 0x30, 0, 0, 0, 0, 0}, 8) == 0);

    /* Every documented writable bit, both values, and even a no-op must stop
     * after the prerequisite read when the complete value is undocumented. */
    const uint8_t masks[] = {KST_FEATURE_UNDERVOLTAGE, KST_FEATURE_REVERSE, KST_FEATURE_WRITABLE_MASK};
    const uint8_t high_bytes[] = {0x01, 0x5C, 0xFF};
    for (size_t h = 0; h < sizeof(high_bytes); ++h) {
        for (size_t m = 0; m < sizeof(masks); ++m) {
            for (unsigned enabled = 0; enabled <= 1; ++enabled) {
                init_node(&client, &fake, 5);
                enqueue(&fake, 1, 0, 0x585, 8,
                        (const uint8_t[8]){0x4B, 0x0F, 0x30, 0, 0x90, high_bytes[h], 0, 0});
                CHECK(kst_servo_set_features(&client, masks[m], enabled ? masks[m] : 0) ==
                      KST_SERVO_ERR_UNSUPPORTED_FEATURES);
                CHECK(fake.send_count == 1 && fake.sent[0].data[0] == 0x40);
                CHECK(fake.sent[0].id == 0x605 && fake.sent[0].data[1] == 0x0F && fake.sent[0].data[2] == 0x30);
                CHECK(fake.callback_count == 1 && !client.busy && client.stats.timeouts == 0);
                CHECK(fake.last_event.feature_raw == (uint16_t)(0x90U | ((uint16_t)high_bytes[h] << 8)));
                CHECK(fake.last_event.feature_bits == 0x90);
            }
        }
    }
    CHECK(strcmp(kst_servo_result_description(KST_SERVO_ERR_UNSUPPORTED_FEATURES),
                 "Feature write blocked: undocumented high byte") == 0);

    /* If a previously documented value changes format during verification,
     * do not report success or issue a compensating/retry write. */
    init(&client, &fake);
    enqueue_features(&fake, 1, 0x9F);
    enqueue_ack(&fake, 2, 0, KST_INDEX_FEATURES);
    enqueue(&fake, 3, 0, 0x5A5, 8,
            (const uint8_t[8]){0x4B, 0x0F, 0x30, 0, 0xBF, 0x5C, 0, 0});
    CHECK(kst_servo_set_features(&client, KST_FEATURE_UNDERVOLTAGE, KST_FEATURE_UNDERVOLTAGE) ==
          KST_SERVO_ERR_VERIFY);
    CHECK(fake.send_count == 3 && fake.callback_count == 3 && !client.busy);
}

static void test_other_high_level_methods(void)
{
    kst_servo_t client;
    fake_t fake;
    kst_event_t event;
    init(&client, &fake);
    enqueue(&fake, 1, 0, 0x5A5, 8, (const uint8_t[8]){0x4F, 0x00, 0x30, 0, 0x25, 0, 0, 0});
    CHECK(kst_servo_read_node(&client, &event) == KST_SERVO_OK && event.node_id == 0x25);
    enqueue(&fake, 2, 0, 0x5A5, 8, (const uint8_t[8]){0x43, 0x05, 0x60, 0, 0xF4, 0x01, 0xF6, 0});
    CHECK(kst_servo_read_status(&client, &event) == KST_SERVO_OK);
    CHECK(event.current_10ma == 500 && event.temperature_c == -10);
    enqueue_features(&fake, 3, 0x9F);
    CHECK(kst_servo_read_features(&client, &event) == KST_SERVO_OK && event.feature_bits == 0x9F &&
          event.feature_raw == 0x009F);
    enqueue(&fake, 4, 0, 0x5A5, 8, (const uint8_t[8]){0x43, 0x0B, 0x10, 0, '3', '.', '6', '1'});
    CHECK(kst_servo_read_version(&client, &event) == KST_SERVO_OK && strcmp(event.version, "3.61") == 0);
    enqueue(&fake, 5, 0, 0x5A5, 8, (const uint8_t[8]){0x4B, 0x0A, 0x30, 0, 0x18, 0xFC, 0, 0});
    CHECK(kst_servo_read_limit(&client, false, &event) == KST_SERVO_OK && event.position_deci_deg == -1000);
    enqueue(&fake, 6, 0, 0x5A5, 8, (const uint8_t[8]){0x4B, 0x0B, 0x30, 0, 0xE8, 0x03, 0, 0});
    CHECK(kst_servo_read_limit(&client, true, &event) == KST_SERVO_OK && event.position_deci_deg == 1000);
    enqueue_ack(&fake, 7, 0, KST_INDEX_REPORT_INTERVAL);
    CHECK(kst_servo_set_report_interval(&client, 100) == KST_SERVO_OK);
    enqueue_ack(&fake, 8, 0, KST_INDEX_BAUDRATE);
    CHECK(kst_servo_set_baudrate(&client, KST_BAUD_250K) == KST_SERVO_OK);
    enqueue_ack(&fake, 9, 0, KST_INDEX_NODE_ID);
    CHECK(kst_servo_set_node_id(&client, 0x26) == KST_SERVO_OK && client.node == 0x25);
    enqueue_ack(&fake, 10, 0, KST_INDEX_LIMIT_NEGATIVE);
    CHECK(kst_servo_set_limit(&client, false, -500) == KST_SERVO_OK);
    enqueue_ack(&fake, 11, 0, KST_INDEX_LIMIT_POSITIVE);
    CHECK(kst_servo_set_limit(&client, true, 500) == KST_SERVO_OK);
    CHECK(fake.send_count == 11); /* no save, restart, retry, or bitrate mutation */
}

static void test_poll_flags_errors_and_reentrancy(void)
{
    kst_servo_t client;
    fake_t fake;
    init(&client, &fake);
    queued_frame_t *bad = enqueue(&fake, 0, 0, 0x5A5, 4, (const uint8_t[8]){0, 0, 0, 0, 0, 0, 0, 0});
    bad->fd = true;
    bad = enqueue(&fake, 0, 0, 0x5A5, 4, (const uint8_t[8]){0, 0, 0, 0, 0, 0, 0, 0});
    bad->rtr = true;
    enqueue(&fake, 0, 0, 0x3A5, 4, (const uint8_t[8]){0x0A, 0x02, 0, 0, 0, 0, 0, 0});
    kst_event_t event;
    CHECK(kst_servo_poll(&client, 0, &event) == KST_SERVO_OK && event.type == KST_EVENT_UNDERVOLTAGE);
    CHECK(client.stats.invalid_frames == 2 && fake.callback_count == 1 && fake.send_count == 0);
    CHECK(kst_servo_poll(&client, 0, NULL) == KST_SERVO_ERR_TIMEOUT && fake.now == 0);
    fake.receive_error = true;
    CHECK(kst_servo_poll(&client, 5, NULL) == KST_SERVO_ERR_TRANSPORT && !client.busy);

    init(&client, &fake);
    fake.send_error = true;
    CHECK(kst_servo_set_position(&client, 0) == KST_SERVO_ERR_TRANSPORT);
    CHECK(fake.send_count == 1 && !client.busy);

    init(&client, &fake);
    fake.reenter_client = &client;
    enqueue_position(&fake, 1, 1, 0);
    CHECK(kst_servo_read_position(&client, NULL) == KST_SERVO_OK);
    CHECK(fake.reenter_result == KST_SERVO_ERR_RX_BUSY && fake.send_count == 1);
}

static void test_invalid_inputs_and_bounded_drain(void)
{
    kst_servo_t client;
    fake_t fake;
    init(&client, &fake);
    CHECK(kst_servo_init(NULL, 0x25, &client.transport, 25, NULL, NULL) == KST_SERVO_ERR_ARGUMENT);
    CHECK(kst_servo_init(&client, 0, &client.transport, 25, NULL, NULL) == KST_SERVO_ERR_ARGUMENT);
    CHECK(kst_servo_read_node(NULL, NULL) == KST_SERVO_ERR_ARGUMENT);
    CHECK(kst_servo_set_position(&client, 1001) == KST_SERVO_ERR_ARGUMENT);
    CHECK(kst_servo_set_node_id(&client, 0) == KST_SERVO_ERR_ARGUMENT);
    CHECK(kst_servo_set_report_interval(&client, 256) == KST_SERVO_ERR_ARGUMENT);
    CHECK(kst_servo_set_limit(&client, false, 1) == KST_SERVO_ERR_ARGUMENT);
    CHECK(kst_servo_set_baudrate(&client, (kst_baudrate_t)8) == KST_SERVO_ERR_ARGUMENT);
    CHECK(fake.send_count == 0);
    for (unsigned i = 0; i < 70; ++i) enqueue_report(&fake, 0, 0, 0);
    CHECK(kst_servo_read_position(&client, NULL) == KST_SERVO_ERR_RX_BUSY);
    CHECK(client.stats.drained_frames == 64 && fake.send_count == 0 && !client.busy);
    CHECK(strcmp(kst_servo_result_description(KST_SERVO_ERR_TIMEOUT), "Response timeout") == 0);
}

int main(void)
{
    test_interleaved_telemetry_and_stale_queue();
    test_matching_ack_and_absolute_timeout();
    test_abort_and_no_ack_commands();
    test_features_read_modify_verify();
    test_unknown_feature_high_byte_read_and_write_guard();
    test_other_high_level_methods();
    test_poll_flags_errors_and_reentrancy();
    test_invalid_inputs_and_bounded_drain();
    printf("PASS: %u KST synchronous driver checks\n", checks);
    return EXIT_SUCCESS;
}
