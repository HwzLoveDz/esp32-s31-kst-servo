/* SPDX-License-Identifier: MIT
 * Deterministic fixture, screenshot and action-wiring validation for fui.c.
 * No CAN interface, physical servo or ESP32 is accessed by this program.
 */
#include "fui.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static app_snapshot_t fixture;
static uint32_t actions_seen;
static app_action_t last_action;
static int32_t last_a, last_b;
static unsigned submitted;
static uint8_t framebuffer[800 * 480 * 4];

bool app_submit(app_action_t action, int32_t a, int32_t b)
{
    assert((unsigned)action <= APP_CLEAR_ALERTS);
    actions_seen |= 1UL << (unsigned)action;
    last_action = action; last_a = a; last_b = b; submitted++;
    printf("action=%u value=%ld value2=%ld\n", (unsigned)action, (long)a, (long)b);
    return true;
}
void app_get_snapshot(app_snapshot_t *out) { *out = fixture; }
int app_controller_start(void) { return 0; }

static void flush(lv_display_t *display, const lv_area_t *area, uint8_t *pixels)
{
    (void)area; (void)pixels;
    lv_display_flush_ready(display);
}

static lv_obj_t *find_label(lv_obj_t *obj, const char *text)
{
    if (lv_obj_check_type(obj, &lv_label_class) && strcmp(lv_label_get_text(obj), text) == 0) return obj;
    for (unsigned i = 0; i < lv_obj_get_child_count(obj); ++i) {
        lv_obj_t *found = find_label(lv_obj_get_child(obj, (int32_t)i), text);
        if (found) return found;
    }
    return NULL;
}

static void click(lv_obj_t *screen, const char *text)
{
    lv_obj_t *found = find_label(screen, text);
    if (!found) { fprintf(stderr, "Missing button: %s\n", text); abort(); }
    lv_obj_t *button = lv_obj_get_parent(found);
    assert(lv_obj_check_type(button, &lv_button_class));
    lv_obj_send_event(button, LV_EVENT_CLICKED, NULL);
}

static void screenshot(lv_display_t *display, const char *folder, const char *name)
{
    lv_obj_update_layout(lv_screen_active());
    lv_refr_now(display);
    char path[1024];
    snprintf(path, sizeof(path), "%s/%s.ppm", folder, name);
    FILE *f = fopen(path, "wb");
    if (!f) { perror(path); exit(1); }
    fprintf(f, "P6\n800 480\n255\n");
    for (size_t i = 0; i < 800U * 480U; ++i) {
        fputc(framebuffer[i * 4 + 2], f);
        fputc(framebuffer[i * 4 + 1], f);
        fputc(framebuffer[i * 4 + 0], f);
    }
    fclose(f);
    printf("Rendered %s\n", path);
}

static void expect_click(lv_obj_t *screen, const char *text, app_action_t action, int32_t a, int32_t b)
{
    unsigned count = submitted;
    click(screen, text);
    assert(submitted == count + 1);
    assert(last_action == action && last_a == a && last_b == b);
}

static void check_actions(lv_obj_t *screen)
{
    expect_click(screen, "ALL READBACK", APP_REFRESH, 0, 0);
    expect_click(screen, "READ POSITION", APP_READ_POSITION, 0, 0);
    expect_click(screen, "READ STATUS", APP_READ_STATUS, 0, 0);
    expect_click(screen, "READ FLAGS", APP_READ_FEATURES, 0, 0);
    expect_click(screen, "READ LIMITS", APP_READ_LIMITS, 0, 0);
    expect_click(screen, "READ NODE", APP_READ_NODE, 0, 0);
    expect_click(screen, "READ VERSION", APP_READ_VERSION, 0, 0);
    expect_click(screen, "START REPORT", APP_REPORT_START, 0, 0);
    expect_click(screen, "STOP REPORT", APP_REPORT_STOP, 0, 0);
    expect_click(screen, "SET INTERVAL", APP_REPORT_INTERVAL, 50, 0);
    fixture.armed = false; fui_refresh(&fixture);
    expect_click(screen, "ARM MOTION", APP_ARM, 1, 0);
    fixture.armed = true; fui_refresh(&fixture);
    expect_click(screen, "DISARM MOTION", APP_ARM, 0, 0);
    click(screen, "+45");
    expect_click(screen, "SEND POSITION", APP_MOVE, 450, 0);
    click(screen, "-0.1");
    expect_click(screen, "SEND POSITION", APP_MOVE, 449, 0);
    click(screen, "ZERO");
    expect_click(screen, "SEND POSITION", APP_MOVE, 0, 0);
    /* Staged device values may widen; motion must retain the host's tighter envelope. */
    fixture.host_negative_limit = -200; fixture.host_positive_limit = 200;
    fui_refresh(&fixture);
    click(screen, "+45");
    expect_click(screen, "SEND POSITION", APP_MOVE, 200, 0);
    click(screen, "-45");
    expect_click(screen, "SEND POSITION", APP_MOVE, -200, 0);
    fixture.host_negative_limit = -1000; fixture.host_positive_limit = 1000;
    fui_refresh(&fixture);
    expect_click(screen, "START SWEEP", APP_DEMO_START, 300, 6000);
    expect_click(screen, "PAUSE TEST", APP_STOP_MOTION, 0, 0);
    unsigned before = submitted;
    click(screen, "SET CURRENT AS CENTER"); assert(submitted == before);
    click(screen, "CANCEL"); assert(submitted == before);
    click(screen, "SET CURRENT AS CENTER");
    expect_click(screen, "SET ZERO", APP_CENTER, 0, 0);
    expect_click(screen, "SET REV", APP_REVERSE, 0, 0);
    expect_click(screen, "SET ALARM", APP_UV_ALARM, 0, 0);
    expect_click(screen, "SET MIN", APP_LIMIT_NEGATIVE, -1000, 0);
    expect_click(screen, "SET MAX", APP_LIMIT_POSITIVE, 1000, 0);
    expect_click(screen, "SET NODE", APP_SET_NODE, 37, 0);
    expect_click(screen, "SET BAUD", APP_SET_BAUD, 500000, 0);
    before = submitted;
    click(screen, "SAVE PARAMETERS"); assert(submitted == before);
    click(screen, "CANCEL"); assert(submitted == before);
    click(screen, "SAVE PARAMETERS");
    expect_click(screen, "SEND SAVE", APP_SAVE, 0, 0);
    expect_click(screen, "CONNECT / RECONNECT", APP_CONNECT, 37, 500000);
    expect_click(screen, "RECOVER CAN BUS", APP_RECOVER_BUS, 0, 0);
    expect_click(screen, "CLEAR LOCAL ALERTS", APP_CLEAR_ALERTS, 0, 0);
    assert(actions_seen == ((1UL << ((unsigned)APP_CLEAR_ALERTS + 1U)) - 1U));
    puts("ALL APP ACTIONS REACHED WITH VERIFIED PARAMETERS; confirmation cancellation verified.");
}

int main(int argc, char **argv)
{
    const char *folder = argc > 1 ? argv[1] : ".";
    (void)mkdir(folder, 0755);
    lv_init();
    lv_display_t *display = lv_display_create(800, 480);
    lv_display_set_color_format(display, LV_COLOR_FORMAT_XRGB8888);
    lv_display_set_buffers(display, framebuffer, NULL, sizeof(framebuffer), LV_DISPLAY_RENDER_MODE_FULL);
    lv_display_set_flush_cb(display, flush);
    lv_obj_t *screen = lv_screen_active();
    fixture.preview = true;
    fixture.ready = false;
    fixture.usb_transport = true;
    snprintf(fixture.transport_status, sizeof(fixture.transport_status), "Connect candleLight to Korvo USB-A");
    fixture.telemetry_stale_ms = 800;
    fixture.host_negative_limit = -1000; fixture.host_positive_limit = 1000;
    fixture.node_id = fixture.pending_node_id = 37;
    fixture.bitrate = fixture.pending_bitrate = 500000;
    fixture.report_interval_ms = 50;
    snprintf(fixture.result, sizeof(fixture.result), "WAITING / connect isolated candleLight USB-CAN and request readback");
    fui_create(screen);
    screenshot(display, folder, "00-offline");

    fixture.adapter_connected = true;
    fixture.usb_vid = 0x1D50; fixture.usb_pid = 0x606F;
    snprintf(fixture.transport_status, sizeof(fixture.transport_status), "ONE_SHOT unavailable; compatible firmware required");
    fui_refresh(&fixture);
    fui_show_page(FUI_LINK);
    assert(find_label(screen, "USB-CAN NOT READY"));
    screenshot(display, folder, "00b-usb-capability-fault");
    fixture.ready = true;
    snprintf(fixture.transport_status, sizeof(fixture.transport_status), "USB-CAN ready / ONE_SHOT / TX is USB acceptance only");
    fui_refresh(&fixture);
    fui_show_page(FUI_MONITOR);
    assert(find_label(screen, "NO CAN RESPONSE"));
    screenshot(display, folder, "00c-usb-no-servo");

    fixture.preview = true;
    fixture.online = true; fixture.position_valid = true; fixture.status_valid = true;
    fixture.features_valid = true; fixture.node_valid = true; fixture.version_valid = true;
    fixture.negative_limit_valid = fixture.positive_limit_valid = true;
    fixture.position_age_ms = 23; fixture.status_age_ms = 177;
    fixture.negative_limit = -1000; fixture.positive_limit = 1000;
    fixture.device_node_id = 37; fixture.feature_bits = 0x20; fixture.feature_raw = 0x20;
    fixture.current_10ma = 37; fixture.temperature_c = 31;
    fixture.tx_count = 128; fixture.rx_count = 306;
    fixture.reporting_requested = true;
    snprintf(fixture.version, sizeof(fixture.version), "FIXTURE-v1");
    snprintf(fixture.result, sizeof(fixture.result), "PREVIEW ONLY / values injected by the host fixture, no hardware connected");
    snprintf(fixture.log[0], APP_LOG_WIDTH, "[FIXTURE] Position request queued by controller");
    snprintf(fixture.log[1], APP_LOG_WIDTH, "[FIXTURE] Position reply decoded into command-axis coordinates");
    snprintf(fixture.log[2], APP_LOG_WIDTH, "[FIXTURE] RX status: current 0.37 A, temperature 31 C");
    snprintf(fixture.log[3], APP_LOG_WIDTH, "[FIXTURE] Device flags 0x20; voltage alarm enabled");
    snprintf(fixture.log[4], APP_LOG_WIDTH, "[FIXTURE] Limits readback: -100.0 / +100.0 deg");
    snprintf(fixture.log[5], APP_LOG_WIDTH, "[FIXTURE] Motion is disarmed; no command sent");
    snprintf(fixture.log[6], APP_LOG_WIDTH, "[FIXTURE] Headless render; real device acceptance pending");
    /* An explicit deterministic trajectory fixture. There is no simulator path
     * inside the delivered firmware UI and these screenshots are watermarked. */
    static const int16_t trace[] = {-210,-210,-180,-135,-80,-10,70,145,206,258,292,315,327,327,327};
    for (unsigned i = 0; i < 120; ++i) {
        fixture.position_deci_deg = trace[i / 8];
        lv_tick_inc(200);
        fui_refresh(&fixture);
    }
    for (unsigned page = 0; page < 5; ++page) {
        static const char *names[] = {"01-monitor", "02-motion", "03-service-top", "06-link", "07-log"};
        fui_show_page((fui_page_t)page);
        screenshot(display, folder, names[page]);
        if (page == FUI_SERVICE) {
            lv_obj_t *heading = find_label(screen, "01 / READBACK DIRECTORY");
            lv_obj_t *service = lv_obj_get_parent(lv_obj_get_parent(heading));
            lv_obj_scroll_to_y(service, 177, LV_ANIM_OFF);
            screenshot(display, folder, "04-service-settings");
            lv_obj_scroll_to_y(service, 427, LV_ANIM_OFF);
            screenshot(display, folder, "04b-service-reports");
            lv_obj_scroll_to_y(service, 545, LV_ANIM_OFF);
            screenshot(display, folder, "05-service-save");
            lv_obj_scroll_to_y(service, 0, LV_ANIM_OFF);
        }
    }
    fui_show_page(FUI_SERVICE);
    click(screen, "SAVE PARAMETERS");
    screenshot(display, folder, "08-save-confirmation");
    click(screen, "CANCEL");
    fui_show_page(FUI_MONITOR);
    fixture.online = false; fixture.position_age_ms = 19230; fixture.status_age_ms = 18690;
    fui_refresh(&fixture);
    screenshot(display, folder, "09-stale-readback");
    fixture.online = true; fixture.position_age_ms = 23; fixture.status_age_ms = 177;
    fui_refresh(&fixture);
    check_actions(screen);
    fixture.position_deci_deg = -1005; fixture.position_outside_limits = true;
    fui_refresh(&fixture);
    unsigned recovery_before = submitted;
    click(screen, "SELECT NEAREST LIMIT");
    assert(submitted == recovery_before); /* Selecting a recovery target never moves. */
    expect_click(screen, "SEND POSITION", APP_MOVE, -1000, 0);
    assert(lv_obj_has_state(lv_obj_get_parent(find_label(screen, "START SWEEP")), LV_STATE_DISABLED));
    fui_show_page(FUI_MOTION);
    screenshot(display, folder, "11-outside-limit-recovery");
    fixture.position_deci_deg = 1800;
    fui_refresh(&fixture);
    recovery_before = submitted;
    click(screen, "SELECT NEAREST LIMIT");
    assert(submitted == recovery_before);
    expect_click(screen, "SEND POSITION", APP_MOVE, 1000, 0);
    fixture.position_outside_limits = false; fixture.position_deci_deg = 0;
    fui_refresh(&fixture);
    assert(lv_obj_has_state(lv_obj_get_parent(find_label(screen, "SELECT NEAREST LIMIT")), LV_STATE_DISABLED));
    puts("Out-of-range feedback remains visible; nearest-limit selection does not send motion.");
    /* Actual 3.66 wire layout, injected as a fixture; never claim live measurement. */
    fixture.feature_bits = 0x90; fixture.feature_raw = 0x5C90;
    fui_refresh(&fixture);
    assert(lv_obj_has_state(lv_obj_get_parent(find_label(screen, "SET REV")), LV_STATE_DISABLED));
    assert(lv_obj_has_state(lv_obj_get_parent(find_label(screen, "SET ALARM")), LV_STATE_DISABLED));
    assert(find_label(screen, "FLAGS READ ONLY / extended bits undocumented; REV and ALARM writes disabled."));
    fui_show_page(FUI_SERVICE);
    lv_obj_t *service_heading = find_label(screen, "01 / READBACK DIRECTORY");
    lv_obj_scroll_to_y(lv_obj_get_parent(lv_obj_get_parent(service_heading)), 177, LV_ANIM_OFF);
    screenshot(display, folder, "10-extended-flags-read-only");
    fixture.feature_raw = 0x20; fixture.feature_bits = 0x20;
    fui_refresh(&fixture);
    assert(!lv_obj_has_state(lv_obj_get_parent(find_label(screen, "SET REV")), LV_STATE_DISABLED));
    assert(!lv_obj_has_state(lv_obj_get_parent(find_label(screen, "SET ALARM")), LV_STATE_DISABLED));
    puts("Extended feature readback keeps REV/ALARM writes disabled; known layout restores controls.");
    fixture.limit_probe_restore_unverified = true;
    fui_refresh(&fixture);
    static const char *locked[] = {"DISARM MOTION", "SEND POSITION", "START SWEEP",
        "SET MIN", "SET MAX", "SET REV", "SET ALARM", "SET CURRENT AS CENTER",
        "SAVE PARAMETERS", "SET NODE", "SET BAUD", "SET INTERVAL", "START REPORT", "STOP REPORT"};
    for (unsigned i = 0; i < sizeof(locked) / sizeof(locked[0]); ++i)
        assert(lv_obj_has_state(lv_obj_get_parent(find_label(screen, locked[i])), LV_STATE_DISABLED));
    assert(!lv_obj_has_state(lv_obj_get_parent(find_label(screen, "PAUSE TEST")), LV_STATE_DISABLED));
    assert(!lv_obj_has_state(lv_obj_get_parent(find_label(screen, "READ LIMITS")), LV_STATE_DISABLED));
    fui_show_page(FUI_MOTION);
    screenshot(display, folder, "12-limit-restore-unverified");
    fixture.limit_probe_restore_unverified = false;
    fui_refresh(&fixture);
    assert(!lv_obj_has_state(lv_obj_get_parent(find_label(screen, "SAVE PARAMETERS")), LV_STATE_DISABLED));
    puts("Unverified limit restore disables motion and parameter writes, including SAVE; readback remains available.");
    /* Visual acceptance fixtures for the mounted-horn view: + left, zero up, - right. */
    static const int16_t view_angles[] = {450, 0, -450};
    static const char *view_names[] = {"13a-axis-positive", "13b-axis-zero", "13c-axis-negative"};
    fui_show_page(FUI_MONITOR);
    for (unsigned i = 0; i < sizeof(view_angles) / sizeof(view_angles[0]); ++i) {
        fixture.position_deci_deg = view_angles[i];
        fui_refresh(&fixture);
        screenshot(display, folder, view_names[i]);
    }
    /* Deleting the root deletes its refresh timer; no process or service remains. */
    lv_obj_clean(screen);
    lv_display_delete(display);
    lv_deinit();
    puts("Preview exit cleanly.");
    return 0;
}

