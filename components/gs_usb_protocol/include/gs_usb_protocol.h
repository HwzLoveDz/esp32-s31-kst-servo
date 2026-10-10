/* SPDX-License-Identifier: MIT */
#ifndef GS_USB_PROTOCOL_H
#define GS_USB_PROTOCOL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* gs_usb wire values. USB identity must be checked by the USB host first. */
enum {
    GS_USB_BREQ_HOST_FORMAT = 0, GS_USB_BREQ_BITTIMING = 1,
    GS_USB_BREQ_MODE = 2, GS_USB_BREQ_BERR = 3,
    GS_USB_BREQ_BT_CONST = 4, GS_USB_BREQ_DEVICE_CONFIG = 5,
    GS_USB_BREQ_TIMESTAMP = 6, GS_USB_BREQ_IDENTIFY = 7,
    GS_USB_BREQ_GET_STATE = 14
};
#define GS_USB_FEATURE_LISTEN_ONLY (UINT32_C(1) << 0)
#define GS_USB_FEATURE_LOOP_BACK (UINT32_C(1) << 1)
#define GS_USB_FEATURE_TRIPLE_SAMPLE (UINT32_C(1) << 2)
#define GS_USB_FEATURE_ONE_SHOT (UINT32_C(1) << 3)
#define GS_USB_FEATURE_HW_TIMESTAMP (UINT32_C(1) << 4)
#define GS_USB_FEATURE_IDENTIFY (UINT32_C(1) << 5)
#define GS_USB_FEATURE_PAD_PKTS_TO_MAX_PKT_SIZE (UINT32_C(1) << 7)
#define GS_USB_FEATURE_FD (UINT32_C(1) << 8)
#define GS_USB_FEATURE_REQ_USB_QUIRK_LPC546XX (UINT32_C(1) << 9)
#define GS_USB_FEATURE_BERR_REPORTING (UINT32_C(1) << 12)
#define GS_USB_FEATURE_GET_STATE (UINT32_C(1) << 13)
#define GS_USB_MODE_RESET UINT32_C(0)
#define GS_USB_MODE_START UINT32_C(1)
#define GS_USB_ECHO_RX UINT32_C(0xffffffff)
#define GS_USB_CAN_EFF_FLAG UINT32_C(0x80000000)
#define GS_USB_CAN_RTR_FLAG UINT32_C(0x40000000)
#define GS_USB_CAN_ERR_FLAG UINT32_C(0x20000000)
#define GS_USB_CAN_ERR_BUSOFF UINT32_C(0x40)
#define GS_USB_CAN_ERR_CRTL UINT32_C(0x04)
#define GS_USB_FLAG_OVERFLOW UINT8_C(1)
#define GS_USB_CLASSIC_FRAME_SIZE 20u
#define GS_USB_CLASSIC_FRAME_TS_SIZE 24u
#define GS_USB_DEVICE_CONFIG_SIZE 12u
#define GS_USB_BT_CONST_SIZE 40u
#define GS_USB_BITTIMING_SIZE 20u
#define GS_USB_MODE_SIZE 8u
#define GS_USB_HOST_FORMAT_SIZE 4u

typedef struct {
    uint8_t interface_number, alternate_setting;
    uint8_t endpoint_in, endpoint_out;
    uint16_t max_packet_in, max_packet_out;
} gs_usb_interface_t;

typedef struct {
    uint16_t channel_count; /* The wire icount is last channel index, not count. */
    uint32_t software_version, hardware_version;
} gs_usb_device_config_t;

typedef struct {
    uint32_t features, clock_hz;
    uint32_t tseg1_min, tseg1_max, tseg2_min, tseg2_max;
    uint32_t sjw_max, brp_min, brp_max, brp_inc;
} gs_usb_bt_const_t;

typedef struct {
    uint32_t prop_seg, phase_seg1, phase_seg2, sjw, brp;
    uint32_t bitrate;
    uint16_t sample_point_permille;
} gs_usb_bittiming_t;

typedef struct {
    uint32_t echo_id, can_id; /* can_id excludes EFF/RTR/ERR flag bits. */
    uint8_t dlc, channel, data[8];
    bool extended, rtr, error, overflow;
    bool timestamp_valid;
    uint32_t timestamp_us;
} gs_usb_frame_t;

uint16_t gs_usb_get_le16(const uint8_t *p);
uint32_t gs_usb_get_le32(const uint8_t *p);
void gs_usb_put_le16(uint8_t *p, uint16_t value);
void gs_usb_put_le32(uint8_t *p, uint32_t value);

/* All fallible functions leave output unchanged on failure. Descriptor parser
 * requires a complete configuration descriptor, validates every record and
 * chooses exactly one vendor interface with two Bulk endpoints at alt 0.
 * Ambiguous configurations are rejected. VID/PID allowlisting is caller-owned.
 */
bool gs_usb_parse_interface(const uint8_t *config, size_t size,
                            gs_usb_interface_t *out);
bool gs_usb_decode_device_config(const uint8_t *wire, size_t size,
                                 gs_usb_device_config_t *out);
bool gs_usb_decode_bt_const(const uint8_t *wire, size_t size,
                            gs_usb_bt_const_t *out);
bool gs_usb_encode_host_format(uint8_t *wire, size_t size);

/* Exact bitrate only; sample point is chosen nearest target (1..999 permille).
 * Ties prefer more time quanta. SJW=1. Limits and integer arithmetic validated.
 */
bool gs_usb_find_bittiming(const gs_usb_bt_const_t *limits, uint32_t bitrate,
                           uint16_t target_sample_point_permille,
                           gs_usb_bittiming_t *out);
bool gs_usb_encode_bittiming(const gs_usb_bittiming_t *timing,
                             uint8_t *wire, size_t size);
/* Requested mode flags must be supported feature bits. Only classic flags
 * LISTEN_ONLY/LOOP_BACK/TRIPLE_SAMPLE/ONE_SHOT/HW_TIMESTAMP/BERR_REPORTING are
 * accepted. RESET requires flags=0. ONE_SHOT must be advertised by the device;
 * never silently drop it and enable automatic retries for actuator commands.
 */
bool gs_usb_encode_mode(uint32_t mode, uint32_t flags, uint32_t features,
                        uint8_t *wire, size_t size);

/* TX is always 20 bytes, no timestamp. Requires a real echo id (!=ECHO_RX),
 * classic DATA/RTR, DLC<=8, valid identifier; errors/overflow are RX-only.
 * RX accepts exactly 20 or 24 according to negotiated HW_TIMESTAMP mode.
 * Unknown flags, FD, malformed IDs/errors, nonzero reserved, wrong channel and
 * wrong length are rejected. Frames with echo_id!=ECHO_RX are transport echoes,
 * NOT a guarantee of CAN bus ACK or servo execution.
 */
bool gs_usb_encode_frame(const gs_usb_frame_t *frame, uint8_t *wire, size_t size);
bool gs_usb_decode_frame(const uint8_t *wire, size_t size, uint8_t expected_channel,
                          bool hardware_timestamp, gs_usb_frame_t *out);

#ifdef __cplusplus
}
#endif
#endif
