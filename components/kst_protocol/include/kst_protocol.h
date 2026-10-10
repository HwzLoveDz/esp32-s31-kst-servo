#ifndef KST_PROTOCOL_H
#define KST_PROTOCOL_H

/* KST extended-frame CAN protocol V3.61B (2022-05-19).
 * Transport independent: every constructed frame MUST be sent as a classical
 * CAN 2.0B 29-bit extended DATA frame (never standard, RTR or CAN FD).
 * The numeric IDs are small, but their wire format is still EXTENDED.
 * Integer payloads are little endian; position is signed 0.1 degree.
 * All constructors reject node 0 (broadcast) and nodes above 127.
 */
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define KST_DEFAULT_NODE_ID UINT8_C(0x25)
#define KST_DEFAULT_BITRATE UINT32_C(500000)
#define KST_FEATURE_UNDERVOLTAGE UINT8_C(0x20)
#define KST_FEATURE_REVERSE UINT8_C(0x40)
#define KST_FEATURE_WRITABLE_MASK UINT8_C(0x60)
/* Documented target and device-limit range; never widened by feedback support. */
#define KST_POSITION_MIN (-1000)
#define KST_POSITION_MAX 1000
/* Single-turn telemetry acceptance only: not proof of writable travel range. */
#define KST_POSITION_FEEDBACK_MIN (-1800)
#define KST_POSITION_FEEDBACK_MAX 1800

/* Object indexes are also useful for matching responses to pending requests. */
#define KST_INDEX_REPORT_INTERVAL UINT16_C(0x2200)
#define KST_INDEX_NODE_ID UINT16_C(0x3000)
#define KST_INDEX_BAUDRATE UINT16_C(0x3001)
#define KST_INDEX_CENTER UINT16_C(0x3009)
#define KST_INDEX_LIMIT_NEGATIVE UINT16_C(0x300A)
#define KST_INDEX_LIMIT_POSITIVE UINT16_C(0x300B)
#define KST_INDEX_FEATURES UINT16_C(0x300F)
#define KST_INDEX_SAVE UINT16_C(0x1010)
#define KST_INDEX_VERSION UINT16_C(0x100B)
#define KST_INDEX_POSITION UINT16_C(0x6002)
#define KST_INDEX_POSITION_TARGET UINT16_C(0x6003)
#define KST_INDEX_STATUS UINT16_C(0x6005)

typedef struct {
    uint32_t id;
    uint8_t len;
    uint8_t data[8];
} kst_can_frame_t;

typedef enum {
    KST_BAUD_20K = 0, KST_BAUD_50K, KST_BAUD_100K, KST_BAUD_125K,
    KST_BAUD_250K, KST_BAUD_500K, KST_BAUD_800K, KST_BAUD_1000K
} kst_baudrate_t;

typedef enum {
    KST_EVENT_POSITION,
    KST_EVENT_POSITION_REPORT,
    KST_EVENT_STATUS,
    KST_EVENT_UNDERVOLTAGE,
    KST_EVENT_WRITE_ACK,
    KST_EVENT_NODE_ID,
    KST_EVENT_FEATURES,
    KST_EVENT_LIMIT_NEGATIVE,
    KST_EVENT_LIMIT_POSITIVE,
    KST_EVENT_VERSION,
    KST_EVENT_ABORT
} kst_event_type_t;

typedef struct {
    kst_event_type_t type;
    uint8_t node;
    uint16_t index;
    uint8_t subindex;
    int16_t position_deci_deg;
    uint16_t current_10ma;
    int8_t temperature_c;
    uint8_t feature_bits; /* Low byte only: documented V3.61B feature bits. */
    uint8_t node_id;
    char version[5]; /* Four protocol bytes, followed by a local NUL. */
    uint32_t abort_code; /* Canonical documented code, or unchanged unknown LE value. */
    /* Complete little-endian FEATURES value. Firmware 3.66 was observed to
     * return a nonzero high byte; its meaning and write format are unknown.
     * Keep this raw value for diagnostics and pass it to kst_make_features.
     */
    uint16_t feature_raw;
    /* ABORT bytes 4..7, always decoded LE exactly as received. Raw wire bytes
     * can be reconstructed by shifting 0, 8, 16 and 24 bits, respectively. */
    uint32_t abort_wire_le;
    bool abort_byte_swapped; /* true only when unknown LE matched a known BE code. */
} kst_event_t;

typedef enum {
    KST_PARSE_OK = 0,
    KST_PARSE_IGNORED, /* A different node or unrelated CAN ID. */
    KST_PARSE_INVALID  /* Bad flags, DLC, command, index or reserved bytes. */
} kst_parse_result_t;

/* Return false for invalid input, leaving *out unchanged. No I/O is performed.
 * Enabling reporting: first report is the only documented success indication.
 * Disabling reporting: absence of reports is NOT an acknowledged transaction.
 */
bool kst_make_reporting(uint8_t node, bool enable, kst_can_frame_t *out);

/* 10..255 ms. Effective immediately; SAVE is needed for persistence. */
bool kst_make_report_interval(uint8_t node, uint16_t milliseconds,
                              kst_can_frame_t *out);
/* Baudrate and node changes require SAVE, then a servo restart to take effect.
 * Acknowledgement is still addressed using the current node and bitrate.
 */
bool kst_make_baudrate(uint8_t node, kst_baudrate_t baudrate, kst_can_frame_t *out);
uint32_t kst_baudrate_bps(kst_baudrate_t baudrate); /* 0 for invalid enum. */
bool kst_make_node_id(uint8_t node, uint8_t new_node, kst_can_frame_t *out);

/* -1000..1000 means -100..100 degrees. Success has NO application ACK.
 * A configured narrower range may cause an ABORT instead of motion.
 * Read actual position separately; a CAN hardware ACK does not prove motion.
 */
bool kst_make_position(uint8_t node, int16_t position_deci_deg, kst_can_frame_t *out);
/* Sets CURRENT position as centre. Intended for fine adjustment (typically
 * within +/-10 degrees); changes calibration immediately, SAVE persists it.
 */
bool kst_make_center(uint8_t node, kst_can_frame_t *out);
/* Persists ALL modified parameters, not just the last one. NO application ACK.
 * Flash completion time is not specified by this protocol document.
 */
bool kst_make_save(uint8_t node, kst_can_frame_t *out);

bool kst_make_read_node(uint8_t node, kst_can_frame_t *out);
bool kst_make_read_position(uint8_t node, kst_can_frame_t *out);
bool kst_make_read_status(uint8_t node, kst_can_frame_t *out);
bool kst_make_read_features(uint8_t node, kst_can_frame_t *out);
bool kst_make_read_version(uint8_t node, kst_can_frame_t *out);

/* READ the current feature value first, then pass event.feature_raw here.
 * A nonzero high byte is an undocumented layout: reject the write rather than
 * discarding it or guessing a 16-bit write format. Existing 8-bit values remain
 * source compatible. Only bits 5
 * (undervoltage alarm) and 6 (reported-angle reversal) may be changed.
 * All reserved bits are preserved. mask must be a nonzero subset of 0x60;
 * values must contain only bits selected by mask. Effective immediately;
 * SAVE persists it. This reversal feature is documented for reported angle;
 * do not assume it reverses physical motion or command polarity.
 */
bool kst_make_features(uint8_t node, uint16_t current, uint8_t mask,
                       uint8_t values, kst_can_frame_t *out);

/* Negative limit: -1000..0; positive: 0..1000, units 0.1 degree.
 * Limits require SAVE, then restart to take effect. These configure persistent
 * mechanical travel constraints and are not a substitute for host limits.
 */
bool kst_make_limit(uint8_t node, bool positive, int16_t position_deci_deg,
                    kst_can_frame_t *out);
bool kst_make_read_limit(uint8_t node, bool positive, kst_can_frame_t *out);

/* Parse only frames addressed to expected_node. Output is unchanged unless OK.
 * Exact ID, classic/extended/data flags, DLC, response command/index/subindex,
 * fixed/reserved bytes and object-specific data ranges are validated.
 * POSITION reads/reports accept single-turn -1800..1800 telemetry; target writes
 * and device-limit reads/writes retain the documented -1000..1000 range.
 * Values beyond the feedback range (including INT16_MIN) are rejected, so a
 * caller can safely reverse a valid position's sign in int16_t storage.
 * FEATURES 0x4B replies retain the low 16 bits; bytes 6/7 must still be zero.
 * Negative-limit replies also accept FF FF sign extension, but only for valid
 * strictly negative values. Position/positive-limit tails remain zero-only.
 * Position target and SAVE ACKs are rejected: V3.61B defines no such ACKs.
 * ABORT carries its object index; match that against a pending request.
 * Known LE abort codes take precedence. If LE is unknown but BE is an exact
 * documented code, abort_code uses BE and abort_byte_swapped is true. Unknown
 * values stay LE; abort_wire_le always preserves the received bytes.
 */
kst_parse_result_t kst_parse(uint8_t expected_node, const kst_can_frame_t *frame,
                             bool extended, bool rtr, bool fd, kst_event_t *out);
/* Static English description; unknown codes remain available in abort_code. */
const char *kst_abort_description(uint32_t code);

/* Deliberate support boundary: no firmware update packets are specified in
 * V3.61B (it refers to a separate update document); none are invented here.
 */
#ifdef __cplusplus
}
#endif
#endif
