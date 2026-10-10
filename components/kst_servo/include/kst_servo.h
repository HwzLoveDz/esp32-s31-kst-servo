#ifndef KST_SERVO_H
#define KST_SERVO_H

#include "kst_protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    KST_SERVO_OK = 0,
    KST_SERVO_SENT_UNCONFIRMED = 1,
    KST_SERVO_ERR_ARGUMENT = -1,
    KST_SERVO_ERR_TRANSPORT = -2,
    KST_SERVO_ERR_TIMEOUT = -3,
    KST_SERVO_ERR_ABORT = -4,
    KST_SERVO_ERR_VERIFY = -5,
    KST_SERVO_ERR_RX_BUSY = -6,
    KST_SERVO_ERR_UNSUPPORTED_FEATURES = -7
} kst_result_t;

/* send: 0=queued/transmitted successfully, negative=transport error.
 * receive: 1=one complete frame, 0=timeout/empty, negative=transport error.
 * receive(timeout=0) MUST be nonblocking. Both callbacks MUST respect timeout.
 * now_ms is a monotonic millisecond clock. All callbacks are caller-owned.
 * The adapter MUST send extended classical DATA frames (no FD, no RTR).
 */
typedef struct {
    void *ctx;
    int (*send)(void *ctx, const kst_can_frame_t *frame, uint32_t timeout_ms);
    int (*receive)(void *ctx, kst_can_frame_t *frame, bool *extended,
                   bool *rtr, bool *fd, uint32_t timeout_ms);
    uint64_t (*now_ms)(void *ctx);
} kst_transport_t;

typedef void (*kst_event_callback_t)(void *user, const kst_event_t *event);

typedef struct {
    uint32_t received_frames;
    uint32_t invalid_frames;
    uint32_t ignored_frames;
    uint32_t drained_frames;
    uint32_t timeouts;
    uint32_t aborts;
} kst_servo_stats_t;

/* Caller-owned storage; no heap allocation, worker, thread or timer is created.
 * One task owns a client AND its RX queue. Do not call from another task or
 * re-enter from event_callback; callback runs synchronously inside this API.
 * Each operation drains at most 64 queued frames before sending. A queue that
 * does not empty returns ERR_RX_BUSY rather than discarding an unbounded flood.
 * No protocol transaction ID exists: draining reduces stale-reply risk but
 * cannot disambiguate an old same-index reply arriving after a new request.
 */
typedef struct {
    uint8_t node;
    kst_transport_t transport;
    uint32_t timeout_ms;
    kst_event_callback_t event_callback;
    void *user;
    uint32_t last_abort;
    uint16_t last_abort_index;
    uint8_t last_abort_subindex;
    kst_servo_stats_t stats;
    bool busy;
    bool initialized;
} kst_servo_t;

kst_result_t kst_servo_init(kst_servo_t *client, uint8_t node,
                           const kst_transport_t *transport, uint32_t timeout_ms,
                           kst_event_callback_t event_callback, void *user);

/* Read output can be NULL; otherwise written only on success. Every valid
 * incoming event, including unsolicited or stale events, also reaches callback.
 */
kst_result_t kst_servo_read_node(kst_servo_t *client, kst_event_t *out);
kst_result_t kst_servo_read_position(kst_servo_t *client, kst_event_t *out);
kst_result_t kst_servo_read_status(kst_servo_t *client, kst_event_t *out);
kst_result_t kst_servo_read_features(kst_servo_t *client, kst_event_t *out);
kst_result_t kst_servo_read_version(kst_servo_t *client, kst_event_t *out);
kst_result_t kst_servo_read_limit(kst_servo_t *client, bool positive, kst_event_t *out);

/* Enable waits for a new valid POSITION_REPORT, not a fabricated write ACK.
 * Disable returns SENT_UNCONFIRMED (no application ACK is defined).
 */
kst_result_t kst_servo_set_reporting(kst_servo_t *client, bool enabled);
kst_result_t kst_servo_set_report_interval(kst_servo_t *client, uint16_t milliseconds);
/* These stage configuration: ACK is not proof of activation. Explicit save
 * plus servo restart is required. client.node and transport are NOT changed.
 */
kst_result_t kst_servo_set_baudrate(kst_servo_t *client, kst_baudrate_t baudrate);
kst_result_t kst_servo_set_node_id(kst_servo_t *client, uint8_t new_node);
kst_result_t kst_servo_set_limit(kst_servo_t *client, bool positive, int16_t position_deci_deg);
/* Position and save return SENT_UNCONFIRMED after successful CAN send; neither
 * has an application ACK. Later ABORTs can be collected by poll/read calls.
 */
kst_result_t kst_servo_set_position(kst_servo_t *client, int16_t position_deci_deg);
kst_result_t kst_servo_save(kst_servo_t *client);
kst_result_t kst_servo_set_center(kst_servo_t *client);
/* Reads the complete feature value, preserves reserved low-byte bits, writes
 * chosen bits, checks ACK,
 * then reads back and verifies only the requested mask. No automatic SAVE.
 * mask: nonzero subset of 0x60. values: subset of mask. Up to three operations,
 * each with timeout_ms; no automatic retry of a configuration write.
 * If the read value has a nonzero high byte, returns ERR_UNSUPPORTED_FEATURES
 * after that read without sending any write: its meaning/write format is not
 * documented. feature_bits remains available for low-byte read-only display.
 */
kst_result_t kst_servo_set_features(kst_servo_t *client, uint8_t mask, uint8_t values);

/* Collect one legal event within timeout (0 is a nonblocking attempt).
 * Unrelated/malformed frames never extend the absolute wait budget.
 */
kst_result_t kst_servo_poll(kst_servo_t *client, uint32_t timeout_ms, kst_event_t *out);
const char *kst_servo_result_description(kst_result_t result);

#ifdef __cplusplus
}
#endif
#endif
