#include "kst_protocol.h"

#include <stddef.h>
#include <string.h>

static const char *known_abort_description(uint32_t code);

static bool valid_node(uint8_t node)
{
    return node >= 1 && node <= 127;
}

static uint16_t get_u16(const uint8_t *bytes)
{
    return (uint16_t)((uint16_t)bytes[0] | ((uint16_t)bytes[1] << 8));
}

static int16_t get_i16(const uint8_t *bytes)
{
    const uint16_t value = get_u16(bytes);
    /* Avoid implementation-defined unsigned-to-signed narrowing. */
    return (int16_t)((value <= 32767U) ? (int32_t)value : (int32_t)value - 65536);
}

static int8_t get_i8(uint8_t value)
{
    return (int8_t)((value <= 127U) ? (int16_t)value : (int16_t)value - 256);
}

static uint32_t get_u32(const uint8_t *bytes)
{
    return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) |
           ((uint32_t)bytes[2] << 16) | ((uint32_t)bytes[3] << 24);
}

static bool make_object(uint8_t node, uint8_t command, uint16_t index,
                        uint8_t subindex, uint32_t value, kst_can_frame_t *out)
{
    if (!valid_node(node) || out == NULL) {
        return false;
    }
    const kst_can_frame_t frame = {
        .id = UINT32_C(0x600) + node,
        .len = 8,
        .data = {command, (uint8_t)index, (uint8_t)(index >> 8), subindex,
                 (uint8_t)value, (uint8_t)(value >> 8),
                 (uint8_t)(value >> 16), (uint8_t)(value >> 24)}
    };
    *out = frame;
    return true;
}

bool kst_make_reporting(uint8_t node, bool enable, kst_can_frame_t *out)
{
    if (!valid_node(node) || out == NULL) {
        return false;
    }
    const kst_can_frame_t frame = {
        .id = node, .len = 2, .data = {enable ? 0x01 : 0x02, 0x00}
    };
    *out = frame;
    return true;
}

bool kst_make_report_interval(uint8_t node, uint16_t milliseconds,
                              kst_can_frame_t *out)
{
    return milliseconds >= 10 && milliseconds <= 255 &&
           make_object(node, 0x22, KST_INDEX_REPORT_INTERVAL, 0, milliseconds, out);
}

uint32_t kst_baudrate_bps(kst_baudrate_t baudrate)
{
    static const uint32_t rates[] = {
        20000, 50000, 100000, 125000, 250000, 500000, 800000, 1000000
    };
    return (unsigned)baudrate < sizeof(rates) / sizeof(rates[0]) ? rates[baudrate] : 0;
}

bool kst_make_baudrate(uint8_t node, kst_baudrate_t baudrate, kst_can_frame_t *out)
{
    return kst_baudrate_bps(baudrate) != 0 &&
           make_object(node, 0x22, KST_INDEX_BAUDRATE, 0, (uint32_t)baudrate, out);
}

bool kst_make_node_id(uint8_t node, uint8_t new_node, kst_can_frame_t *out)
{
    return valid_node(new_node) &&
           make_object(node, 0x22, KST_INDEX_NODE_ID, 0, new_node, out);
}

bool kst_make_position(uint8_t node, int16_t position_deci_deg, kst_can_frame_t *out)
{
    return position_deci_deg >= KST_POSITION_MIN && position_deci_deg <= KST_POSITION_MAX &&
           make_object(node, 0x22, KST_INDEX_POSITION_TARGET, 0,
                       (uint16_t)position_deci_deg, out);
}

bool kst_make_center(uint8_t node, kst_can_frame_t *out)
{
    return make_object(node, 0x22, KST_INDEX_CENTER, 0, 0, out);
}

bool kst_make_save(uint8_t node, kst_can_frame_t *out)
{
    return make_object(node, 0x22, KST_INDEX_SAVE, 1, UINT32_C(0x65766173), out);
}

bool kst_make_read_node(uint8_t node, kst_can_frame_t *out)
{
    return make_object(node, 0x40, KST_INDEX_NODE_ID, 0, 0, out);
}

bool kst_make_read_position(uint8_t node, kst_can_frame_t *out)
{
    return make_object(node, 0x40, KST_INDEX_POSITION, 0, 0, out);
}

bool kst_make_read_status(uint8_t node, kst_can_frame_t *out)
{
    return make_object(node, 0x40, KST_INDEX_STATUS, 0, 0, out);
}

bool kst_make_read_features(uint8_t node, kst_can_frame_t *out)
{
    return make_object(node, 0x40, KST_INDEX_FEATURES, 0, 0, out);
}

bool kst_make_read_version(uint8_t node, kst_can_frame_t *out)
{
    return make_object(node, 0x40, KST_INDEX_VERSION, 0, 0, out);
}

bool kst_make_features(uint8_t node, uint16_t current, uint8_t mask,
                       uint8_t values, kst_can_frame_t *out)
{
    if ((current & UINT16_C(0xFF00)) != 0 ||
        mask == 0 || (mask & (uint8_t)~KST_FEATURE_WRITABLE_MASK) != 0 ||
        (values & (uint8_t)~mask) != 0) {
        return false;
    }
    const uint8_t updated = (uint8_t)(((uint8_t)current & (uint8_t)~mask) | values);
    return make_object(node, 0x22, KST_INDEX_FEATURES, 0, updated, out);
}

bool kst_make_limit(uint8_t node, bool positive, int16_t position_deci_deg,
                    kst_can_frame_t *out)
{
    const bool valid = positive ? position_deci_deg >= 0 && position_deci_deg <= 1000
                                : position_deci_deg >= -1000 && position_deci_deg <= 0;
    return valid && make_object(node, 0x22,
                               positive ? KST_INDEX_LIMIT_POSITIVE : KST_INDEX_LIMIT_NEGATIVE,
                               0, (uint16_t)position_deci_deg, out);
}

bool kst_make_read_limit(uint8_t node, bool positive, kst_can_frame_t *out)
{
    return make_object(node, 0x40,
                       positive ? KST_INDEX_LIMIT_POSITIVE : KST_INDEX_LIMIT_NEGATIVE,
                       0, 0, out);
}

static bool known_index(uint16_t index, uint8_t subindex)
{
    if (index == KST_INDEX_SAVE) {
        return subindex == 1;
    }
    if (subindex != 0) {
        return false;
    }
    switch (index) {
    case KST_INDEX_REPORT_INTERVAL:
    case KST_INDEX_NODE_ID:
    case KST_INDEX_BAUDRATE:
    case KST_INDEX_CENTER:
    case KST_INDEX_LIMIT_NEGATIVE:
    case KST_INDEX_LIMIT_POSITIVE:
    case KST_INDEX_FEATURES:
    case KST_INDEX_VERSION:
    case KST_INDEX_POSITION:
    case KST_INDEX_POSITION_TARGET:
    case KST_INDEX_STATUS:
        return true;
    default:
        return false;
    }
}

static bool acknowledged_index(uint16_t index)
{
    switch (index) {
    case KST_INDEX_REPORT_INTERVAL:
    case KST_INDEX_BAUDRATE:
    case KST_INDEX_NODE_ID:
    case KST_INDEX_CENTER:
    case KST_INDEX_FEATURES:
    case KST_INDEX_LIMIT_NEGATIVE:
    case KST_INDEX_LIMIT_POSITIVE:
        return true;
    default:
        return false;
    }
}

static bool valid_documented_position(int16_t position)
{
    return position >= KST_POSITION_MIN && position <= KST_POSITION_MAX;
}

static bool valid_feedback_position(int16_t position)
{
    return position >= KST_POSITION_FEEDBACK_MIN && position <= KST_POSITION_FEEDBACK_MAX;
}

kst_parse_result_t kst_parse(uint8_t expected_node, const kst_can_frame_t *frame,
                             bool extended, bool rtr, bool fd, kst_event_t *out)
{
    if (!valid_node(expected_node) || frame == NULL || out == NULL) {
        return KST_PARSE_INVALID;
    }
    if (frame->id != UINT32_C(0x580) + expected_node &&
        frame->id != UINT32_C(0x380) + expected_node) {
        return KST_PARSE_IGNORED;
    }
    if (!extended || rtr || fd || frame->len > 8) {
        return KST_PARSE_INVALID;
    }
    const uint8_t *d = frame->data;
    kst_event_t event = {0};
    event.node = expected_node;

    if (frame->id == UINT32_C(0x380) + expected_node) {
        if (frame->len != 4 || d[0] != 0x0A || d[1] != 0x02 || d[2] != 0 || d[3] != 0) {
            return KST_PARSE_INVALID;
        }
        event.type = KST_EVENT_UNDERVOLTAGE;
    } else if (frame->len == 4) {
        event.position_deci_deg = get_i16(d);
        if (d[2] != 0 || d[3] != 0 || !valid_feedback_position(event.position_deci_deg)) {
            return KST_PARSE_INVALID;
        }
        event.type = KST_EVENT_POSITION_REPORT;
    } else {
        if (frame->len != 8) {
            return KST_PARSE_INVALID;
        }
        event.index = get_u16(d + 1);
        event.subindex = d[3];
        if (!known_index(event.index, event.subindex)) {
            return KST_PARSE_INVALID;
        }
        if (d[0] == 0x80) {
            event.type = KST_EVENT_ABORT;
            event.abort_wire_le = get_u32(d + 4);
            event.abort_code = event.abort_wire_le;
            /* Observed 3.66 replies use BE abort bytes. Prefer known LE codes;
             * normalize only an exact known BE code, leaving unknown data raw. */
            if (known_abort_description(event.abort_code) == NULL) {
                const uint32_t be_code = (uint32_t)d[4] << 24 | (uint32_t)d[5] << 16 |
                                         (uint32_t)d[6] << 8 | (uint32_t)d[7];
                if (known_abort_description(be_code) != NULL) {
                    event.abort_code = be_code;
                    event.abort_byte_swapped = true;
                }
            }
        } else if (d[0] == 0x60) {
            if (event.subindex != 0 || !acknowledged_index(event.index) || get_u32(d + 4) != 0) {
                return KST_PARSE_INVALID;
            }
            event.type = KST_EVENT_WRITE_ACK;
        } else if (d[0] == 0x4B &&
                   (event.index == KST_INDEX_POSITION || event.index == KST_INDEX_LIMIT_NEGATIVE ||
                    event.index == KST_INDEX_LIMIT_POSITIVE)) {
            event.position_deci_deg = get_i16(d + 4);
            const bool zero_tail = d[6] == 0 && d[7] == 0;
            /* Firmware 3.66 sign-extends negative-limit replies only. Keep
             * the documented zero tail and accept exactly FF FF for a valid
             * negative value; do not broaden position or positive limits. */
            const bool negative_limit_sign_extension =
                event.index == KST_INDEX_LIMIT_NEGATIVE && event.position_deci_deg < 0 &&
                d[6] == 0xFF && d[7] == 0xFF;
            const bool valid_range = event.index == KST_INDEX_POSITION ?
                valid_feedback_position(event.position_deci_deg) :
                valid_documented_position(event.position_deci_deg);
            if ((!zero_tail && !negative_limit_sign_extension) || !valid_range ||
                (event.index == KST_INDEX_LIMIT_NEGATIVE && event.position_deci_deg > 0) ||
                (event.index == KST_INDEX_LIMIT_POSITIVE && event.position_deci_deg < 0)) {
                return KST_PARSE_INVALID;
            }
            event.type = event.index == KST_INDEX_POSITION ? KST_EVENT_POSITION :
                         event.index == KST_INDEX_LIMIT_NEGATIVE ? KST_EVENT_LIMIT_NEGATIVE :
                         KST_EVENT_LIMIT_POSITIVE;
        } else if (d[0] == 0x43 && event.index == KST_INDEX_STATUS) {
            event.current_10ma = get_u16(d + 4);
            event.temperature_c = get_i8(d[6]);
            if (d[7] != 0 || event.temperature_c < -40 || event.temperature_c > 120) {
                return KST_PARSE_INVALID;
            }
            event.type = KST_EVENT_STATUS;
        } else if (d[0] == 0x4F && event.index == KST_INDEX_NODE_ID) {
            if (!valid_node(d[4]) || d[5] != 0 || d[6] != 0 || d[7] != 0) {
                return KST_PARSE_INVALID;
            }
            event.type = KST_EVENT_NODE_ID;
            event.node_id = d[4];
        } else if (d[0] == 0x4B && event.index == KST_INDEX_FEATURES) {
            if (d[6] != 0 || d[7] != 0) {
                return KST_PARSE_INVALID;
            }
            event.type = KST_EVENT_FEATURES;
            event.feature_bits = d[4];
            event.feature_raw = get_u16(d + 4);
        } else if (d[0] == 0x43 && event.index == KST_INDEX_VERSION) {
            event.type = KST_EVENT_VERSION;
            memcpy(event.version, d + 4, 4);
            event.version[4] = '\0';
        } else {
            return KST_PARSE_INVALID;
        }
    }
    *out = event;
    return KST_PARSE_OK;
}

static const char *known_abort_description(uint32_t code)
{
    switch (code) {
    case UINT32_C(0x05030000): return "Toggle bit not alternated";
    case UINT32_C(0x05040000): return "SDO protocol timeout";
    case UINT32_C(0x05040001): return "Invalid or unknown command specifier";
    case UINT32_C(0x05040002): return "Invalid block size";
    case UINT32_C(0x05040003): return "Invalid sequence number";
    case UINT32_C(0x05040004): return "CRC error";
    case UINT32_C(0x05040005): return "Out of memory";
    case UINT32_C(0x06010000): return "Unsupported object access";
    case UINT32_C(0x06010001): return "Attempt to read a write-only object";
    case UINT32_C(0x06010002): return "Attempt to write a read-only object";
    case UINT32_C(0x06020000): return "Object does not exist";
    case UINT32_C(0x06040041): return "Object cannot be mapped to PDO";
    case UINT32_C(0x06040042): return "Mapped objects exceed PDO length";
    case UINT32_C(0x06040043): return "General parameter incompatibility";
    case UINT32_C(0x06040047): return "General internal incompatibility";
    case UINT32_C(0x06060000): return "Access failed due to hardware error";
    case UINT32_C(0x06070010): return "Data type or service length mismatch";
    case UINT32_C(0x06070012): return "Service length too high";
    case UINT32_C(0x06070013): return "Service length too low";
    case UINT32_C(0x06090011): return "Subindex does not exist";
    case UINT32_C(0x06090030): return "Parameter value outside allowed range";
    case UINT32_C(0x06090031): return "Parameter value too high";
    case UINT32_C(0x06090032): return "Parameter value too low";
    case UINT32_C(0x06090036): return "Maximum value less than minimum value";
    case UINT32_C(0x08000000): return "General error";
    case UINT32_C(0x08000020): return "Cannot transfer or store data in application";
    case UINT32_C(0x08000021): return "Data transfer blocked by local control";
    case UINT32_C(0x08000022): return "Data transfer blocked by device state";
    case UINT32_C(0x08000023): return "Object dictionary unavailable";
    default: return NULL;
    }
}

const char *kst_abort_description(uint32_t code)
{
    const char *description = known_abort_description(code);
    return description != NULL ? description : "Unknown KST abort code";
}
