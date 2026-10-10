/* SPDX-License-Identifier: MIT
 * Independently implemented byte codecs for the public gs_usb protocol.
 */
#include "gs_usb_protocol.h"

#include <string.h>

uint16_t gs_usb_get_le16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

uint32_t gs_usb_get_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

void gs_usb_put_le16(uint8_t *p, uint16_t value)
{
    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8);
}

void gs_usb_put_le32(uint8_t *p, uint32_t value)
{
    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8);
    p[2] = (uint8_t)(value >> 16);
    p[3] = (uint8_t)(value >> 24);
}

bool gs_usb_parse_interface(const uint8_t *config, size_t size,
                            gs_usb_interface_t *out)
{
    gs_usb_interface_t candidate = {0}, selected = {0};
    bool have_interface = false, wanted = false;
    unsigned expected = 0, endpoints = 0, matches = 0;
    uint32_t seen_addresses = 0;
    size_t pos;
    if (!config || !out || size < 9 || config[0] != 9 || config[1] != 2 ||
        gs_usb_get_le16(config + 2) != size || config[4] == 0 || config[5] == 0)
        return false;

    for (pos = 9; pos < size;) {
        uint8_t length, type;
        if (size - pos < 2) return false;
        length = config[pos];
        type = config[pos + 1];
        if (length < 2 || length > size - pos) return false;
        if (type == 4) {
            if (length < 9) return false;
            if (have_interface && endpoints != expected) return false;
            if (wanted) {
                if (candidate.endpoint_in == 0 || candidate.endpoint_out == 0)
                    return false;
                selected = candidate;
                ++matches;
            }
            memset(&candidate, 0, sizeof(candidate));
            candidate.interface_number = config[pos + 2];
            candidate.alternate_setting = config[pos + 3];
            expected = config[pos + 4];
            endpoints = 0;
            seen_addresses = 0;
            have_interface = true;
            wanted = config[pos + 3] == 0 && config[pos + 5] == 0xff && expected == 2;
        } else if (type == 5) {
            uint8_t address, number, attributes;
            uint16_t packet;
            uint32_t address_bit;
            if (length < 7 || !have_interface) return false;
            address = config[pos + 2];
            number = address & 0x0f;
            attributes = config[pos + 3];
            packet = gs_usb_get_le16(config + pos + 4);
            if (number == 0 || (address & 0x70) != 0) return false;
            address_bit = UINT32_C(1) << (number + ((address & 0x80) ? 16 : 0));
            if ((seen_addresses & address_bit) != 0) return false;
            seen_addresses |= address_bit;
            ++endpoints;
            if (endpoints > expected) return false;
            if (wanted) {
                if (attributes != 2 || packet == 0 || packet > 512) return false;
                if (address & 0x80) {
                    if (candidate.endpoint_in) return false;
                    candidate.endpoint_in = address;
                    candidate.max_packet_in = packet;
                } else {
                    if (candidate.endpoint_out) return false;
                    candidate.endpoint_out = address;
                    candidate.max_packet_out = packet;
                }
            }
        } else if (type == 2) {
            return false; /* Nested configurations cannot occur here. */
        }
        pos += length;
    }
    if (!have_interface || endpoints != expected) return false;
    if (wanted) {
        if (candidate.endpoint_in == 0 || candidate.endpoint_out == 0) return false;
        selected = candidate;
        ++matches;
    }
    if (matches != 1) return false;
    *out = selected;
    return true;
}

bool gs_usb_decode_device_config(const uint8_t *wire, size_t size,
                                 gs_usb_device_config_t *out)
{
    gs_usb_device_config_t decoded;
    if (!wire || !out || size != GS_USB_DEVICE_CONFIG_SIZE ||
        wire[0] || wire[1] || wire[2]) return false;
    decoded.channel_count = (uint16_t)wire[3] + 1u;
    decoded.software_version = gs_usb_get_le32(wire + 4);
    decoded.hardware_version = gs_usb_get_le32(wire + 8);
    *out = decoded;
    return true;
}

static bool valid_limits(const gs_usb_bt_const_t *b)
{
    return b && b->clock_hz != 0 && b->tseg1_min != 0 &&
           b->tseg1_min <= b->tseg1_max && b->tseg2_min != 0 &&
           b->tseg2_min <= b->tseg2_max && b->sjw_max != 0 &&
           b->brp_min != 0 && b->brp_min <= b->brp_max && b->brp_inc != 0;
}

bool gs_usb_decode_bt_const(const uint8_t *wire, size_t size,
                            gs_usb_bt_const_t *out)
{
    gs_usb_bt_const_t b;
    if (!wire || !out || size != GS_USB_BT_CONST_SIZE) return false;
    b.features = gs_usb_get_le32(wire);
    b.clock_hz = gs_usb_get_le32(wire + 4);
    b.tseg1_min = gs_usb_get_le32(wire + 8);
    b.tseg1_max = gs_usb_get_le32(wire + 12);
    b.tseg2_min = gs_usb_get_le32(wire + 16);
    b.tseg2_max = gs_usb_get_le32(wire + 20);
    b.sjw_max = gs_usb_get_le32(wire + 24);
    b.brp_min = gs_usb_get_le32(wire + 28);
    b.brp_max = gs_usb_get_le32(wire + 32);
    b.brp_inc = gs_usb_get_le32(wire + 36);
    if (!valid_limits(&b)) return false;
    *out = b;
    return true;
}

bool gs_usb_encode_host_format(uint8_t *wire, size_t size)
{
    if (!wire || size != GS_USB_HOST_FORMAT_SIZE) return false;
    gs_usb_put_le32(wire, UINT32_C(0x0000beef));
    return true;
}

bool gs_usb_find_bittiming(const gs_usb_bt_const_t *b, uint32_t bitrate,
                           uint16_t target, gs_usb_bittiming_t *out)
{
    gs_usb_bittiming_t best = {0};
    uint32_t ticks, brp, best_tq = 0, best_error = 0;
    if (!out || !valid_limits(b) || bitrate == 0 || target == 0 || target >= 1000 ||
        b->tseg1_max > 1024 || b->tseg2_max > 1024 || b->brp_max > 65536 ||
        b->clock_hz % bitrate != 0) return false;
    ticks = b->clock_hz / bitrate;
    for (brp = b->brp_min; brp <= b->brp_max;) {
        if (ticks % brp == 0) {
            uint32_t tq = ticks / brp;
            if (tq >= 1u + b->tseg1_min + b->tseg2_min &&
                tq <= 1u + b->tseg1_max + b->tseg2_max) {
                uint32_t low = b->tseg2_min, high = b->tseg2_max;
                uint32_t near = tq * (1000u - target) / 1000u;
                unsigned candidate_index;
                if (tq > 1u + b->tseg1_max && low < tq - 1u - b->tseg1_max)
                    low = tq - 1u - b->tseg1_max;
                if (high > tq - 1u - b->tseg1_min)
                    high = tq - 1u - b->tseg1_min;
                for (candidate_index = 0; candidate_index != 2; ++candidate_index) {
                    uint32_t tseg2 = near + candidate_index;
                    uint32_t tseg1, sample_numerator, target_numerator, error;
                    uint64_t left, right;
                    if (tseg2 < low) tseg2 = low;
                    if (tseg2 > high) tseg2 = high;
                    tseg1 = tq - 1u - tseg2;
                    sample_numerator = (tq - tseg2) * 1000u;
                    target_numerator = tq * target;
                    error = sample_numerator > target_numerator ?
                            sample_numerator - target_numerator :
                            target_numerator - sample_numerator;
                    left = (uint64_t)error * best_tq;
                    right = (uint64_t)best_error * tq;
                    if (best_tq == 0 || left < right ||
                        (left == right && tq > best_tq)) {
                        best.prop_seg = tseg1 / 2u;
                        best.phase_seg1 = tseg1 - best.prop_seg;
                        best.phase_seg2 = tseg2;
                        best.sjw = 1;
                        best.brp = brp;
                        best.bitrate = bitrate;
                        best.sample_point_permille = (uint16_t)
                            ((sample_numerator + tq / 2u) / tq);
                        best_error = error;
                        best_tq = tq;
                    }
                }
            }
        }
        if (b->brp_inc > b->brp_max - brp) break;
        brp += b->brp_inc;
    }
    if (best_tq == 0) return false;
    *out = best;
    return true;
}

bool gs_usb_encode_bittiming(const gs_usb_bittiming_t *b, uint8_t *wire, size_t size)
{
    if (!b || !wire || size != GS_USB_BITTIMING_SIZE || !b->phase_seg1 ||
        !b->phase_seg2 || !b->sjw || b->sjw > b->phase_seg2 || !b->brp ||
        b->prop_seg > UINT32_MAX - b->phase_seg1) return false;
    gs_usb_put_le32(wire, b->prop_seg);
    gs_usb_put_le32(wire + 4, b->phase_seg1);
    gs_usb_put_le32(wire + 8, b->phase_seg2);
    gs_usb_put_le32(wire + 12, b->sjw);
    gs_usb_put_le32(wire + 16, b->brp);
    return true;
}

bool gs_usb_encode_mode(uint32_t mode, uint32_t flags, uint32_t features,
                        uint8_t *wire, size_t size)
{
    const uint32_t supported = GS_USB_FEATURE_LISTEN_ONLY | GS_USB_FEATURE_LOOP_BACK |
        GS_USB_FEATURE_TRIPLE_SAMPLE | GS_USB_FEATURE_ONE_SHOT |
        GS_USB_FEATURE_HW_TIMESTAMP | GS_USB_FEATURE_BERR_REPORTING;
    if (!wire || size != GS_USB_MODE_SIZE || mode > GS_USB_MODE_START ||
        (flags & ~supported) != 0 || (flags & ~features) != 0 ||
        (mode == GS_USB_MODE_RESET && flags != 0)) return false;
    gs_usb_put_le32(wire, mode);
    gs_usb_put_le32(wire + 4, flags);
    return true;
}

static bool valid_frame(const gs_usb_frame_t *f)
{
    if (f->dlc > 8 || f->can_id > UINT32_C(0x1fffffff)) return false;
    if (f->error) return !f->extended && !f->rtr && f->dlc == 8 &&
                          f->echo_id == GS_USB_ECHO_RX;
    return f->extended || f->can_id <= UINT32_C(0x7ff);
}

bool gs_usb_encode_frame(const gs_usb_frame_t *f, uint8_t *wire, size_t size)
{
    uint8_t encoded[GS_USB_CLASSIC_FRAME_SIZE] = {0};
    uint32_t can_id;
    if (!f || !wire || size != GS_USB_CLASSIC_FRAME_SIZE || !valid_frame(f) ||
        f->error || f->overflow || f->echo_id == GS_USB_ECHO_RX) return false;
    can_id = f->can_id;
    if (f->extended) can_id |= GS_USB_CAN_EFF_FLAG;
    if (f->rtr) can_id |= GS_USB_CAN_RTR_FLAG;
    gs_usb_put_le32(encoded, f->echo_id);
    gs_usb_put_le32(encoded + 4, can_id);
    encoded[8] = f->dlc;
    encoded[9] = f->channel;
    if (!f->rtr) memcpy(encoded + 12, f->data, f->dlc);
    memcpy(wire, encoded, sizeof(encoded));
    return true;
}

bool gs_usb_decode_frame(const uint8_t *wire, size_t size, uint8_t channel,
                          bool hardware_timestamp, gs_usb_frame_t *out)
{
    gs_usb_frame_t decoded = {0};
    uint32_t can_id;
    const size_t expected_size = hardware_timestamp ? GS_USB_CLASSIC_FRAME_TS_SIZE :
                                                     GS_USB_CLASSIC_FRAME_SIZE;
    if (!wire || !out || size != expected_size || wire[9] != channel ||
        (wire[10] & (uint8_t)~GS_USB_FLAG_OVERFLOW) != 0 || wire[11] != 0) return false;
    decoded.echo_id = gs_usb_get_le32(wire);
    can_id = gs_usb_get_le32(wire + 4);
    decoded.can_id = can_id & UINT32_C(0x1fffffff);
    decoded.extended = (can_id & GS_USB_CAN_EFF_FLAG) != 0;
    decoded.rtr = (can_id & GS_USB_CAN_RTR_FLAG) != 0;
    decoded.error = (can_id & GS_USB_CAN_ERR_FLAG) != 0;
    decoded.overflow = (wire[10] & GS_USB_FLAG_OVERFLOW) != 0;
    decoded.dlc = wire[8];
    decoded.channel = wire[9];
    if (!valid_frame(&decoded)) return false;
    if (!decoded.rtr) memcpy(decoded.data, wire + 12, decoded.dlc);
    decoded.timestamp_valid = hardware_timestamp;
    if (hardware_timestamp) decoded.timestamp_us = gs_usb_get_le32(wire + 20);
    *out = decoded;
    return true;
}
