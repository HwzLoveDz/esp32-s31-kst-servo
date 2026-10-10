#include "kst_servo.h"

#include <stddef.h>
#include <string.h>

#define KST_DRAIN_LIMIT 64U
#define KST_WAIT_FRAME_LIMIT 4096U

static bool ready(const kst_servo_t *client)
{
    return client != NULL && client->initialized && client->node >= 1 && client->node <= 127 &&
           client->transport.send != NULL && client->transport.receive != NULL &&
           client->transport.now_ms != NULL && client->timeout_ms != 0;
}

static kst_result_t begin(kst_servo_t *client)
{
    if (!ready(client)) return KST_SERVO_ERR_ARGUMENT;
    if (client->busy) return KST_SERVO_ERR_RX_BUSY;
    client->busy = true;
    return KST_SERVO_OK;
}

static kst_result_t finish(kst_servo_t *client, kst_result_t result)
{
    client->busy = false;
    return result;
}

kst_result_t kst_servo_init(kst_servo_t *client, uint8_t node,
                           const kst_transport_t *transport, uint32_t timeout_ms,
                           kst_event_callback_t event_callback, void *user)
{
    if (client == NULL || transport == NULL || node < 1 || node > 127 || timeout_ms == 0 ||
        transport->send == NULL || transport->receive == NULL || transport->now_ms == NULL) {
        return KST_SERVO_ERR_ARGUMENT;
    }
    /* Copy first: transport may be a field of the client being reinitialized. */
    const kst_transport_t copied_transport = *transport;
    memset(client, 0, sizeof(*client));
    client->node = node;
    client->transport = copied_transport;
    client->timeout_ms = timeout_ms;
    client->event_callback = event_callback;
    client->user = user;
    client->initialized = true;
    return KST_SERVO_OK;
}

/* 1=parsed event, 0=malformed/unrelated, -1=transport error, -2=no frame. */
static int receive_event(kst_servo_t *client, uint32_t timeout_ms, kst_event_t *event)
{
    kst_can_frame_t frame = {0};
    bool extended = false, rtr = false, fd = false;
    const int received = client->transport.receive(client->transport.ctx, &frame, &extended,
                                                    &rtr, &fd, timeout_ms);
    if (received < 0) return -1;
    if (received == 0) return -2;
    if (received != 1) return -1;
    ++client->stats.received_frames;
    const kst_parse_result_t parsed = kst_parse(client->node, &frame, extended, rtr, fd, event);
    if (parsed == KST_PARSE_INVALID) {
        ++client->stats.invalid_frames;
        return 0;
    }
    if (parsed == KST_PARSE_IGNORED) {
        ++client->stats.ignored_frames;
        return 0;
    }
    if (event->type == KST_EVENT_ABORT) {
        ++client->stats.aborts;
        client->last_abort = event->abort_code;
        client->last_abort_index = event->index;
        client->last_abort_subindex = event->subindex;
    }
    if (client->event_callback != NULL) client->event_callback(client->user, event);
    return 1;
}

static kst_result_t drain(kst_servo_t *client)
{
    for (unsigned i = 0; i < KST_DRAIN_LIMIT; ++i) {
        kst_event_t event;
        const int received = receive_event(client, 0, &event);
        if (received == -2) return KST_SERVO_OK;
        if (received < 0) return KST_SERVO_ERR_TRANSPORT;
        ++client->stats.drained_frames;
    }
    return KST_SERVO_ERR_RX_BUSY;
}

static kst_result_t timeout(kst_servo_t *client)
{
    ++client->stats.timeouts;
    return KST_SERVO_ERR_TIMEOUT;
}

static kst_result_t wait_event(kst_servo_t *client, uint64_t start, uint32_t budget_ms,
                               bool any_event, kst_event_type_t type,
                               uint16_t index, uint8_t subindex, kst_event_t *out)
{
    for (unsigned i = 0; i < KST_WAIT_FRAME_LIMIT; ++i) {
        const uint64_t elapsed = client->transport.now_ms(client->transport.ctx) - start;
        if (budget_ms != 0 && elapsed >= budget_ms) return timeout(client);
        const uint32_t remaining = budget_ms == 0 ? 0 : budget_ms - (uint32_t)elapsed;
        kst_event_t event;
        const int received = receive_event(client, remaining, &event);
        if (received == -2) return timeout(client);
        if (received < 0) return KST_SERVO_ERR_TRANSPORT;
        if (received == 0) continue;
        if (any_event || (event.type == type && event.index == index && event.subindex == subindex)) {
            if (out != NULL) *out = event;
            return KST_SERVO_OK;
        }
        if (event.type == KST_EVENT_ABORT && event.index == index && event.subindex == subindex) {
            return KST_SERVO_ERR_ABORT;
        }
    }
    /* Also bounded when a defective adapter/clock returns infinite frames at
     * one timestamp. Never silently convert this into a successful response. */
    return KST_SERVO_ERR_RX_BUSY;
}

static kst_result_t exchange(kst_servo_t *client, const kst_can_frame_t *frame,
                             bool expect_reply, kst_event_type_t type,
                             uint16_t index, uint8_t subindex, kst_event_t *out)
{
    kst_result_t result = drain(client);
    if (result != KST_SERVO_OK) return result;
    client->last_abort = 0;
    client->last_abort_index = 0;
    client->last_abort_subindex = 0;
    const uint64_t start = client->transport.now_ms(client->transport.ctx);
    if (client->transport.send(client->transport.ctx, frame, client->timeout_ms) != 0) {
        return KST_SERVO_ERR_TRANSPORT;
    }
    if (!expect_reply) return KST_SERVO_SENT_UNCONFIRMED;
    return wait_event(client, start, client->timeout_ms, false, type, index, subindex, out);
}

static kst_result_t run_frame(kst_servo_t *client, const kst_can_frame_t *frame,
                              bool expect_reply, kst_event_type_t type,
                              uint16_t index, kst_event_t *out)
{
    const kst_result_t started = begin(client);
    if (started != KST_SERVO_OK) return started;
    return finish(client, exchange(client, frame, expect_reply, type, index, 0, out));
}

static bool read_frame(uint8_t node, uint16_t index, kst_can_frame_t *frame,
                        kst_event_type_t *type)
{
    switch (index) {
    case KST_INDEX_NODE_ID:
        *type = KST_EVENT_NODE_ID;
        return kst_make_read_node(node, frame);
    case KST_INDEX_POSITION:
        *type = KST_EVENT_POSITION;
        return kst_make_read_position(node, frame);
    case KST_INDEX_STATUS:
        *type = KST_EVENT_STATUS;
        return kst_make_read_status(node, frame);
    case KST_INDEX_FEATURES:
        *type = KST_EVENT_FEATURES;
        return kst_make_read_features(node, frame);
    case KST_INDEX_VERSION:
        *type = KST_EVENT_VERSION;
        return kst_make_read_version(node, frame);
    case KST_INDEX_LIMIT_NEGATIVE:
        *type = KST_EVENT_LIMIT_NEGATIVE;
        return kst_make_read_limit(node, false, frame);
    case KST_INDEX_LIMIT_POSITIVE:
        *type = KST_EVENT_LIMIT_POSITIVE;
        return kst_make_read_limit(node, true, frame);
    default:
        return false;
    }
}

static kst_result_t read_internal(kst_servo_t *client, uint16_t index, kst_event_t *out)
{
    kst_can_frame_t frame;
    kst_event_type_t type;
    if (!read_frame(client->node, index, &frame, &type)) return KST_SERVO_ERR_ARGUMENT;
    return exchange(client, &frame, true, type, index, 0, out);
}

static kst_result_t read_object(kst_servo_t *client, uint16_t index, kst_event_t *out)
{
    const kst_result_t started = begin(client);
    if (started != KST_SERVO_OK) return started;
    return finish(client, read_internal(client, index, out));
}

kst_result_t kst_servo_read_node(kst_servo_t *client, kst_event_t *out)
{ return read_object(client, KST_INDEX_NODE_ID, out); }
kst_result_t kst_servo_read_position(kst_servo_t *client, kst_event_t *out)
{ return read_object(client, KST_INDEX_POSITION, out); }
kst_result_t kst_servo_read_status(kst_servo_t *client, kst_event_t *out)
{ return read_object(client, KST_INDEX_STATUS, out); }
kst_result_t kst_servo_read_features(kst_servo_t *client, kst_event_t *out)
{ return read_object(client, KST_INDEX_FEATURES, out); }
kst_result_t kst_servo_read_version(kst_servo_t *client, kst_event_t *out)
{ return read_object(client, KST_INDEX_VERSION, out); }
kst_result_t kst_servo_read_limit(kst_servo_t *client, bool positive, kst_event_t *out)
{ return read_object(client, positive ? KST_INDEX_LIMIT_POSITIVE : KST_INDEX_LIMIT_NEGATIVE, out); }

kst_result_t kst_servo_set_reporting(kst_servo_t *client, bool enabled)
{
    kst_can_frame_t frame;
    if (!ready(client) || !kst_make_reporting(client->node, enabled, &frame)) return KST_SERVO_ERR_ARGUMENT;
    return run_frame(client, &frame, enabled, KST_EVENT_POSITION_REPORT, 0, NULL);
}

kst_result_t kst_servo_set_report_interval(kst_servo_t *client, uint16_t milliseconds)
{
    kst_can_frame_t frame;
    if (!ready(client) || !kst_make_report_interval(client->node, milliseconds, &frame)) return KST_SERVO_ERR_ARGUMENT;
    return run_frame(client, &frame, true, KST_EVENT_WRITE_ACK, KST_INDEX_REPORT_INTERVAL, NULL);
}

kst_result_t kst_servo_set_baudrate(kst_servo_t *client, kst_baudrate_t baudrate)
{
    kst_can_frame_t frame;
    if (!ready(client) || !kst_make_baudrate(client->node, baudrate, &frame)) return KST_SERVO_ERR_ARGUMENT;
    return run_frame(client, &frame, true, KST_EVENT_WRITE_ACK, KST_INDEX_BAUDRATE, NULL);
}

kst_result_t kst_servo_set_node_id(kst_servo_t *client, uint8_t new_node)
{
    kst_can_frame_t frame;
    if (!ready(client) || !kst_make_node_id(client->node, new_node, &frame)) return KST_SERVO_ERR_ARGUMENT;
    return run_frame(client, &frame, true, KST_EVENT_WRITE_ACK, KST_INDEX_NODE_ID, NULL);
}

kst_result_t kst_servo_set_limit(kst_servo_t *client, bool positive, int16_t position_deci_deg)
{
    kst_can_frame_t frame;
    if (!ready(client) || !kst_make_limit(client->node, positive, position_deci_deg, &frame)) return KST_SERVO_ERR_ARGUMENT;
    return run_frame(client, &frame, true, KST_EVENT_WRITE_ACK,
                     positive ? KST_INDEX_LIMIT_POSITIVE : KST_INDEX_LIMIT_NEGATIVE, NULL);
}

kst_result_t kst_servo_set_position(kst_servo_t *client, int16_t position_deci_deg)
{
    kst_can_frame_t frame;
    if (!ready(client) || !kst_make_position(client->node, position_deci_deg, &frame)) return KST_SERVO_ERR_ARGUMENT;
    return run_frame(client, &frame, false, KST_EVENT_POSITION, KST_INDEX_POSITION_TARGET, NULL);
}

kst_result_t kst_servo_save(kst_servo_t *client)
{
    kst_can_frame_t frame;
    if (!ready(client) || !kst_make_save(client->node, &frame)) return KST_SERVO_ERR_ARGUMENT;
    return run_frame(client, &frame, false, KST_EVENT_WRITE_ACK, KST_INDEX_SAVE, NULL);
}

kst_result_t kst_servo_set_center(kst_servo_t *client)
{
    kst_can_frame_t frame;
    if (!ready(client) || !kst_make_center(client->node, &frame)) return KST_SERVO_ERR_ARGUMENT;
    return run_frame(client, &frame, true, KST_EVENT_WRITE_ACK, KST_INDEX_CENTER, NULL);
}

kst_result_t kst_servo_set_features(kst_servo_t *client, uint8_t mask, uint8_t values)
{
    if (mask == 0 || (mask & (uint8_t)~KST_FEATURE_WRITABLE_MASK) != 0 ||
        (values & (uint8_t)~mask) != 0) return KST_SERVO_ERR_ARGUMENT;
    kst_result_t result = begin(client);
    if (result != KST_SERVO_OK) return result;
    kst_event_t before, after;
    result = read_internal(client, KST_INDEX_FEATURES, &before);
    if (result != KST_SERVO_OK) return finish(client, result);
    if ((before.feature_raw & UINT16_C(0xFF00)) != 0) {
        return finish(client, KST_SERVO_ERR_UNSUPPORTED_FEATURES);
    }
    kst_can_frame_t frame;
    if (!kst_make_features(client->node, before.feature_raw, mask, values, &frame)) {
        return finish(client, KST_SERVO_ERR_ARGUMENT);
    }
    result = exchange(client, &frame, true, KST_EVENT_WRITE_ACK, KST_INDEX_FEATURES, 0, NULL);
    if (result != KST_SERVO_OK) return finish(client, result);
    result = read_internal(client, KST_INDEX_FEATURES, &after);
    if (result != KST_SERVO_OK) return finish(client, result);
    return finish(client, (after.feature_raw & UINT16_C(0xFF00)) == 0 &&
                         (after.feature_bits & mask) == values ? KST_SERVO_OK : KST_SERVO_ERR_VERIFY);
}

kst_result_t kst_servo_poll(kst_servo_t *client, uint32_t timeout_ms, kst_event_t *out)
{
    const kst_result_t started = begin(client);
    if (started != KST_SERVO_OK) return started;
    const uint64_t start = client->transport.now_ms(client->transport.ctx);
    return finish(client, wait_event(client, start, timeout_ms, true, KST_EVENT_POSITION, 0, 0, out));
}

const char *kst_servo_result_description(kst_result_t result)
{
    switch (result) {
    case KST_SERVO_OK: return "Application response received";
    case KST_SERVO_SENT_UNCONFIRMED: return "Sent; protocol defines no application acknowledgement";
    case KST_SERVO_ERR_ARGUMENT: return "Invalid argument or uninitialized client";
    case KST_SERVO_ERR_TRANSPORT: return "CAN transport error";
    case KST_SERVO_ERR_TIMEOUT: return "Response timeout";
    case KST_SERVO_ERR_ABORT: return "Servo returned an application abort";
    case KST_SERVO_ERR_VERIFY: return "Feature readback did not match requested bits";
    case KST_SERVO_ERR_RX_BUSY: return "Receive queue busy, excessive traffic, or reentrant call";
    case KST_SERVO_ERR_UNSUPPORTED_FEATURES: return "Feature write blocked: undocumented high byte";
    default: return "Unknown driver result";
    }
}
