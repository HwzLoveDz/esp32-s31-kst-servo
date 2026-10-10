/* Host-only gs_usb tests; no adapter or actuator access. */
#include "gs_usb_protocol.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned checks;
#define CHECK(expr) do { ++checks; if (!(expr)) { \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr); \
    exit(EXIT_FAILURE); } } while (0)

static const uint8_t configuration[] = {
    9, 2, 32, 0, 1, 1, 0, 0x80, 50,
    9, 4, 0, 0, 2, 0xff, 0xff, 0xff, 0,
    7, 5, 0x81, 2, 64, 0, 0,
    7, 5, 0x02, 2, 64, 0, 0
};

static const gs_usb_bt_const_t bxcan = {
    GS_USB_FEATURE_ONE_SHOT | GS_USB_FEATURE_HW_TIMESTAMP,
    48000000, 1, 16, 1, 8, 4, 1, 1024, 1
};

static void test_endian_and_controls(void)
{
    uint8_t unaligned[48] = {0};
    uint8_t *p = unaligned + 1;
    gs_usb_device_config_t config = {0};
    gs_usb_bt_const_t limits;
    const uint8_t expected_mode[] = {1, 0, 0, 0, 8, 0, 0, 0};
    const uint8_t expected_bittiming[] = {
        5,0,0,0, 6,0,0,0, 3,0,0,0, 1,0,0,0, 4,0,0,0
    };
    gs_usb_bittiming_t timing = {5,6,3,1,4,800000,800};
    gs_usb_put_le16(p, 0xabcd);
    CHECK(p[0] == 0xcd && p[1] == 0xab);
    CHECK(gs_usb_get_le16(p) == 0xabcd);
    gs_usb_put_le32(p, UINT32_C(0x89abcdef));
    CHECK(p[0] == 0xef && p[1] == 0xcd && p[2] == 0xab && p[3] == 0x89);
    CHECK(gs_usb_get_le32(p) == UINT32_C(0x89abcdef));
    CHECK(gs_usb_encode_host_format(p, 4));
    CHECK(memcmp(p, (const uint8_t[]){0xef, 0xbe, 0, 0}, 4) == 0);
    CHECK(!gs_usb_encode_host_format(p, 3));
    CHECK(!gs_usb_encode_host_format(NULL, 4));
    memset(p, 0, 40);
    p[3] = 1;
    gs_usb_put_le32(p + 4, UINT32_C(0x12345678));
    gs_usb_put_le32(p + 8, UINT32_C(0x87654321));
    CHECK(gs_usb_decode_device_config(p, 12, &config));
    CHECK(config.channel_count == 2);
    CHECK(config.software_version == UINT32_C(0x12345678));
    CHECK(config.hardware_version == UINT32_C(0x87654321));
    p[3] = 255;
    CHECK(gs_usb_decode_device_config(p, 12, &config));
    CHECK(config.channel_count == 256);
    p[0] = 1;
    CHECK(!gs_usb_decode_device_config(p, 12, &config));
    CHECK(config.channel_count == 256);
    p[0] = 0;
    CHECK(!gs_usb_decode_device_config(p, 11, &config));
    CHECK(!gs_usb_decode_device_config(p, 13, &config));
    CHECK(!gs_usb_decode_device_config(NULL, 12, &config));
    const uint32_t bt_words[] = {
        24, 48000000, 1, 16, 1, 8, 4, 1, 1024, 1
    };
    for (unsigned i = 0; i < 10; ++i) gs_usb_put_le32(p + i * 4, bt_words[i]);
    CHECK(gs_usb_decode_bt_const(p, 40, &limits));
    CHECK(limits.features == 24 && limits.clock_hz == 48000000);
    CHECK(limits.tseg1_max == 16 && limits.brp_max == 1024);
    CHECK(limits.tseg2_min == 1 && limits.sjw_max == 4 && limits.brp_inc == 1);
    gs_usb_put_le32(p + 36, 0);
    CHECK(!gs_usb_decode_bt_const(p, 40, &limits));
    CHECK(limits.brp_inc == 1);
    CHECK(!gs_usb_decode_bt_const(p, 39, &limits));
    CHECK(gs_usb_encode_mode(GS_USB_MODE_START, GS_USB_FEATURE_ONE_SHOT,
                             limits.features, p, 8));
    CHECK(memcmp(p, expected_mode, 8) == 0);
    CHECK(!gs_usb_encode_mode(GS_USB_MODE_START, GS_USB_FEATURE_ONE_SHOT, 0, p, 8));
    CHECK(memcmp(p, expected_mode, 8) == 0);
    CHECK(!gs_usb_encode_mode(GS_USB_MODE_RESET, GS_USB_FEATURE_ONE_SHOT,
                              limits.features, p, 8));
    CHECK(!gs_usb_encode_mode(2, 0, 0, p, 8));
    CHECK(!gs_usb_encode_mode(GS_USB_MODE_START, GS_USB_FEATURE_FD, UINT32_MAX, p, 8));
    CHECK(!gs_usb_encode_mode(GS_USB_MODE_START, UINT32_C(1) << 31, UINT32_MAX, p, 8));
    CHECK(gs_usb_encode_mode(GS_USB_MODE_RESET, 0, 0, p, 8));
    CHECK(memcmp(p, (const uint8_t[8]){0}, 8) == 0);
    CHECK(gs_usb_encode_bittiming(&timing, p, 20));
    CHECK(memcmp(p, expected_bittiming, 20) == 0);
    timing.sjw = 4;
    CHECK(!gs_usb_encode_bittiming(&timing, p, 20));
    CHECK(memcmp(p, expected_bittiming, 20) == 0);
    timing.sjw = 1;
    timing.prop_seg = UINT32_MAX;
    CHECK(!gs_usb_encode_bittiming(&timing, p, 20));
}

static void test_descriptors(void)
{
    uint8_t bad[80] = {0};
    gs_usb_interface_t result;
    CHECK(gs_usb_parse_interface(configuration, sizeof(configuration), &result));
    CHECK(result.interface_number == 0 && result.alternate_setting == 0);
    CHECK(result.endpoint_in == 0x81 && result.endpoint_out == 0x02);
    CHECK(result.max_packet_in == 64 && result.max_packet_out == 64);
    for (size_t n = 0; n < sizeof(configuration); ++n) {
        gs_usb_interface_t saved = result;
        CHECK(!gs_usb_parse_interface(configuration, n, &result));
        CHECK(memcmp(&result, &saved, sizeof(result)) == 0);
    }
    static const struct { size_t pos; uint8_t value; } mutations[] = {
        {0,0}, {1,1}, {2,31}, {3,1}, {4,0}, {5,0},
        {9,0}, {9,1}, {9,8}, {9,60}, {12,1}, {13,1}, {13,3}, {14,2},
        {18,0}, {18,6}, {18,255}, {20,0x80}, {20,0x91}, {21,3}, {22,0}, {23,4},
        {25,1}, {25,6}, {27,0x81}, {27,0x82}, {28,3}
    };
    for (size_t i = 0; i < sizeof(mutations) / sizeof(mutations[0]); ++i) {
        memcpy(bad, configuration, sizeof(configuration));
        bad[mutations[i].pos] = mutations[i].value;
        CHECK(!gs_usb_parse_interface(bad, sizeof(configuration), &result));
    }
    /* Duplicate vendor interfaces are ambiguous and cannot be silently chosen. */
    memcpy(bad, configuration, sizeof(configuration));
    memcpy(bad + 32, configuration + 9, 23);
    bad[2] = 55;
    bad[4] = 2;
    bad[34] = 1;
    CHECK(!gs_usb_parse_interface(bad, 55, &result));
    /* Other interfaces may coexist; their endpoints must be structurally valid. */
    bad[37] = 3;
    CHECK(gs_usb_parse_interface(bad, 55, &result));
    bad[32 + 4] = 1;
    CHECK(!gs_usb_parse_interface(bad, 55, &result));
    CHECK(!gs_usb_parse_interface(NULL, 32, &result));
    CHECK(!gs_usb_parse_interface(configuration, 32, NULL));
}

static void test_frames(void)
{
    uint8_t wire[25] = {0};
    uint8_t *p = wire + 1; /* Force unaligned wire storage. */
    gs_usb_frame_t tx = {0}, decoded = {0};
    const uint8_t golden[] = {
        0x04,0x03,0x02,0x01, 0x25,0x06,0x00,0x80,
        8,0,0,0, 0x22,0x03,0x60,0,0xf4,0x01,0,0
    };
    tx.echo_id = UINT32_C(0x01020304);
    tx.can_id = 0x625;
    tx.extended = true;
    tx.dlc = 8;
    memcpy(tx.data, golden + 12, 8);
    CHECK(gs_usb_encode_frame(&tx, p, 20));
    CHECK(memcmp(p, golden, 20) == 0);
    CHECK(gs_usb_decode_frame(p, 20, 0, false, &decoded));
    CHECK(decoded.echo_id == tx.echo_id && decoded.extended && !decoded.error);
    CHECK(decoded.can_id == 0x625 && decoded.dlc == 8 && decoded.channel == 0);
    CHECK(memcmp(decoded.data, tx.data, 8) == 0);
    gs_usb_put_le32(p, GS_USB_ECHO_RX);
    CHECK(gs_usb_decode_frame(p, 20, 0, false, &decoded));
    CHECK(decoded.echo_id == GS_USB_ECHO_RX);
    CHECK(!decoded.timestamp_valid);
    gs_usb_put_le32(p + 20, UINT32_C(0xfedcba98));
    CHECK(gs_usb_decode_frame(p, 24, 0, true, &decoded));
    CHECK(decoded.timestamp_valid && decoded.timestamp_us == UINT32_C(0xfedcba98));
    CHECK(!gs_usb_decode_frame(p, 24, 0, false, &decoded));
    CHECK(!gs_usb_decode_frame(p, 20, 0, true, &decoded));
    for (size_t size = 0; size < 20; ++size)
        CHECK(!gs_usb_decode_frame(p, size, 0, false, &decoded));
    CHECK(!gs_usb_decode_frame(p, 20, 1, false, &decoded));
    p[10] = GS_USB_FLAG_OVERFLOW;
    CHECK(gs_usb_decode_frame(p, 20, 0, false, &decoded) && decoded.overflow);
    for (unsigned flag = 2; flag < 256; flag <<= 1) {
        p[10] = (uint8_t)flag;
        CHECK(!gs_usb_decode_frame(p, 20, 0, false, &decoded));
    }
    p[10] = 0;
    p[11] = 1;
    CHECK(!gs_usb_decode_frame(p, 20, 0, false, &decoded));
    p[11] = 0;
    p[8] = 9;
    CHECK(!gs_usb_decode_frame(p, 20, 0, false, &decoded));
    p[8] = 8;
    gs_usb_put_le32(p + 4, GS_USB_CAN_ERR_FLAG | GS_USB_CAN_ERR_BUSOFF);
    CHECK(gs_usb_decode_frame(p, 20, 0, false, &decoded));
    CHECK(decoded.error && decoded.can_id == GS_USB_CAN_ERR_BUSOFF);
    gs_usb_put_le32(p + 4, GS_USB_CAN_ERR_FLAG | GS_USB_CAN_EFF_FLAG);
    CHECK(!gs_usb_decode_frame(p, 20, 0, false, &decoded));
    gs_usb_put_le32(p + 4, GS_USB_CAN_ERR_FLAG | GS_USB_CAN_RTR_FLAG);
    CHECK(!gs_usb_decode_frame(p, 20, 0, false, &decoded));
    gs_usb_put_le32(p + 4, GS_USB_CAN_ERR_FLAG);
    p[8] = 7;
    CHECK(!gs_usb_decode_frame(p, 20, 0, false, &decoded));
    p[8] = 8;
    gs_usb_put_le32(p, 0);
    CHECK(!gs_usb_decode_frame(p, 20, 0, false, &decoded));
    gs_usb_put_le32(p, GS_USB_ECHO_RX);
    gs_usb_put_le32(p + 4, 0x800);
    CHECK(!gs_usb_decode_frame(p, 20, 0, false, &decoded));
    gs_usb_put_le32(p + 4, GS_USB_CAN_RTR_FLAG | 0x7ff);
    CHECK(gs_usb_decode_frame(p, 20, 0, false, &decoded));
    CHECK(decoded.rtr && !decoded.extended);
    CHECK(memcmp(decoded.data, (const uint8_t[8]){0}, 8) == 0);
    tx.dlc = 3;
    tx.rtr = true;
    CHECK(gs_usb_encode_frame(&tx, p, 20));
    CHECK(memcmp(p + 12, (const uint8_t[8]){0}, 8) == 0);
    tx.rtr = false;
    CHECK(gs_usb_encode_frame(&tx, p, 20));
    CHECK(memcmp(p + 12, tx.data, 3) == 0);
    CHECK(memcmp(p + 15, (const uint8_t[5]){0}, 5) == 0);
    tx.can_id = UINT32_C(0x1fffffff);
    CHECK(gs_usb_encode_frame(&tx, p, 20));
    tx.can_id = UINT32_C(0x20000000);
    CHECK(!gs_usb_encode_frame(&tx, p, 20));
    tx.can_id = 0x800;
    tx.extended = false;
    CHECK(!gs_usb_encode_frame(&tx, p, 20));
    tx.can_id = 0x7ff;
    CHECK(gs_usb_encode_frame(&tx, p, 20));
    tx.echo_id = GS_USB_ECHO_RX;
    CHECK(!gs_usb_encode_frame(&tx, p, 20));
    tx.echo_id = 0;
    tx.overflow = true;
    CHECK(!gs_usb_encode_frame(&tx, p, 20));
    tx.overflow = false;
    tx.error = true;
    CHECK(!gs_usb_encode_frame(&tx, p, 20));
}

/* A deliberately simple exhaustive oracle verifies sample-point optimality,
 * separately from production's two-candidate-per-prescaler algorithm. */
static void check_timing(const gs_usb_bt_const_t *limits, uint32_t rate, unsigned target)
{
    gs_usb_bittiming_t t;
    bool found = gs_usb_find_bittiming(limits, rate, (uint16_t)target, &t);
    bool oracle_found = false;
    unsigned best_tq = 0, best_err = 0;
    for (uint32_t brp = limits->brp_min; brp <= limits->brp_max; brp += limits->brp_inc)
        for (uint32_t a = limits->tseg1_min; a <= limits->tseg1_max; ++a)
            for (uint32_t b = limits->tseg2_min; b <= limits->tseg2_max; ++b) {
                unsigned tq = 1 + a + b;
                if ((uint64_t)rate * brp * tq != limits->clock_hz) continue;
                unsigned actual = 1000 * (1 + a), goal = target * tq;
                unsigned error = actual > goal ? actual - goal : goal - actual;
                if (!oracle_found || (uint64_t)error * best_tq < (uint64_t)best_err * tq ||
                    ((uint64_t)error * best_tq == (uint64_t)best_err * tq && tq > best_tq)) {
                    best_tq = tq;
                    best_err = error;
                    oracle_found = true;
                }
            }
    CHECK(found == oracle_found);
    if (found) {
        uint32_t tq = 1 + t.prop_seg + t.phase_seg1 + t.phase_seg2;
        unsigned actual = 1000 * (1 + t.prop_seg + t.phase_seg1), goal = target * tq;
        unsigned error = actual > goal ? actual - goal : goal - actual;
        CHECK((uint64_t)rate * t.brp * tq == limits->clock_hz);
        CHECK((uint64_t)error * best_tq == (uint64_t)best_err * tq);
        CHECK(tq == best_tq);
        CHECK(t.sjw > 0 && t.sjw <= limits->sjw_max && t.sjw <= t.phase_seg2);
        CHECK(t.phase_seg2 >= limits->tseg2_min && t.phase_seg2 <= limits->tseg2_max);
        CHECK(t.prop_seg + t.phase_seg1 >= limits->tseg1_min);
        CHECK(t.prop_seg + t.phase_seg1 <= limits->tseg1_max);
        CHECK((t.brp - limits->brp_min) % limits->brp_inc == 0);
    }
}

static void test_timing(void)
{
    const uint32_t rates[] = {20000,50000,100000,125000,250000,500000,800000,1000000};
    const uint16_t sample_points[] = {800,800,800,813,813,813,800,813};
    gs_usb_bittiming_t result = {0};
    gs_usb_bt_const_t limits = bxcan;
    for (unsigned i = 0; i < sizeof(rates) / sizeof(rates[0]); ++i) {
        check_timing(&limits, rates[i], 800);
        CHECK(gs_usb_find_bittiming(&limits, rates[i], 800, &result));
        /* Exact baudrate is mandatory. Sample point is nearest realizable 80%:
         * several rates on 48 MHz bxCAN use 81.25%, rounded here to 813. */
        CHECK(result.sample_point_permille == sample_points[i]);
    }
    CHECK(gs_usb_find_bittiming(&limits, 800000, 800, &result));
    CHECK(result.brp == 3 && result.prop_seg + result.phase_seg1 == 15 &&
          result.phase_seg2 == 4); /* Highest-TQ exact 80% solution. */
    for (unsigned target = 550; target <= 950; target += 37)
        check_timing(&limits, 800000, target);
    limits.brp_min = 2;
    limits.brp_inc = 3;
    for (unsigned i = 0; i < sizeof(rates) / sizeof(rates[0]); ++i)
        check_timing(&limits, rates[i], 800);
    limits = bxcan;
    limits.clock_hz = 36000000;
    check_timing(&limits, 800000, 800);
    check_timing(&limits, 1000000, 875);
    CHECK(!gs_usb_find_bittiming(&limits, 123457, 800, &result));
    gs_usb_bittiming_t saved = result;
    CHECK(!gs_usb_find_bittiming(&limits, 0, 800, &result));
    CHECK(memcmp(&saved, &result, sizeof(result)) == 0);
    CHECK(!gs_usb_find_bittiming(&limits, 500000, 0, &result));
    CHECK(!gs_usb_find_bittiming(&limits, 500000, 1000, &result));
    limits.brp_inc = 0;
    CHECK(!gs_usb_find_bittiming(&limits, 500000, 800, &result));
    limits = bxcan;
    limits.brp_max = UINT32_MAX;
    CHECK(!gs_usb_find_bittiming(&limits, 500000, 800, &result));
    limits = bxcan;
    limits.tseg1_max = UINT32_MAX;
    CHECK(!gs_usb_find_bittiming(&limits, 500000, 800, &result));
    CHECK(!gs_usb_find_bittiming(NULL, 500000, 800, &result));
}

int main(void)
{
    test_endian_and_controls();
    test_descriptors();
    test_frames();
    test_timing();
    printf("gs_usb_protocol: %u checks passed\n", checks);
    return EXIT_SUCCESS;
}
