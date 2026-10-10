/* Host-only protocol tests: no ESP-IDF, serial port, CAN adapter or servo. */
#include "kst_protocol.h"

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

static void golden(const kst_can_frame_t *frame, uint32_t id, uint8_t len,
                   const uint8_t expected[8])
{
    CHECK(frame->id == id);
    CHECK(frame->len == len);
    CHECK(memcmp(frame->data, expected, 8) == 0);
}

static void test_golden_commands(void)
{
    kst_can_frame_t f;
    CHECK(kst_make_reporting(0x25, true, &f));
    golden(&f, 0x25, 2, (const uint8_t[8]){0x01, 0, 0, 0, 0, 0, 0, 0});
    CHECK(kst_make_reporting(0x25, false, &f));
    golden(&f, 0x25, 2, (const uint8_t[8]){0x02, 0, 0, 0, 0, 0, 0, 0});
    CHECK(kst_make_report_interval(0x25, 100, &f));
    golden(&f, 0x625, 8, (const uint8_t[8]){0x22, 0x00, 0x22, 0, 0x64, 0, 0, 0});
    CHECK(kst_make_baudrate(0x25, KST_BAUD_500K, &f));
    golden(&f, 0x625, 8, (const uint8_t[8]){0x22, 0x01, 0x30, 0, 0x05, 0, 0, 0});
    CHECK(kst_make_node_id(0x25, 0x26, &f));
    golden(&f, 0x625, 8, (const uint8_t[8]){0x22, 0x00, 0x30, 0, 0x26, 0, 0, 0});

    /* Signed-angle examples: exercise byte order and two's complement.
     * The document's +50 degree low byte says FC, a numerical typo: 500 = 01F4.
     * Follow its stated 0.1-degree unit, not that one inconsistent table cell. */
    static const struct { int16_t angle; uint8_t lo; uint8_t hi; } angles[] = {
        {-1000, 0x18, 0xFC}, {-500, 0x0C, 0xFE}, {-109, 0x93, 0xFF},
        {-8, 0xF8, 0xFF}, {0, 0, 0}, {8, 8, 0}, {109, 0x6D, 0},
        {500, 0xF4, 0x01}, {1000, 0xE8, 0x03}
    };
    for (size_t i = 0; i < sizeof(angles) / sizeof(angles[0]); ++i) {
        CHECK(kst_make_position(0x25, angles[i].angle, &f));
        golden(&f, 0x625, 8, (const uint8_t[8]){
            0x22, 0x03, 0x60, 0, angles[i].lo, angles[i].hi, 0, 0});
    }
    CHECK(kst_make_center(0x25, &f));
    golden(&f, 0x625, 8, (const uint8_t[8]){0x22, 0x09, 0x30, 0, 0, 0, 0, 0});
    CHECK(kst_make_save(0x25, &f));
    golden(&f, 0x625, 8, (const uint8_t[8]){0x22, 0x10, 0x10, 0x01, 0x73, 0x61, 0x76, 0x65});
    CHECK(kst_make_read_node(0x25, &f));
    golden(&f, 0x625, 8, (const uint8_t[8]){0x40, 0x00, 0x30, 0, 0, 0, 0, 0});
    CHECK(kst_make_read_position(0x25, &f));
    golden(&f, 0x625, 8, (const uint8_t[8]){0x40, 0x02, 0x60, 0, 0, 0, 0, 0});
    CHECK(kst_make_read_status(0x25, &f));
    golden(&f, 0x625, 8, (const uint8_t[8]){0x40, 0x05, 0x60, 0, 0, 0, 0, 0});
    CHECK(kst_make_read_features(0x25, &f));
    golden(&f, 0x625, 8, (const uint8_t[8]){0x40, 0x0F, 0x30, 0, 0, 0, 0, 0});
    CHECK(kst_make_features(0x25, 0x9F, KST_FEATURE_UNDERVOLTAGE,
                            KST_FEATURE_UNDERVOLTAGE, &f));
    golden(&f, 0x625, 8, (const uint8_t[8]){0x22, 0x0F, 0x30, 0, 0xBF, 0, 0, 0});
    CHECK(kst_make_features(0x25, 0xFF, KST_FEATURE_REVERSE, 0, &f));
    golden(&f, 0x625, 8, (const uint8_t[8]){0x22, 0x0F, 0x30, 0, 0xBF, 0, 0, 0});
    CHECK(kst_make_limit(0x25, false, -1000, &f));
    golden(&f, 0x625, 8, (const uint8_t[8]){0x22, 0x0A, 0x30, 0, 0x18, 0xFC, 0, 0});
    CHECK(kst_make_limit(0x25, true, 1000, &f));
    golden(&f, 0x625, 8, (const uint8_t[8]){0x22, 0x0B, 0x30, 0, 0xE8, 0x03, 0, 0});
    CHECK(kst_make_read_limit(0x25, false, &f));
    golden(&f, 0x625, 8, (const uint8_t[8]){0x40, 0x0A, 0x30, 0, 0, 0, 0, 0});
    CHECK(kst_make_read_limit(0x25, true, &f));
    golden(&f, 0x625, 8, (const uint8_t[8]){0x40, 0x0B, 0x30, 0, 0, 0, 0, 0});
    CHECK(kst_make_read_version(0x25, &f));
    golden(&f, 0x625, 8, (const uint8_t[8]){0x40, 0x0B, 0x10, 0, 0, 0, 0, 0});
}

static void test_boundaries_and_no_broadcast(void)
{
    kst_can_frame_t f;
    memset(&f, 0xA5, sizeof(f));
    kst_can_frame_t unchanged;
    memcpy(&unchanged, &f, sizeof(f));
    CHECK(!kst_make_position(0, 0, &f));
    CHECK(!kst_make_position(128, 0, &f));
    CHECK(!kst_make_position(0x25, -1001, &f));
    CHECK(!kst_make_position(0x25, 1001, &f));
    CHECK(!kst_make_position(0x25, 0, NULL));
    CHECK(!kst_make_report_interval(0x25, 9, &f));
    CHECK(!kst_make_report_interval(0x25, 256, &f));
    CHECK(!kst_make_baudrate(0x25, (kst_baudrate_t)-1, &f));
    CHECK(!kst_make_baudrate(0x25, (kst_baudrate_t)8, &f));
    CHECK(!kst_make_node_id(0x25, 0, &f));
    CHECK(!kst_make_node_id(0x25, 128, &f));
    CHECK(!kst_make_reporting(0, true, &f));
    CHECK(!kst_make_center(0, &f));
    CHECK(!kst_make_save(0, &f));
    CHECK(!kst_make_read_node(0, &f));
    CHECK(!kst_make_read_position(0, &f));
    CHECK(!kst_make_read_status(0, &f));
    CHECK(!kst_make_read_features(0, &f));
    CHECK(!kst_make_read_version(0, &f));
    CHECK(!kst_make_read_limit(0, true, &f));
    CHECK(!kst_make_features(0x25, 0xA5, 0x01, 0, &f));
    CHECK(!kst_make_features(0x25, 0xA5, 0, 0, &f));
    CHECK(!kst_make_features(0x25, 0xA5, 0x20, 0x40, &f));
    CHECK(!kst_make_features(5, 0x5C90, KST_FEATURE_REVERSE, KST_FEATURE_REVERSE, &f));
    CHECK(!kst_make_features(5, 0x0190, KST_FEATURE_UNDERVOLTAGE, 0, &f));
    CHECK(!kst_make_features(5, 0xFF90, KST_FEATURE_WRITABLE_MASK, 0, &f));
    CHECK(!kst_make_limit(0x25, false, 1, &f));
    CHECK(!kst_make_limit(0x25, false, -1001, &f));
    CHECK(!kst_make_limit(0x25, true, -1, &f));
    CHECK(!kst_make_limit(0x25, true, 1001, &f));
    CHECK(memcmp(&f, &unchanged, sizeof(f)) == 0);

    CHECK(kst_make_position(1, -1000, &f));
    CHECK(kst_make_position(127, 1000, &f));
    CHECK(kst_make_report_interval(0x25, 10, &f));
    CHECK(kst_make_report_interval(0x25, 255, &f));
    CHECK(kst_make_limit(0x25, false, 0, &f));
    CHECK(kst_make_limit(0x25, true, 0, &f));
    static const uint32_t rates[] = {20000, 50000, 100000, 125000, 250000, 500000, 800000, 1000000};
    for (unsigned i = 0; i < sizeof(rates) / sizeof(rates[0]); ++i) {
        CHECK(kst_baudrate_bps((kst_baudrate_t)i) == rates[i]);
        CHECK(kst_make_baudrate(0x25, (kst_baudrate_t)i, &f));
        CHECK(f.data[4] == i);
    }
    CHECK(kst_baudrate_bps((kst_baudrate_t)99) == 0);
}

static void test_responses(void)
{
    kst_event_t e;
    kst_can_frame_t f = {.id = 0x5A5, .len = 8,
                        .data = {0x4B, 0x02, 0x60, 0, 0x93, 0xFF, 0, 0}};
    CHECK(kst_parse(0x25, &f, true, false, false, &e) == KST_PARSE_OK);
    CHECK(e.type == KST_EVENT_POSITION && e.position_deci_deg == -109);
    CHECK(e.node == 0x25 && e.index == KST_INDEX_POSITION && e.subindex == 0);
    f = (kst_can_frame_t){.id = 0x5A5, .len = 4, .data = {0x18, 0xFC, 0, 0}};
    CHECK(kst_parse(0x25, &f, true, false, false, &e) == KST_PARSE_OK);
    CHECK(e.type == KST_EVENT_POSITION_REPORT && e.position_deci_deg == -1000);
    f = (kst_can_frame_t){.id = 0x5A5, .len = 8,
                         .data = {0x43, 0x05, 0x60, 0, 0xF4, 0x01, 0xD8, 0}};
    CHECK(kst_parse(0x25, &f, true, false, false, &e) == KST_PARSE_OK);
    CHECK(e.type == KST_EVENT_STATUS && e.current_10ma == 500 && e.temperature_c == -40);
    const uint8_t temperatures[] = {0xD8, 0xF6, 0x00, 0x19, 0x78};
    const int expected[] = {-40, -10, 0, 25, 120};
    for (size_t i = 0; i < sizeof(temperatures); ++i) {
        f.data[6] = temperatures[i];
        CHECK(kst_parse(0x25, &f, true, false, false, &e) == KST_PARSE_OK);
        CHECK(e.temperature_c == expected[i]);
    }
    f = (kst_can_frame_t){.id = 0x3A5, .len = 4, .data = {0x0A, 0x02, 0, 0}};
    CHECK(kst_parse(0x25, &f, true, false, false, &e) == KST_PARSE_OK);
    CHECK(e.type == KST_EVENT_UNDERVOLTAGE);
    f = (kst_can_frame_t){.id = 0x5A5, .len = 8, .data = {0x4F, 0x00, 0x30, 0, 0x25, 0, 0, 0}};
    CHECK(kst_parse(0x25, &f, true, false, false, &e) == KST_PARSE_OK);
    CHECK(e.type == KST_EVENT_NODE_ID && e.node_id == 0x25);
    f = (kst_can_frame_t){.id = 0x5A5, .len = 8, .data = {0x4B, 0x0F, 0x30, 0, 0xBF, 0, 0, 0}};
    CHECK(kst_parse(0x25, &f, true, false, false, &e) == KST_PARSE_OK);
    CHECK(e.type == KST_EVENT_FEATURES && e.feature_bits == 0xBF && e.feature_raw == 0x00BF);
    f = (kst_can_frame_t){.id = 0x5A5, .len = 8, .data = {0x4B, 0x0A, 0x30, 0, 0x18, 0xFC, 0, 0}};
    CHECK(kst_parse(0x25, &f, true, false, false, &e) == KST_PARSE_OK);
    CHECK(e.type == KST_EVENT_LIMIT_NEGATIVE && e.position_deci_deg == -1000);
    f = (kst_can_frame_t){.id = 0x5A5, .len = 8, .data = {0x4B, 0x0B, 0x30, 0, 0xE8, 0x03, 0, 0}};
    CHECK(kst_parse(0x25, &f, true, false, false, &e) == KST_PARSE_OK);
    CHECK(e.type == KST_EVENT_LIMIT_POSITIVE && e.position_deci_deg == 1000);
    f = (kst_can_frame_t){.id = 0x5A5, .len = 8, .data = {0x43, 0x0B, 0x10, 0, '3', '.', '6', '1'}};
    CHECK(kst_parse(0x25, &f, true, false, false, &e) == KST_PARSE_OK);
    CHECK(e.type == KST_EVENT_VERSION && strcmp(e.version, "3.61") == 0);

    const uint16_t ack_indexes[] = {0x2200, 0x3001, 0x3000, 0x3009, 0x300F, 0x300A, 0x300B};
    for (size_t i = 0; i < sizeof(ack_indexes) / sizeof(ack_indexes[0]); ++i) {
        f = (kst_can_frame_t){.id = 0x5A5, .len = 8,
                             .data = {0x60, (uint8_t)ack_indexes[i], (uint8_t)(ack_indexes[i] >> 8), 0, 0, 0, 0, 0}};
        CHECK(kst_parse(0x25, &f, true, false, false, &e) == KST_PARSE_OK);
        CHECK(e.type == KST_EVENT_WRITE_ACK && e.index == ack_indexes[i]);
    }
    f = (kst_can_frame_t){.id = 0x5A5, .len = 8, .data = {0x80, 0x03, 0x60, 0, 0x30, 0, 0x09, 0x06}};
    CHECK(kst_parse(0x25, &f, true, false, false, &e) == KST_PARSE_OK);
    CHECK(e.type == KST_EVENT_ABORT && e.index == KST_INDEX_POSITION_TARGET);
    CHECK(e.abort_code == UINT32_C(0x06090030));
    CHECK(strcmp(kst_abort_description(e.abort_code), "Parameter value outside allowed range") == 0);
    CHECK(strcmp(kst_abort_description(0xDEADBEEF), "Unknown KST abort code") == 0);
    f.data[1] = 0x10; f.data[2] = 0x10; f.data[3] = 1;
    CHECK(kst_parse(0x25, &f, true, false, false, &e) == KST_PARSE_OK);
    CHECK(e.index == KST_INDEX_SAVE && e.subindex == 1);
}

static void invalid_node(uint8_t node, const kst_can_frame_t *f)
{
    kst_event_t e;
    memset(&e, 0xA5, sizeof(e));
    kst_event_t original;
    memcpy(&original, &e, sizeof(e));
    CHECK(kst_parse(node, f, true, false, false, &e) == KST_PARSE_INVALID);
    CHECK(memcmp(&e, &original, sizeof(e)) == 0);
}

static void invalid(const kst_can_frame_t *f)
{ invalid_node(0x25, f); }

static void test_abort_endianness_compatibility(void)
{
    /* Exact 3.66 rejection of the +180 degree limit probe, node 5. */
    kst_can_frame_t f = {.id = 0x585, .len = 8,
                         .data = {0x80, 0x0B, 0x30, 0, 0x06, 0x09, 0, 0x30}};
    kst_event_t e;
    CHECK(kst_parse(5, &f, true, false, false, &e) == KST_PARSE_OK);
    CHECK(e.type == KST_EVENT_ABORT && e.index == KST_INDEX_LIMIT_POSITIVE && e.subindex == 0);
    CHECK(e.abort_code == UINT32_C(0x06090030));
    CHECK(e.abort_wire_le == UINT32_C(0x30000906) && e.abort_byte_swapped);
    CHECK(strcmp(kst_abort_description(e.abort_code), "Parameter value outside allowed range") == 0);

    const uint32_t known[] = {UINT32_C(0x06090030), UINT32_C(0x05040000)};
    for (size_t i = 0; i < sizeof(known) / sizeof(known[0]); ++i) {
        for (unsigned big_endian = 0; big_endian <= 1; ++big_endian) {
            for (unsigned byte = 0; byte < 4; ++byte) {
                f.data[4 + byte] = (uint8_t)(known[i] >> (8 * (big_endian ? 3 - byte : byte)));
            }
            CHECK(kst_parse(5, &f, true, false, false, &e) == KST_PARSE_OK);
            CHECK(e.abort_code == known[i]);
            CHECK(e.abort_byte_swapped == (big_endian != 0));
            for (unsigned byte = 0; byte < 4; ++byte) {
                CHECK((uint8_t)(e.abort_wire_le >> (8 * byte)) == f.data[4 + byte]);
            }
        }
    }

    /* Neither arbitrary bytes nor a known-looking prefix justify normalization. */
    const uint32_t unknown[] = {UINT32_C(0xDEADBEEF), UINT32_C(0x06090099), 0};
    for (size_t i = 0; i < sizeof(unknown) / sizeof(unknown[0]); ++i) {
        for (unsigned big_endian = 0; big_endian <= 1; ++big_endian) {
            uint32_t wire_le = 0;
            for (unsigned byte = 0; byte < 4; ++byte) {
                f.data[4 + byte] = (uint8_t)(unknown[i] >> (8 * (big_endian ? 3 - byte : byte)));
                wire_le |= (uint32_t)f.data[4 + byte] << (8 * byte);
            }
            CHECK(kst_parse(5, &f, true, false, false, &e) == KST_PARSE_OK);
            CHECK(e.abort_wire_le == wire_le && e.abort_code == wire_le);
            CHECK(!e.abort_byte_swapped);
            CHECK(strcmp(kst_abort_description(e.abort_code), "Unknown KST abort code") == 0);
        }
    }
}

static void test_single_turn_feedback_separate_from_writes(void)
{
    /* Actual 3.66 node-5 READ POSITION outside the configured -100 degree limit. */
    kst_can_frame_t f = {.id = 0x585, .len = 8,
                         .data = {0x4B, 0x02, 0x60, 0, 0x13, 0xFC, 0, 0}};
    kst_event_t e;
    CHECK(kst_parse(5, &f, true, false, false, &e) == KST_PARSE_OK);
    CHECK(e.type == KST_EVENT_POSITION && e.node == 5 && e.position_deci_deg == -1005);
    CHECK(KST_POSITION_MIN == -1000 && KST_POSITION_MAX == 1000);
    CHECK(KST_POSITION_FEEDBACK_MIN == -1800 && KST_POSITION_FEEDBACK_MAX == 1800);

    const int16_t accepted[] = {-1800, -1005, -1001, -1, 0, 1, 1001, 1005, 1800};
    for (size_t i = 0; i < sizeof(accepted) / sizeof(accepted[0]); ++i) {
        const uint16_t wire = (uint16_t)accepted[i];
        f = (kst_can_frame_t){.id = 0x585, .len = 8,
            .data = {0x4B, 0x02, 0x60, 0, (uint8_t)wire, (uint8_t)(wire >> 8), 0, 0}};
        CHECK(kst_parse(5, &f, true, false, false, &e) == KST_PARSE_OK);
        CHECK(e.type == KST_EVENT_POSITION && e.position_deci_deg == accepted[i]);
        /* The existing consumer's reversed-sign sample must fit int16_t. */
        const int32_t reversed = -(int32_t)e.position_deci_deg;
        CHECK(reversed >= INT16_MIN && reversed <= INT16_MAX);
        CHECK((int16_t)reversed == -accepted[i]);

        f = (kst_can_frame_t){.id = 0x585, .len = 4,
            .data = {(uint8_t)wire, (uint8_t)(wire >> 8), 0, 0}};
        CHECK(kst_parse(5, &f, true, false, false, &e) == KST_PARSE_OK);
        CHECK(e.type == KST_EVENT_POSITION_REPORT && e.position_deci_deg == accepted[i]);
    }

    const int16_t rejected[] = {-1801, 1801, INT16_MIN, INT16_MAX};
    for (size_t i = 0; i < sizeof(rejected) / sizeof(rejected[0]); ++i) {
        const uint16_t wire = (uint16_t)rejected[i];
        f = (kst_can_frame_t){.id = 0x585, .len = 8,
            .data = {0x4B, 0x02, 0x60, 0, (uint8_t)wire, (uint8_t)(wire >> 8), 0, 0}};
        invalid_node(5, &f);
        f = (kst_can_frame_t){.id = 0x585, .len = 4,
            .data = {(uint8_t)wire, (uint8_t)(wire >> 8), 0, 0}};
        invalid_node(5, &f);
    }

    /* Limits remain documented range even though POSITION accepts these values. */
    const int16_t outside_limits[] = {-1800, -1005, -1001, 1001, 1005, 1800};
    for (size_t i = 0; i < sizeof(outside_limits) / sizeof(outside_limits[0]); ++i) {
        const uint16_t wire = (uint16_t)outside_limits[i];
        f = (kst_can_frame_t){.id = 0x585, .len = 8,
            .data = {0x4B, outside_limits[i] < 0 ? 0x0A : 0x0B, 0x30, 0,
                     (uint8_t)wire, (uint8_t)(wire >> 8), 0, 0}};
        invalid_node(5, &f);
        if (outside_limits[i] < 0) {
            f.data[6] = f.data[7] = 0xFF;
            invalid_node(5, &f);
        }
        memset(&f, 0xA5, sizeof(f));
        kst_can_frame_t unchanged;
        memcpy(&unchanged, &f, sizeof(f));
        CHECK(!kst_make_position(5, outside_limits[i], &f));
        CHECK(!kst_make_limit(5, outside_limits[i] > 0, outside_limits[i], &f));
        CHECK(memcmp(&f, &unchanged, sizeof(f)) == 0);
    }

    /* Target and limit endpoint payloads must remain unchanged. */
    CHECK(kst_make_position(5, -1000, &f));
    golden(&f, 0x605, 8, (const uint8_t[8]){0x22, 0x03, 0x60, 0, 0x18, 0xFC, 0, 0});
    CHECK(kst_make_position(5, 1000, &f));
    golden(&f, 0x605, 8, (const uint8_t[8]){0x22, 0x03, 0x60, 0, 0xE8, 0x03, 0, 0});
    CHECK(kst_make_limit(5, false, -1000, &f));
    golden(&f, 0x605, 8, (const uint8_t[8]){0x22, 0x0A, 0x30, 0, 0x18, 0xFC, 0, 0});
    CHECK(kst_make_limit(5, true, 1000, &f));
    golden(&f, 0x605, 8, (const uint8_t[8]){0x22, 0x0B, 0x30, 0, 0xE8, 0x03, 0, 0});
}

static void test_firmware_366_negative_limit_sign_extension(void)
{
    /* Exact observed node-5 frame: EXT 0x585 DLC 8 4B0A300018FCFFFF. */
    kst_can_frame_t f = {.id = 0x585, .len = 8,
                         .data = {0x4B, 0x0A, 0x30, 0, 0x18, 0xFC, 0xFF, 0xFF}};
    kst_event_t e;
    CHECK(kst_parse(5, &f, true, false, false, &e) == KST_PARSE_OK);
    CHECK(e.type == KST_EVENT_LIMIT_NEGATIVE && e.position_deci_deg == -1000);
    CHECK(e.node == 5 && e.index == KST_INDEX_LIMIT_NEGATIVE && e.subindex == 0);

    const int16_t angles[] = {-1, -1000};
    for (size_t i = 0; i < sizeof(angles) / sizeof(angles[0]); ++i) {
        f.data[4] = (uint8_t)angles[i];
        f.data[5] = (uint8_t)((uint16_t)angles[i] >> 8);
        for (unsigned sign_extended = 0; sign_extended <= 1; ++sign_extended) {
            f.data[6] = f.data[7] = sign_extended ? 0xFF : 0;
            CHECK(kst_parse(5, &f, true, false, false, &e) == KST_PARSE_OK);
            CHECK(e.type == KST_EVENT_LIMIT_NEGATIVE && e.position_deci_deg == angles[i]);
        }
        f.data[6] = 0xFF; f.data[7] = 0;
        invalid_node(5, &f);
        f.data[6] = 0; f.data[7] = 0xFF;
        invalid_node(5, &f);
    }

    const int16_t rejected[] = {0, 1, 1000, 1001, -1001, INT16_MIN};
    for (size_t i = 0; i < sizeof(rejected) / sizeof(rejected[0]); ++i) {
        f.data[4] = (uint8_t)rejected[i];
        f.data[5] = (uint8_t)((uint16_t)rejected[i] >> 8);
        f.data[6] = f.data[7] = 0xFF;
        invalid_node(5, &f);
    }

    /* Zero remains valid only with the original zero tail. */
    f.data[4] = f.data[5] = f.data[6] = f.data[7] = 0;
    CHECK(kst_parse(5, &f, true, false, false, &e) == KST_PARSE_OK);
    CHECK(e.type == KST_EVENT_LIMIT_NEGATIVE && e.position_deci_deg == 0);

    /* Other object types do not inherit the compatibility exception. */
    f.data[1] = 0x02; f.data[2] = 0x60;
    f.data[4] = 0xFF; f.data[5] = 0xFF; f.data[6] = f.data[7] = 0xFF;
    invalid_node(5, &f);
    f.data[1] = 0x0B; f.data[2] = 0x30;
    f.data[4] = 1; f.data[5] = 0;
    invalid_node(5, &f);

    /* Receive compatibility must never change the documented write payload. */
    CHECK(kst_make_limit(5, false, -1000, &f));
    golden(&f, 0x605, 8, (const uint8_t[8]){0x22, 0x0A, 0x30, 0, 0x18, 0xFC, 0, 0});
}

static void test_firmware_366_features_read_only(void)
{
    /* Exact observed node-5 response: EXT 0x585 DLC 8 4B0F3000905C0000.
     * A valid read must retain both bytes without assigning meaning to 0x5C. */
    kst_can_frame_t f = {.id = 0x585, .len = 8,
                         .data = {0x4B, 0x0F, 0x30, 0, 0x90, 0x5C, 0, 0}};
    kst_event_t e;
    CHECK(kst_parse(5, &f, true, false, false, &e) == KST_PARSE_OK);
    CHECK(e.node == 5 && e.index == KST_INDEX_FEATURES && e.type == KST_EVENT_FEATURES);
    CHECK(e.feature_bits == 0x90 && e.feature_raw == 0x5C90);
    CHECK((e.feature_bits & KST_FEATURE_WRITABLE_MASK) == 0);

    const uint8_t high_bytes[] = {0x00, 0x01, 0x5C, 0xFF};
    for (size_t i = 0; i < sizeof(high_bytes); ++i) {
        f.data[5] = high_bytes[i];
        CHECK(kst_parse(5, &f, true, false, false, &e) == KST_PARSE_OK);
        CHECK(e.feature_raw == (uint16_t)(0x90U | ((uint16_t)high_bytes[i] << 8)));
        CHECK(e.feature_bits == 0x90);
    }

    /* Broaden only the 16-bit read payload; upper reserved bytes remain strict. */
    f.id = 0x5A5;
    f.data[5] = 0x5C;
    f.data[6] = 1;
    invalid(&f);
    f.data[6] = 0; f.data[7] = 1;
    invalid(&f);
    f.data[7] = 0; f.data[0] = 0x43;
    invalid(&f);
}

static void test_malformed_and_unrelated_frames(void)
{
    kst_event_t e;
    const kst_can_frame_t good = {.id = 0x5A5, .len = 8,
                                 .data = {0x4B, 0x02, 0x60, 0, 0, 0, 0, 0}};
    kst_can_frame_t f = good;
    CHECK(kst_parse(0x25, &f, false, false, false, &e) == KST_PARSE_INVALID);
    CHECK(kst_parse(0x25, &f, true, true, false, &e) == KST_PARSE_INVALID);
    CHECK(kst_parse(0x25, &f, true, false, true, &e) == KST_PARSE_INVALID);
    CHECK(kst_parse(0, &f, true, false, false, &e) == KST_PARSE_INVALID);
    CHECK(kst_parse(0x25, NULL, true, false, false, &e) == KST_PARSE_INVALID);
    CHECK(kst_parse(0x25, &f, true, false, false, NULL) == KST_PARSE_INVALID);
    f.id = 0x5A6;
    CHECK(kst_parse(0x25, &f, true, false, false, &e) == KST_PARSE_IGNORED);
    f.id = 0x625; /* Our own TX frames are never interpreted as servo replies. */
    CHECK(kst_parse(0x25, &f, true, false, false, &e) == KST_PARSE_IGNORED);
    f.id = 0x200005A5;
    CHECK(kst_parse(0x25, &f, true, false, false, &e) == KST_PARSE_IGNORED);
    for (unsigned len = 0; len <= 15; ++len) {
        if (len == 8) continue;
        f = good; f.len = (uint8_t)len;
        invalid(&f); /* len 4 has nonzero reserved bytes, not a valid report. */
    }
    f = good; f.data[0] = 0x43; invalid(&f);
    f = good; f.data[1] = 0x03; invalid(&f);
    f = good; f.data[2] = 0x61; invalid(&f);
    f = good; f.data[3] = 1; invalid(&f);
    f = good; f.data[6] = 1; invalid(&f);
    f = good; f.data[7] = 1; invalid(&f);
    f = good; f.data[4] = 0x09; f.data[5] = 0x07; invalid(&f); /* +180.1 degrees */
    f = (kst_can_frame_t){.id = 0x5A5, .len = 4, .data = {0, 0, 1, 0}}; invalid(&f);
    f = (kst_can_frame_t){.id = 0x5A5, .len = 4, .data = {0xF7, 0xF8, 0, 0}}; invalid(&f); /* -180.1 */
    f = (kst_can_frame_t){.id = 0x3A5, .len = 4, .data = {0x0B, 0x02, 0, 0}}; invalid(&f);
    f = (kst_can_frame_t){.id = 0x3A5, .len = 8, .data = {0x0A, 0x02, 0, 0}}; invalid(&f);
    f = (kst_can_frame_t){.id = 0x5A5, .len = 8, .data = {0x60, 0x03, 0x60, 0, 0, 0, 0, 0}}; invalid(&f);
    f = (kst_can_frame_t){.id = 0x5A5, .len = 8, .data = {0x60, 0x10, 0x10, 1, 0, 0, 0, 0}}; invalid(&f);
    f = (kst_can_frame_t){.id = 0x5A5, .len = 8, .data = {0x60, 0x00, 0x22, 0, 1, 0, 0, 0}}; invalid(&f);
    f = (kst_can_frame_t){.id = 0x5A5, .len = 8, .data = {0x80, 0xFE, 0xFF, 0, 0, 0, 0, 0}}; invalid(&f);
    f = (kst_can_frame_t){.id = 0x5A5, .len = 8, .data = {0x43, 0x05, 0x60, 0, 0, 0, 0xD7, 0}}; invalid(&f);
    f.data[6] = 121; invalid(&f);
    f = (kst_can_frame_t){.id = 0x5A5, .len = 8, .data = {0x4F, 0x00, 0x30, 0, 0, 0, 0, 0}}; invalid(&f);
    f.data[4] = 128; invalid(&f);
    f = (kst_can_frame_t){.id = 0x5A5, .len = 8, .data = {0x4B, 0x0F, 0x30, 0, 0, 0, 1, 0}}; invalid(&f);
    f = (kst_can_frame_t){.id = 0x5A5, .len = 8, .data = {0x4B, 0x0A, 0x30, 0, 1, 0, 0, 0}}; invalid(&f);
    f = (kst_can_frame_t){.id = 0x5A5, .len = 8, .data = {0x4B, 0x0B, 0x30, 0, 0xFF, 0xFF, 0, 0}}; invalid(&f);
}

static void test_every_angle_round_trip(void)
{
    /* Independent decoder format plus all representable commanded angles.
     * Catches sign-extension, byte-order and endpoints beyond sample cases. */
    for (int angle = -1000; angle <= 1000; ++angle) {
        kst_can_frame_t f;
        kst_event_t e;
        CHECK(kst_make_position(0x25, (int16_t)angle, &f));
        f.id = 0x5A5;
        f.data[0] = 0x4B;
        f.data[1] = 0x02;
        CHECK(kst_parse(0x25, &f, true, false, false, &e) == KST_PARSE_OK);
        CHECK(e.position_deci_deg == angle);
    }
}

int main(void)
{
    test_golden_commands();
    test_boundaries_and_no_broadcast();
    test_responses();
    test_abort_endianness_compatibility();
    test_single_turn_feedback_separate_from_writes();
    test_firmware_366_features_read_only();
    test_firmware_366_negative_limit_sign_extension();
    test_malformed_and_unrelated_frames();
    test_every_angle_round_trip();
    printf("PASS: %u KST protocol checks\n", checks);
    return EXIT_SUCCESS;
}
