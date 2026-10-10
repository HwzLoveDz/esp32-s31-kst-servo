/* SPDX-License-Identifier: MIT
 * KST component inspection console. UI values are always controller readback.
 * Preview data is injected by preview/render.c, never generated in this file.
 */
#include "fui.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define C_BG       0x111719
#define C_PANEL    0x1A2225
#define C_RAISED   0x283236
#define C_LINE     0x425055
#define C_TEXT     0xEEEBDD
#define C_MUTED    0xA3B2B5
#define C_AMBER    0xF1B955
#define C_CYAN     0x91D4D3
#define C_RED      0xFF927B
#define COLOR(x)   lv_color_hex(x)
#define DECI_FMT(v) ((v) < 0 ? "-" : ""), abs((int)(v)) / 10, abs((int)(v)) % 10
#define PAGE_COUNT 5
#define HISTORY_POINTS 120
#define REFRESH_MS 200
#define PI_F 3.14159265358979323846f

typedef struct {
    lv_obj_t *root, *page[PAGE_COUNT], *nav[PAGE_COUNT];
    lv_obj_t *connection, *subtitle, *result;
    lv_obj_t *angle, *position_age, *joint, *current, *temperature, *status_age;
    lv_obj_t *limits, *monitor_node, *monitor_state, *chart, *history_note;
    lv_chart_series_t *history;
    lv_obj_t *target, *target_label, *move, *arm, *demo, *pause, *motion_readback;
    lv_obj_t *nearest_limit, *motion_help;
    lv_obj_t *parameter_buttons[12];
    unsigned parameter_button_count;
    lv_obj_t *amplitude, *period;
    lv_obj_t *service_actual, *negative, *positive, *reverse, *uv;
    lv_obj_t *set_reverse, *set_uv, *feature_note;
    lv_obj_t *report_interval, *report_state, *save_state, *version;
    lv_obj_t *local_node, *local_baud, *device_node, *device_baud, *link_state;
    lv_obj_t *log_stats, *log_lines[APP_LOG_LINES], *alert;
    lv_obj_t *modal;
    lv_timer_t *timer;
    app_snapshot_t state;
    uint32_t chart_tick;
    uint32_t queue_error_tick;
    bool have_chart_tick;
    bool queue_error;
    unsigned selected_page;
} fui_state_t;
static fui_state_t ui;

static const uint32_t baud_values[] = {1000000, 800000, 500000, 250000, 125000, 100000, 50000, 20000};
static const char *page_names[] = {"MONITOR", "MOTION", "SERVICE", "LINK", "LOG"};

static lv_obj_t *box(lv_obj_t *p, int x, int y, int w, int h, uint32_t color)
{
    lv_obj_t *o = lv_obj_create(p);
    lv_obj_remove_style_all(o);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_size(o, w, h);
    lv_obj_set_style_bg_color(o, COLOR(color), 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    return o;
}

static lv_obj_t *label(lv_obj_t *p, int x, int y, int w, const char *text,
                       const lv_font_t *font, uint32_t color)
{
    lv_obj_t *o = lv_label_create(p);
    lv_obj_set_pos(o, x, y);
    if (w > 0) lv_obj_set_width(o, w);
    lv_label_set_text(o, text);
    lv_obj_set_style_text_font(o, font, 0);
    lv_obj_set_style_text_color(o, COLOR(color), 0);
    lv_obj_set_style_text_line_space(o, 3, 0);
    return o;
}

static void rule(lv_obj_t *p, int x, int y, int w, uint32_t color)
{
    box(p, x, y, w, 1, color);
}

static lv_obj_t *button(lv_obj_t *p, int x, int y, int w, const char *text,
                        bool primary, lv_event_cb_t cb, void *data)
{
    lv_obj_t *o = lv_button_create(p);
    lv_obj_remove_style_all(o);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_size(o, w, 42);
    lv_obj_set_style_radius(o, 3, 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(o, COLOR(primary ? C_AMBER : C_RAISED), 0);
    lv_obj_set_style_border_width(o, primary ? 0 : 1, 0);
    lv_obj_set_style_border_color(o, COLOR(C_LINE), 0);
    lv_obj_set_style_bg_color(o, COLOR(primary ? 0xD49B39 : 0x3F5057), LV_STATE_PRESSED);
    lv_obj_set_style_opa(o, LV_OPA_40, LV_STATE_DISABLED);
    lv_obj_set_style_outline_width(o, 2, LV_STATE_FOCUS_KEY);
    lv_obj_set_style_outline_color(o, COLOR(C_CYAN), LV_STATE_FOCUS_KEY);
    lv_obj_t *t = label(o, 0, 0, 0, text, &lv_font_montserrat_14,
                        primary ? C_BG : C_TEXT);
    lv_obj_center(t);
    if (cb) lv_obj_add_event_cb(o, cb, LV_EVENT_CLICKED, data);
    return o;
}

static void submit(app_action_t action, int32_t value, int32_t value2)
{
    if (!app_submit(action, value, value2)) {
        ui.queue_error = true;
        ui.queue_error_tick = lv_tick_get();
        lv_label_set_text(ui.result, "QUEUE FULL / command was not queued; retry after the current transaction");
        lv_obj_set_style_text_color(ui.result, COLOR(C_RED), 0);
    } else ui.queue_error = false;
}

static void modal_close(lv_event_t *event)
{
    (void)event;
    if (ui.modal) {
        lv_obj_delete(ui.modal);
        ui.modal = NULL;
    }
}

static void modal_confirm(lv_event_t *event)
{
    app_action_t action = (app_action_t)(uintptr_t)lv_event_get_user_data(event);
    modal_close(NULL);
    submit(action, 0, 0);
}

static void confirm(app_action_t action)
{
    if (ui.modal) return;
    ui.modal = box(ui.root, 0, 0, 800, 480, C_BG);
    lv_obj_set_style_bg_opa(ui.modal, LV_OPA_80, 0);
    lv_obj_add_flag(ui.modal, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t *card = box(ui.modal, 136, 112, 528, 244, C_PANEL);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_border_color(card, COLOR(C_AMBER), 0);
    bool save = action == APP_SAVE;
    label(card, 24, 22, 480, save ? "WRITE NONVOLATILE PARAMETERS?" : "REDEFINE THE CURRENT ZERO?",
          &lv_font_montserrat_20, C_AMBER);
    label(card, 24, 65, 480,
          save ? "Save sends a write request without an acknowledgement.\nPersistence must be checked after a power cycle.\nPending limits / node / bitrate need a restart."
               : "The present shaft position becomes zero immediately.\nOnly fine adjustment within +/-10 deg is accepted.\nThe controller pauses the test and disarms motion.",
          &lv_font_montserrat_16, C_TEXT);
    button(card, 24, 178, 232, "CANCEL", false, modal_close, NULL);
    button(card, 272, 178, 232, save ? "SEND SAVE" : "SET ZERO", true,
           modal_confirm, (void *)(uintptr_t)action);
}

static uint32_t selected_baud(lv_obj_t *dropdown)
{
    uint32_t index = lv_dropdown_get_selected(dropdown);
    if (index >= sizeof(baud_values) / sizeof(baud_values[0])) index = 2;
    return baud_values[index];
}

static void action_event(lv_event_t *event)
{
    app_action_t action = (app_action_t)((uintptr_t)lv_event_get_user_data(event) - 1U);
    int32_t a = 0, b = 0;
    switch (action) {
    case APP_ARM: a = !ui.state.armed; break;
    case APP_MOVE: a = lv_slider_get_value(ui.target); break;
    case APP_DEMO_START:
        a = lv_spinbox_get_value(ui.amplitude); b = lv_spinbox_get_value(ui.period); break;
    case APP_REVERSE: a = (int32_t)lv_dropdown_get_selected(ui.reverse); break;
    case APP_UV_ALARM: a = (int32_t)lv_dropdown_get_selected(ui.uv); break;
    case APP_LIMIT_NEGATIVE: a = lv_spinbox_get_value(ui.negative); break;
    case APP_LIMIT_POSITIVE: a = lv_spinbox_get_value(ui.positive); break;
    case APP_REPORT_INTERVAL: a = lv_spinbox_get_value(ui.report_interval); break;
    case APP_CONNECT: a = lv_spinbox_get_value(ui.local_node); b = (int32_t)selected_baud(ui.local_baud); break;
    case APP_SET_NODE: a = lv_spinbox_get_value(ui.device_node); break;
    case APP_SET_BAUD: a = (int32_t)selected_baud(ui.device_baud); break;
    case APP_SAVE: case APP_CENTER: confirm(action); return;
    default: break;
    }
    submit(action, a, b);
}

static lv_obj_t *action_button(lv_obj_t *p, int x, int y, int w, const char *text,
                               app_action_t action, bool primary)
{
    lv_obj_t *obj = button(p, x, y, w, text, primary, action_event,
                          (void *)(uintptr_t)((unsigned)action + 1U));
    if ((action >= APP_REPORT_START && action <= APP_REPORT_INTERVAL) ||
        (action >= APP_CENTER && action <= APP_SAVE)) {
        LV_ASSERT(ui.parameter_button_count < sizeof(ui.parameter_buttons) / sizeof(ui.parameter_buttons[0]));
        ui.parameter_buttons[ui.parameter_button_count++] = obj;
    }
    return obj;
}

/* The +/- touch controls modify values only. A separate SET / MOVE action sends.
 * Nodes step by 1; angle limits use 0.1 deg; demo period uses 100 ms. */
static void minus_event(lv_event_t *e) { lv_spinbox_decrement(lv_event_get_user_data(e)); }
static void plus_event(lv_event_t *e) { lv_spinbox_increment(lv_event_get_user_data(e)); }

static lv_obj_t *number(lv_obj_t *p, int x, int y, int w, int32_t min,
                        int32_t max, int32_t value, unsigned digits, unsigned decimal,
                        uint32_t step)
{
    lv_obj_t *n = lv_spinbox_create(p);
    lv_obj_set_pos(n, x + 44, y);
    lv_obj_set_size(n, w - 88, 42);
    lv_spinbox_set_range(n, min, max);
    lv_spinbox_set_digit_format(n, digits, decimal ? digits - decimal : 0);
    lv_spinbox_set_value(n, value);
    lv_spinbox_set_step(n, step);
    lv_obj_set_style_bg_color(n, COLOR(C_BG), 0);
    lv_obj_set_style_text_color(n, COLOR(C_TEXT), 0);
    lv_obj_set_style_text_font(n, &lv_font_montserrat_20, 0);
    lv_obj_set_style_border_color(n, COLOR(C_LINE), 0);
    lv_obj_set_style_border_width(n, 1, 0);
    lv_obj_set_style_radius(n, 3, 0);
    lv_obj_set_style_pad_top(n, 9, 0);
    lv_obj_set_style_pad_left(n, 8, 0);
    lv_obj_set_style_text_color(n, COLOR(C_TEXT), LV_PART_CURSOR);
    lv_obj_set_style_bg_color(n, COLOR(C_RAISED), LV_PART_CURSOR);
    /* The on-screen step remains stable even if a finger taps a digit. */
    lv_obj_remove_flag(n, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t *minus = button(p, x, y, 40, "-", false, minus_event, n);
    lv_obj_t *plus = button(p, x + w - 40, y, 40, "+", false, plus_event, n);
    lv_obj_add_event_cb(minus, minus_event, LV_EVENT_LONG_PRESSED_REPEAT, n);
    lv_obj_add_event_cb(plus, plus_event, LV_EVENT_LONG_PRESSED_REPEAT, n);
    return n;
}

static lv_obj_t *dropdown(lv_obj_t *p, int x, int y, int w, const char *options, unsigned selected)
{
    lv_obj_t *o = lv_dropdown_create(p);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_size(o, w, 42);
    lv_dropdown_set_options(o, options);
    lv_dropdown_set_selected(o, selected);
    lv_obj_set_style_bg_color(o, COLOR(C_BG), 0);
    lv_obj_set_style_text_color(o, COLOR(C_TEXT), 0);
    lv_obj_set_style_text_font(o, &lv_font_montserrat_16, 0);
    lv_obj_set_style_border_color(o, COLOR(C_LINE), 0);
    lv_obj_set_style_border_width(o, 1, 0);
    lv_obj_set_style_radius(o, 3, 0);
    lv_obj_set_style_pad_top(o, 12, 0);
    lv_obj_t *list = lv_dropdown_get_list(o);
    lv_obj_set_style_bg_color(list, COLOR(C_RAISED), 0);
    lv_obj_set_style_text_color(list, COLOR(C_TEXT), 0);
    lv_obj_set_style_text_font(list, &lv_font_montserrat_16, 0);
    lv_obj_set_style_pad_ver(list, 14, LV_PART_SELECTED);
    lv_obj_set_style_text_line_space(list, 20, 0);
    lv_obj_set_style_bg_color(list, COLOR(C_AMBER), LV_PART_SELECTED | LV_STATE_CHECKED);
    lv_obj_set_style_text_color(list, COLOR(C_BG), LV_PART_SELECTED | LV_STATE_CHECKED);
    return o;
}

static void target_changed(lv_event_t *event)
{
    (void)event;
    int value = lv_slider_get_value(ui.target);
    lv_label_set_text_fmt(ui.target_label, "%s%d.%d", DECI_FMT(value));
}

static void preset_event(lv_event_t *event)
{
    int value = (int)(intptr_t)lv_event_get_user_data(event);
    int min = lv_slider_get_min_value(ui.target), max = lv_slider_get_max_value(ui.target);
    if (value < min) value = min;
    if (value > max) value = max;
    lv_slider_set_value(ui.target, value, LV_ANIM_OFF);
    target_changed(NULL);
}

static void fine_event(lv_event_t *event)
{
    int delta = (int)(intptr_t)lv_event_get_user_data(event);
    int value = lv_slider_get_value(ui.target) + delta;
    int min = lv_slider_get_min_value(ui.target), max = lv_slider_get_max_value(ui.target);
    if (value < min) value = min;
    if (value > max) value = max;
    lv_slider_set_value(ui.target, value, LV_ANIM_OFF);
    target_changed(NULL);
}

static bool position_fresh(const app_snapshot_t *s)
{
    uint32_t allowed = s->telemetry_stale_ms ? s->telemetry_stale_ms : 800U;
    return s->online && s->position_valid && s->position_age_ms <= allowed;
}

static bool status_fresh(const app_snapshot_t *s)
{
    uint32_t allowed = s->telemetry_stale_ms ? s->telemetry_stale_ms : 800U;
    return s->online && s->status_valid && s->status_age_ms <= allowed;
}

static void nearest_limit_event(lv_event_t *event)
{
    (void)event;
    if (!position_fresh(&ui.state) || !ui.state.position_outside_limits) return;
    int lo = lv_slider_get_min_value(ui.target), hi = lv_slider_get_max_value(ui.target);
    int target = ui.state.position_deci_deg < lo ? lo : hi;
    lv_slider_set_value(ui.target, target, LV_ANIM_OFF);
    target_changed(NULL); /* Editing only; ARM and SEND remain explicit. */
}

static void draw_line(lv_layer_t *layer, int x1, int y1, int x2, int y2,
                       int width, uint32_t color)
{
    lv_draw_line_dsc_t d;
    lv_draw_line_dsc_init(&d);
    d.p1.x = x1; d.p1.y = y1; d.p2.x = x2; d.p2.y = y2;
    d.width = width; d.color = COLOR(color); d.opa = LV_OPA_COVER;
    lv_draw_line(layer, &d);
}

static void draw_ring(lv_layer_t *layer, int x, int y, int radius, int width, uint32_t color)
{
    lv_draw_arc_dsc_t d;
    lv_draw_arc_dsc_init(&d);
    d.center.x = x; d.center.y = y; d.radius = radius; d.width = width;
    d.start_angle = 0; d.end_angle = 360; d.color = COLOR(color); d.opa = LV_OPA_COVER;
    lv_draw_arc(layer, &d);
}

/* A measured-angle schematic, not a calibrated mechanical CAD drawing.
 * An unknown / stale position has no arm: an invented neutral position is unsafe. */
static void joint_draw(lv_event_t *event)
{
    lv_obj_t *obj = lv_event_get_target_obj(event);
    lv_area_t a; lv_obj_get_coords(obj, &a);
    lv_layer_t *layer = lv_event_get_layer(event);
    int cx = a.x1 + 206, cy = a.y1 + 124;
    const int radius = 106;
    draw_ring(layer, cx, cy, 88, 1, C_LINE);
    draw_ring(layer, cx, cy, 57, 1, C_LINE);
    draw_ring(layer, cx, cy, 29, 2, C_LINE);
    draw_ring(layer, cx, cy, 12, 3, C_MUTED);
    draw_line(layer, cx - 145, cy, cx + 144, cy, 1, C_RAISED);
    draw_line(layer, cx, cy - 112, cx, cy + 102, 1, C_RAISED);
    /* Mechanical cradle and four fastener holes. */
    draw_line(layer, cx - 74, cy + 52, cx - 74, cy + 91, 2, C_LINE);
    draw_line(layer, cx + 74, cy + 52, cx + 74, cy + 91, 2, C_LINE);
    draw_line(layer, cx - 74, cy + 91, cx + 74, cy + 91, 2, C_LINE);
    for (int dx = -66; dx <= 66; dx += 132) {
        draw_ring(layer, cx + dx, cy + 79, 5, 1, C_MUTED);
        draw_ring(layer, cx + dx, cy - 58, 4, 1, C_LINE);
    }
    /* Shaft view matches the mounted horn: positive counterclockwise, zero up.
     * This display transform does not change feedback or command coordinates. */
    for (int angle = -100; angle <= 100; angle += 10) {
        float rad = (float)angle * PI_F / 180.0f;
        int inner = radius - ((angle % 50 == 0) ? 12 : 6);
        int x1 = cx - (int)lroundf(sinf(rad) * inner);
        int y1 = cy - (int)lroundf(cosf(rad) * inner);
        int x2 = cx - (int)lroundf(sinf(rad) * radius);
        int y2 = cy - (int)lroundf(cosf(rad) * radius);
        draw_line(layer, x1, y1, x2, y2, angle == 0 ? 2 : 1, angle == 0 ? C_TEXT : C_MUTED);
    }
    if (position_fresh(&ui.state)) {
        float rad = (float)ui.state.position_deci_deg * PI_F / 1800.0f;
        int ex = cx - (int)lroundf(sinf(rad) * 81.0f);
        int ey = cy - (int)lroundf(cosf(rad) * 81.0f);
        draw_line(layer, cx, cy, ex, ey, 15, C_AMBER);
        draw_line(layer, cx, cy, ex, ey, 2, C_BG);
        draw_ring(layer, ex, ey, 10, 3, C_AMBER);
        draw_ring(layer, cx, cy, 22, 5, C_AMBER);
        draw_ring(layer, cx, cy, 8, 3, C_TEXT);
    }
}

static void build_monitor(lv_obj_t *page)
{
    lv_obj_t *left = box(page, 0, 0, 394, 344, C_PANEL);
    label(left, 16, 12, 360, "01 / OUTPUT JOINT", &lv_font_montserrat_14, C_MUTED);
    ui.angle = label(left, 16, 35, 190, "--", &lv_font_montserrat_48, C_TEXT);
    label(left, 194, 56, 50, "deg", &lv_font_montserrat_16, C_MUTED);
    ui.position_age = label(left, 16, 91, 356, "NO POSITION READBACK", &lv_font_montserrat_12, C_MUTED);
    ui.joint = box(left, 0, 104, 394, 218, C_PANEL);
    lv_obj_add_event_cb(ui.joint, joint_draw, LV_EVENT_DRAW_MAIN, NULL);
    label(ui.joint, 18, 154, 48, "+100", &lv_font_montserrat_12, C_MUTED);
    label(ui.joint, 337, 154, 48, "-100", &lv_font_montserrat_12, C_MUTED);
    label(ui.joint, 25, 42, 56, "AXIS\nVIEW", &lv_font_montserrat_12, C_MUTED);
    ui.limits = label(left, 16, 321, 366, "LIMIT READBACK  -- / --", &lv_font_montserrat_12, C_MUTED);

    lv_obj_t *right = box(page, 406, 0, 362, 344, C_PANEL);
    label(right, 16, 12, 330, "02 / DRIVE TELEMETRY", &lv_font_montserrat_14, C_MUTED);
    ui.current = label(right, 16, 42, 162, "-- A", &lv_font_montserrat_36, C_TEXT);
    ui.temperature = label(right, 199, 42, 145, "-- C", &lv_font_montserrat_36, C_TEXT);
    label(right, 16, 86, 160, "MOTOR CURRENT", &lv_font_montserrat_12, C_MUTED);
    label(right, 199, 86, 146, "INTERNAL TEMP", &lv_font_montserrat_12, C_MUTED);
    ui.status_age = label(right, 16, 108, 330, "NO STATUS READBACK", &lv_font_montserrat_12, C_MUTED);
    rule(right, 16, 131, 330, C_LINE);
    ui.monitor_node = label(right, 16, 143, 150, "NODE --", &lv_font_montserrat_14, C_CYAN);
    ui.monitor_state = label(right, 199, 143, 150, "DISARMED", &lv_font_montserrat_14, C_MUTED);
    ui.history_note = label(right, 16, 174, 330, "ANGLE HISTORY / 24 s / +/-100 deg", &lv_font_montserrat_12, C_MUTED);
    ui.chart = lv_chart_create(right);
    lv_obj_set_pos(ui.chart, 16, 196); lv_obj_set_size(ui.chart, 330, 87);
    lv_obj_set_style_bg_opa(ui.chart, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(ui.chart, 0, 0);
    lv_obj_set_style_pad_all(ui.chart, 0, 0);
    lv_obj_set_style_line_color(ui.chart, COLOR(C_LINE), LV_PART_MAIN);
    lv_obj_set_style_line_width(ui.chart, 1, LV_PART_MAIN);
    lv_obj_set_style_size(ui.chart, 0, 0, LV_PART_INDICATOR);
    lv_obj_set_style_line_width(ui.chart, 2, LV_PART_ITEMS);
    lv_chart_set_type(ui.chart, LV_CHART_TYPE_LINE);
    lv_chart_set_point_count(ui.chart, HISTORY_POINTS);
    lv_chart_set_range(ui.chart, LV_CHART_AXIS_PRIMARY_Y, -1800, 1800);
    lv_chart_set_div_line_count(ui.chart, 3, 5);
    ui.history = lv_chart_add_series(ui.chart, COLOR(C_CYAN), LV_CHART_AXIS_PRIMARY_Y);
    lv_chart_set_all_values(ui.chart, ui.history, LV_CHART_POINT_NONE);
    action_button(right, 16, 294, 160, "READ POSITION", APP_READ_POSITION, false);
    action_button(right, 186, 294, 160, "READ STATUS", APP_READ_STATUS, false);
}

static void build_motion(lv_obj_t *page)
{
    lv_obj_t *left = box(page, 0, 0, 478, 344, C_PANEL);
    label(left, 16, 12, 448, "01 / POSITION COMMAND", &lv_font_montserrat_14, C_MUTED);
    ui.target_label = label(left, 16, 37, 260, "0.0", &lv_font_montserrat_48, C_AMBER);
    label(left, 190, 61, 70, "deg", &lv_font_montserrat_16, C_MUTED);
    ui.motion_readback = label(left, 278, 49, 185, "ACTUAL  --\nTARGET IS NOT SENT", &lv_font_montserrat_14, C_MUTED);
    ui.target = lv_slider_create(left);
    lv_obj_set_pos(ui.target, 31, 114); lv_obj_set_size(ui.target, 416, 12);
    lv_slider_set_range(ui.target, -1000, 1000);
    lv_slider_set_value(ui.target, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(ui.target, COLOR(C_LINE), LV_PART_MAIN);
    lv_obj_set_style_bg_color(ui.target, COLOR(C_AMBER), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(ui.target, COLOR(C_TEXT), LV_PART_KNOB);
    lv_obj_set_style_pad_all(ui.target, 14, LV_PART_KNOB);
    lv_obj_set_ext_click_area(ui.target, 18);
    lv_obj_add_event_cb(ui.target, target_changed, LV_EVENT_VALUE_CHANGED, NULL);
    button(left, 16, 149, 76, "-45", false, preset_event, (void *)(intptr_t)-450);
    button(left, 100, 149, 76, "ZERO", false, preset_event, NULL);
    button(left, 184, 149, 76, "+45", false, preset_event, (void *)(intptr_t)450);
    button(left, 278, 149, 84, "-0.1", false, fine_event, (void *)(intptr_t)-1);
    button(left, 370, 149, 92, "+0.1", false, fine_event, (void *)(intptr_t)1);
    ui.arm = action_button(left, 16, 210, 162, "ARM MOTION", APP_ARM, false);
    ui.move = action_button(left, 190, 210, 272, "SEND POSITION", APP_MOVE, true);
    ui.nearest_limit = button(left, 16, 266, 446, "SELECT NEAREST LIMIT", false, nearest_limit_event, NULL);
    ui.motion_help = label(left, 16, 316, 446, "Edit only. ARM then SEND. Alerts: LOG / CLEAR.",
                          &lv_font_montserrat_12, C_MUTED);

    lv_obj_t *right = box(page, 490, 0, 278, 344, C_PANEL);
    label(right, 16, 12, 244, "02 / SWEEP TEST", &lv_font_montserrat_14, C_MUTED);
    label(right, 16, 43, 244, "AMPLITUDE / deg from zero", &lv_font_montserrat_12, C_MUTED);
    ui.amplitude = number(right, 16, 64, 246, 0, 1000, 300, 4, 1, 10);
    label(right, 16, 118, 244, "CYCLE / ms", &lv_font_montserrat_12, C_MUTED);
    ui.period = number(right, 16, 139, 246, 4000, 60000, 6000, 5, 0, 1000);
    ui.demo = action_button(right, 16, 194, 246, "START SWEEP", APP_DEMO_START, true);
    ui.pause = action_button(right, 16, 246, 246, "PAUSE TEST", APP_STOP_MOTION, false);
    label(right, 16, 299, 246, "Pause stops new commands.\nIt does not release motor torque.",
          &lv_font_montserrat_12, C_MUTED);
}

static lv_obj_t *service_card(lv_obj_t *page, int y, int height, const char *title)
{
    lv_obj_t *card = box(page, 0, y, 754, height, C_PANEL);
    label(card, 16, 12, 724, title, &lv_font_montserrat_14, C_MUTED);
    return card;
}

static void build_service(lv_obj_t *page)
{
    lv_obj_add_flag(page, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(page, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(page, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_style_bg_color(page, COLOR(C_LINE), LV_PART_SCROLLBAR);
    lv_obj_set_style_width(page, 4, LV_PART_SCROLLBAR);
    lv_obj_t *card = service_card(page, 0, 165, "01 / READBACK DIRECTORY");
    action_button(card, 16, 40, 134, "ALL READBACK", APP_REFRESH, true);
    action_button(card, 162, 40, 134, "READ FLAGS", APP_READ_FEATURES, false);
    action_button(card, 308, 40, 134, "READ LIMITS", APP_READ_LIMITS, false);
    action_button(card, 454, 40, 134, "READ NODE", APP_READ_NODE, false);
    action_button(card, 600, 40, 138, "READ VERSION", APP_READ_VERSION, false);
    ui.version = label(card, 16, 94, 721, "FIRMWARE VERSION  --", &lv_font_montserrat_14, C_CYAN);
    ui.service_actual = label(card, 16, 121, 720, "FLAGS  --    LIMITS  -- / --", &lv_font_montserrat_14, C_TEXT);

    card = service_card(page, 177, 238, "02 / MECHANICAL SETTINGS  [limits: SAVE + RESTART]");
    label(card, 16, 40, 340, "NEGATIVE LIMIT / deg", &lv_font_montserrat_12, C_MUTED);
    label(card, 390, 40, 340, "POSITIVE LIMIT / deg", &lv_font_montserrat_12, C_MUTED);
    ui.negative = number(card, 16, 61, 216, -1000, 0, -1000, 4, 1, 1);
    action_button(card, 244, 61, 110, "SET MIN", APP_LIMIT_NEGATIVE, false);
    ui.positive = number(card, 390, 61, 216, 0, 1000, 1000, 4, 1, 1);
    action_button(card, 618, 61, 120, "SET MAX", APP_LIMIT_POSITIVE, false);
    label(card, 16, 119, 340, "FEEDBACK REVERSE", &lv_font_montserrat_12, C_MUTED);
    label(card, 390, 119, 340, "LOW-VOLTAGE ALARM", &lv_font_montserrat_12, C_MUTED);
    ui.reverse = dropdown(card, 16, 140, 216, "OFF\nON", 0);
    ui.set_reverse = action_button(card, 244, 140, 110, "SET REV", APP_REVERSE, false);
    ui.uv = dropdown(card, 390, 140, 216, "OFF\nON", 0);
    ui.set_uv = action_button(card, 618, 140, 120, "SET ALARM", APP_UV_ALARM, false);
    ui.feature_note = label(card, 16, 198, 720, "Fields are proposed settings. Readback above confirms the device's response.",
          &lv_font_montserrat_12, C_MUTED);

    card = service_card(page, 427, 153, "03 / POSITION REPORT STREAM");
    label(card, 16, 41, 216, "INTERVAL / ms", &lv_font_montserrat_12, C_MUTED);
    ui.report_interval = number(card, 16, 63, 228, 10, 255, 100, 3, 0, 1);
    action_button(card, 256, 63, 140, "SET INTERVAL", APP_REPORT_INTERVAL, false);
    action_button(card, 408, 63, 154, "START REPORT", APP_REPORT_START, false);
    action_button(card, 574, 63, 164, "STOP REPORT", APP_REPORT_STOP, false);
    ui.report_state = label(card, 16, 119, 720, "REQUESTED  OFF   |   Frames and timestamps confirm activity.", &lv_font_montserrat_12, C_MUTED);

    card = service_card(page, 592, 177, "04 / ZERO REFERENCE & NONVOLATILE MEMORY");
    action_button(card, 16, 44, 268, "SET CURRENT AS CENTER", APP_CENTER, false);
    action_button(card, 302, 44, 268, "SAVE PARAMETERS", APP_SAVE, true);
    ui.save_state = label(card, 16, 103, 720, "NO PENDING WRITES", &lv_font_montserrat_14, C_CYAN);
    label(card, 16, 130, 720,
          "SAVE has no acknowledgement. Power-cycle, reconnect and read back to verify persistence.",
          &lv_font_montserrat_12, C_MUTED);

    card = service_card(page, 781, 104, "05 / FIRMWARE SERVICE BOUNDARY");
    label(card, 16, 41, 720,
          "Version query is supported. Firmware flashing requires the additional KST upgrade\nprotocol and a matching image; this console does not implement an undocumented updater.",
          &lv_font_montserrat_14, C_MUTED);
}

static void build_link(lv_obj_t *page)
{
    lv_obj_t *left = box(page, 0, 0, 375, 278, C_PANEL);
    label(left, 16, 12, 340, "01 / CONNECT THIS CONTROLLER", &lv_font_montserrat_14, C_MUTED);
    label(left, 16, 46, 340, "CURRENT DEVICE NODE / decimal", &lv_font_montserrat_12, C_MUTED);
    ui.local_node = number(left, 16, 66, 240, 1, 127, 37, 3, 0, 1);
    label(left, 16, 122, 340, "BUS BITRATE / bit/s", &lv_font_montserrat_12, C_MUTED);
    ui.local_baud = dropdown(left, 16, 143, 340, "1000000\n800000\n500000\n250000\n125000\n100000\n50000\n20000", 2);
    action_button(left, 16, 199, 340, "CONNECT / RECONNECT", APP_CONNECT, true);
    label(left, 16, 249, 340, "Local settings; does not rewrite the servo.", &lv_font_montserrat_12, C_MUTED);

    lv_obj_t *right = box(page, 387, 0, 381, 278, C_PANEL);
    label(right, 16, 12, 350, "02 / CHANGE THE DEVICE", &lv_font_montserrat_14, C_MUTED);
    label(right, 16, 46, 348, "NEW NODE / decimal", &lv_font_montserrat_12, C_MUTED);
    ui.device_node = number(right, 16, 66, 226, 1, 127, 37, 3, 0, 1);
    action_button(right, 252, 66, 113, "SET NODE", APP_SET_NODE, false);
    label(right, 16, 122, 348, "NEW BITRATE / bit/s", &lv_font_montserrat_12, C_MUTED);
    ui.device_baud = dropdown(right, 16, 143, 226, "1000000\n800000\n500000\n250000\n125000\n100000\n50000\n20000", 2);
    action_button(right, 252, 143, 113, "SET BAUD", APP_SET_BAUD, false);
    label(right, 16, 202, 348,
          "SET stages a device change. SAVE, power-cycle,\nthen reconnect using the new node / bitrate.",
          &lv_font_montserrat_12, C_MUTED);
    action_button(right, 16, 235, 226, "SAVE PARAMETERS", APP_SAVE, false);

    ui.link_state = label(page, 16, 284, 734, "CAN 2.0B / 29-bit extended / 8-byte frames\nACTIVE --    PENDING --", &lv_font_montserrat_12, C_CYAN);
}

static void build_log(lv_obj_t *page)
{
    lv_obj_t *card = box(page, 0, 0, 768, 344, C_PANEL);
    label(card, 16, 12, 600, "01 / TRANSACTION LOG", &lv_font_montserrat_14, C_MUTED);
    ui.log_stats = label(card, 16, 39, 734, "TX 0   RX 0   TIMEOUT 0   DROPPED 0   BUS ERR 0", &lv_font_montserrat_14, C_CYAN);
    rule(card, 16, 65, 736, C_LINE);
    for (unsigned i = 0; i < APP_LOG_LINES; ++i) {
        ui.log_lines[i] = label(card, 16, 77 + (int)i * 25, 736, "", &lv_font_montserrat_14, C_TEXT);
        lv_label_set_long_mode(ui.log_lines[i], LV_LABEL_LONG_DOT);
        lv_obj_set_height(ui.log_lines[i], 20);
    }
    ui.alert = label(card, 16, 257, 736, "No alert received. Device health is not certified by this console.", &lv_font_montserrat_12, C_MUTED);
    action_button(card, 16, 294, 224, "REFRESH READBACK", APP_REFRESH, false);
    action_button(card, 252, 294, 244, "RECOVER CAN BUS", APP_RECOVER_BUS, false);
    action_button(card, 508, 294, 244, "CLEAR LOCAL ALERTS", APP_CLEAR_ALERTS, false);
}

void fui_show_page(fui_page_t page)
{
    if (!ui.root || (unsigned)page >= PAGE_COUNT) return;
    ui.selected_page = (unsigned)page;
    for (unsigned i = 0; i < PAGE_COUNT; ++i) {
        if (i == (unsigned)page) lv_obj_remove_flag(ui.page[i], LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(ui.page[i], LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_bg_color(ui.nav[i], COLOR(i == (unsigned)page ? C_AMBER : C_PANEL), 0);
        lv_obj_set_style_text_color(lv_obj_get_child(ui.nav[i], 0),
                                    COLOR(i == (unsigned)page ? C_BG : C_MUTED), 0);
    }
}

static void nav_event(lv_event_t *event)
{
    fui_show_page((fui_page_t)(uintptr_t)lv_event_get_user_data(event));
}

static void refresh_timer(lv_timer_t *timer)
{
    (void)timer;
    app_snapshot_t state;
    app_get_snapshot(&state);
    fui_refresh(&state);
}

static void seed_fields(const app_snapshot_t *state)
{
    /* Seed proposed connection fields from actual controller configuration once.
     * Subsequent refreshes must not overwrite a user's in-progress edit. */
    if (state->node_id >= 1 && state->node_id <= 127) {
        lv_spinbox_set_value(ui.local_node, state->node_id);
        lv_spinbox_set_value(ui.device_node, state->node_valid ? state->device_node_id : state->node_id);
    }
    for (unsigned i = 0; i < sizeof(baud_values) / sizeof(baud_values[0]); ++i) {
        if (baud_values[i] == state->bitrate) {
            lv_dropdown_set_selected(ui.local_baud, i);
            lv_dropdown_set_selected(ui.device_baud, i);
        }
    }
    if (state->report_interval_ms >= 10 && state->report_interval_ms <= 255)
        lv_spinbox_set_value(ui.report_interval, (int32_t)state->report_interval_ms);
}

static void root_deleted(lv_event_t *event)
{
    (void)event;
    if (ui.timer) lv_timer_delete(ui.timer);
    ui.timer = NULL;
    ui.root = NULL;
}

void fui_create(lv_obj_t *parent)
{
    /* This controller has one display. Re-creation replaces its owned root. */
    if (ui.root) lv_obj_delete(ui.root);
    memset(&ui, 0, sizeof(ui));
    ui.root = box(parent, 0, 0, 800, 480, C_BG);
    lv_obj_add_event_cb(ui.root, root_deleted, LV_EVENT_DELETE, NULL);
    box(ui.root, 16, 13, 4, 33, C_AMBER);
    label(ui.root, 30, 9, 485, "ACTUATOR BAY / 01", &lv_font_montserrat_24, C_TEXT);
    ui.subtitle = label(ui.root, 31, 38, 505, "KST HS20-12-M-5518  /  COMPONENT INSPECTION", &lv_font_montserrat_12, C_MUTED);
    ui.connection = label(ui.root, 539, 16, 244, "NO CAN RESPONSE", &lv_font_montserrat_16, C_MUTED);
    lv_obj_set_style_text_align(ui.connection, LV_TEXT_ALIGN_RIGHT, 0);
    rule(ui.root, 16, 58, 768, C_LINE);
    for (unsigned i = 0; i < PAGE_COUNT; ++i) {
        ui.page[i] = box(ui.root, 16, 68, 768, 344, C_BG);
        lv_obj_add_flag(ui.page[i], LV_OBJ_FLAG_HIDDEN);
    }
    build_monitor(ui.page[FUI_MONITOR]);
    build_motion(ui.page[FUI_MOTION]);
    build_service(ui.page[FUI_SERVICE]);
    build_link(ui.page[FUI_LINK]);
    build_log(ui.page[FUI_LOG]);
    ui.result = label(ui.root, 16, 418, 768, "WAITING / no command has been sent", &lv_font_montserrat_12, C_MUTED);
    lv_label_set_long_mode(ui.result, LV_LABEL_LONG_DOT);
    lv_obj_set_height(ui.result, 17);
    for (unsigned i = 0; i < PAGE_COUNT; ++i) {
        ui.nav[i] = button(ui.root, 16 + (int)i * 156, 440, 144, page_names[i], false,
                           nav_event, (void *)(uintptr_t)i);
        lv_obj_set_height(ui.nav[i], 40);
        lv_obj_set_style_border_width(ui.nav[i], 0, 0);
        lv_obj_set_style_radius(ui.nav[i], 2, 0);
    }
    fui_show_page(FUI_MONITOR);
    ui.timer = lv_timer_create(refresh_timer, REFRESH_MS, NULL);
    app_snapshot_t state;
    app_get_snapshot(&state);
    seed_fields(&state);
    fui_refresh(&state);
}

static void enable(lv_obj_t *obj, bool yes)
{
    if (yes) lv_obj_remove_state(obj, LV_STATE_DISABLED);
    else lv_obj_add_state(obj, LV_STATE_DISABLED);
}

void fui_refresh(const app_snapshot_t *s)
{
    if (!s || !ui.root) return;
    ui.state = *s;
    const bool pf = position_fresh(s), sf = status_fresh(s);
    const bool writes_allowed = !s->limit_probe_restore_unverified;
    for (unsigned i = 0; i < ui.parameter_button_count; ++i)
        enable(ui.parameter_buttons[i], writes_allowed);
    if (s->preview) {
        lv_label_set_text(ui.subtitle, "PREVIEW FIXTURE / SYNTHETIC INPUT / NOT HARDWARE DATA");
    } else {
        lv_label_set_text(ui.subtitle, "KST HS20-12-M-5518  /  COMPONENT INSPECTION");
    }
    const char *link = s->usb_transport && !s->adapter_connected ? "USB-CAN NOT CONNECTED" :
                       s->bus_off ? "CAN BUS OFF" : !s->ready ? (s->usb_transport ? "USB-CAN NOT READY" : "CAN NOT READY") :
                       s->online ? "CAN RESPONSE RECEIVED" : "NO CAN RESPONSE";
    lv_label_set_text(ui.connection, link);
    lv_obj_set_style_text_color(ui.connection, COLOR(s->bus_off ? C_RED : s->online ? C_CYAN : C_MUTED), 0);
    if (s->position_valid) {
        lv_label_set_text_fmt(ui.angle, "%s%d.%d", DECI_FMT(s->position_deci_deg));
        lv_label_set_text_fmt(ui.position_age, "%s / %lu ms / command-axis feedback",
                              pf ? "READBACK" : "STALE", (unsigned long)s->position_age_ms);
    } else {
        lv_label_set_text(ui.angle, "--");
        lv_label_set_text(ui.position_age, "NO POSITION READBACK / 0.1 deg encoding");
    }
    lv_obj_set_style_text_color(ui.angle, COLOR(pf ? C_TEXT : C_MUTED), 0);
    lv_obj_set_style_text_color(ui.position_age, COLOR(s->position_valid && !pf ? C_AMBER : C_MUTED), 0);
    lv_obj_invalidate(ui.joint);
    if (s->status_valid) {
        lv_label_set_text_fmt(ui.current, "%u.%02u A", s->current_10ma / 100U, s->current_10ma % 100U);
        lv_label_set_text_fmt(ui.temperature, "%d C", s->temperature_c);
        lv_label_set_text_fmt(ui.status_age, "%s / %lu ms", sf ? "READBACK" : "STALE", (unsigned long)s->status_age_ms);
    } else {
        lv_label_set_text(ui.current, "-- A"); lv_label_set_text(ui.temperature, "-- C");
        lv_label_set_text(ui.status_age, "NO STATUS READBACK");
    }
    lv_obj_set_style_text_color(ui.current, COLOR(sf ? C_TEXT : C_MUTED), 0);
    lv_obj_set_style_text_color(ui.temperature, COLOR(sf ? C_TEXT : C_MUTED), 0);
    lv_obj_set_style_text_color(ui.status_age, COLOR(s->status_valid && !sf ? C_AMBER : C_MUTED), 0);
    char min[24] = "--", max[24] = "--";
    if (s->negative_limit_valid) snprintf(min, sizeof(min), "%s%d.%d", DECI_FMT(s->negative_limit));
    if (s->positive_limit_valid) snprintf(max, sizeof(max), "%s%d.%d", DECI_FMT(s->positive_limit));
    lv_label_set_text_fmt(ui.limits, "READ %s/%s | HOST %s%d.%d/%s%d.%d deg", min, max,
                          DECI_FMT(s->host_negative_limit), DECI_FMT(s->host_positive_limit));
    lv_label_set_text_fmt(ui.monitor_node, "NODE %03u / %lu k", s->node_id, (unsigned long)s->bitrate / 1000UL);
    lv_label_set_text(ui.monitor_state, s->demo_active ? "SWEEP ACTIVE" : s->armed ? "ARMED" : "DISARMED");
    lv_obj_set_style_text_color(ui.monitor_state, COLOR(s->armed ? C_AMBER : C_MUTED), 0);
    if (s->position_valid) {
        lv_label_set_text_fmt(ui.motion_readback, "%s  %s%d.%d deg\n%s", pf ? "ACTUAL" : "STALE", DECI_FMT(s->position_deci_deg),
                              s->position_outside_limits ? "OUTSIDE TARGET LIMITS" : "Edit, then tap SEND");
    } else lv_label_set_text(ui.motion_readback, "ACTUAL  --\nEdit, then tap SEND");
    lv_label_set_text(lv_obj_get_child(ui.arm, 0), s->armed ? "DISARM MOTION" : "ARM MOTION");
    enable(ui.arm, writes_allowed && s->ready && !s->bus_off && (s->online || s->armed));
    enable(ui.move, writes_allowed && s->ready && s->online && s->armed && !s->bus_off && !s->busy);
    enable(ui.demo, writes_allowed && s->ready && s->online && s->armed && !s->bus_off && !s->busy && !s->demo_active && !s->position_outside_limits);
    enable(ui.nearest_limit, pf && s->position_outside_limits && !s->busy);
    lv_obj_set_style_text_color(ui.motion_readback, COLOR(s->position_outside_limits ? C_AMBER : C_MUTED), 0);
    lv_label_set_text(ui.motion_help, !writes_allowed ? "Limit restore unverified. Motion and parameter writes locked." :
                      s->abort_code ? "Clear alert in LOG, then ARM and SEND a valid target." :
                      "Edit only. ARM then SEND. Alerts: LOG / CLEAR.");
    /* Pause is always available: a timeout must never disable the stop request. */
    enable(ui.pause, true);
    if (s->host_negative_limit < s->host_positive_limit) {
        int lo = s->host_negative_limit < -1000 ? -1000 : s->host_negative_limit;
        int hi = s->host_positive_limit > 1000 ? 1000 : s->host_positive_limit;
        if (lo < hi && (lv_slider_get_min_value(ui.target) != lo || lv_slider_get_max_value(ui.target) != hi)) {
            lv_slider_set_range(ui.target, lo, hi);
            if (lv_slider_get_value(ui.target) < lo) lv_slider_set_value(ui.target, lo, LV_ANIM_OFF);
            if (lv_slider_get_value(ui.target) > hi) lv_slider_set_value(ui.target, hi, LV_ANIM_OFF);
            target_changed(NULL);
        }
    }
    lv_label_set_text_fmt(ui.version, "FIRMWARE VERSION  %s", s->version_valid ? s->version : "-- (not read)");
    if (s->features_valid) {
        lv_label_set_text_fmt(ui.service_actual, "FLAGS 0x%04X / REV %s / UV %s   LIMITS %s / %s deg",
                              (unsigned)s->feature_raw, (s->feature_bits & 0x40) ? "ON" : "OFF",
                              (s->feature_bits & 0x20) ? "ON" : "OFF", min, max);
    } else lv_label_set_text_fmt(ui.service_actual, "FLAGS -- (not read)     LIMITS %s / %s deg", min, max);
    bool unknown_features = s->features_valid && (s->feature_raw & 0xFF00U);
    bool can_set_features = writes_allowed && s->ready && s->online && s->features_valid && !s->busy &&
                            !s->bus_off && !unknown_features;
    enable(ui.set_reverse, can_set_features);
    enable(ui.set_uv, can_set_features);
    lv_label_set_text(ui.feature_note, unknown_features ?
        "FLAGS READ ONLY / extended bits undocumented; REV and ALARM writes disabled." :
        "Fields are proposed settings. Readback above confirms the device's response.");
    lv_obj_set_style_text_color(ui.feature_note, COLOR(unknown_features ? C_AMBER : C_MUTED), 0);
    lv_label_set_text_fmt(ui.report_state, "REQUESTED  %s  /  %lu ms   |   Frames and timestamps confirm activity.",
                          s->reporting_requested ? "ON" : "OFF", (unsigned long)s->report_interval_ms);
    lv_label_set_text(ui.save_state, !writes_allowed ? "RESTORE UNVERIFIED / PARAMETER WRITES AND SAVE LOCKED" :
                                   s->requires_restart ? "PENDING PARAMETERS / SAVE, RESTART, THEN READ BACK" :
                                   s->config_dirty ? "PARAMETERS MODIFIED / SAVE REQUIRED FOR PERSISTENCE" :
                                   "PERSISTENCE NOT VERIFIED / read back after a power cycle");
    lv_obj_set_style_text_color(ui.save_state, COLOR(s->config_dirty || s->requires_restart ? C_AMBER : C_MUTED), 0);
    char node_read[16] = "--";
    if (s->node_valid) snprintf(node_read, sizeof(node_read), "%03u", s->device_node_id);
    char transport_line[100];
    if (s->usb_transport)
        snprintf(transport_line, sizeof(transport_line), "candleLight USB %04X:%04X / CAN 2.0B / readback node %s",
                 s->usb_vid, s->usb_pid, node_read);
    else
        snprintf(transport_line, sizeof(transport_line), "TWAI / CAN 2.0B / 29-bit / 8-byte / readback node %s", node_read);
    if (s->requires_restart) {
        char pending_node[16] = "--", pending_baud[24] = "--";
        if (s->pending_node_id >= 1 && s->pending_node_id <= 127)
            snprintf(pending_node, sizeof(pending_node), "%03u", s->pending_node_id);
        if (s->pending_bitrate) snprintf(pending_baud, sizeof(pending_baud), "%lu", (unsigned long)s->pending_bitrate);
        lv_label_set_text_fmt(ui.link_state,
                              "%s\nACTIVE %03u / %lu bit/s   PENDING %s / %s bit/s / RESTART REQUIRED\n%s",
                              transport_line, s->node_id, (unsigned long)s->bitrate,
                              pending_node, pending_baud, s->transport_status);
    } else {
        lv_label_set_text_fmt(ui.link_state,
                              "%s\nACTIVE %03u / %lu bit/s   |   No staged restart reported\n%s",
                              transport_line, s->node_id, (unsigned long)s->bitrate, s->transport_status);
    }
    lv_label_set_text_fmt(ui.log_stats, "%s %lu   RX %lu   TIMEOUT %lu   DROPPED %lu   BUS ERR %lu",
                          s->usb_transport ? "USB TX" : "CAN TX", (unsigned long)s->tx_count, (unsigned long)s->rx_count,
                          (unsigned long)s->timeouts, (unsigned long)s->dropped_frames, (unsigned long)s->bus_errors);
    for (unsigned i = 0; i < APP_LOG_LINES; ++i) lv_label_set_text(ui.log_lines[i], s->log[i]);
    if (!writes_allowed) lv_label_set_text(ui.alert, "LIMIT RESTORE UNVERIFIED / writes locked; CLEAR and RECONNECT cannot unlock.");
    else if (s->bus_off) lv_label_set_text(ui.alert, "BUS OFF / inspect wiring, termination and bitrate before recovery.");
    else if (s->low_voltage_latched) lv_label_set_text(ui.alert, "LOW VOLTAGE RECEIVED / latched locally; check 12 V supply under load.");
    else if (s->abort_code) lv_label_set_text_fmt(ui.alert, "DEVICE ABORT / 0x%08lX", (unsigned long)s->abort_code);
    else if (s->usb_transport && !s->ready) lv_label_set_text(ui.alert, s->transport_status);
    else if (s->usb_transport) lv_label_set_text(ui.alert, "USB TX / echo confirms adapter acceptance only; verify servo readback.");
    else lv_label_set_text(ui.alert, "No alert received. Device health is not certified by this console.");
    lv_obj_set_style_text_color(ui.alert, COLOR(!writes_allowed || s->bus_off || s->low_voltage_latched || s->abort_code ? C_RED : C_MUTED), 0);
    uint32_t now = lv_tick_get();
    if (ui.queue_error && (uint32_t)(now - ui.queue_error_tick) >= 5000U) ui.queue_error = false;
    if (!ui.queue_error) {
        lv_label_set_text(ui.result, s->result[0] ? s->result : "WAITING / no completed transaction");
        lv_obj_set_style_text_color(ui.result, COLOR(s->bus_off || s->abort_code ? C_RED : C_MUTED), 0);
    }
    if (!ui.have_chart_tick || (uint32_t)(now - ui.chart_tick) >= REFRESH_MS) {
        /* Fixed-rate samples of received feedback, never interpolated or synthesized.
         * A long UI scheduling gap is represented explicitly instead of a fake line. */
        if (ui.have_chart_tick && (uint32_t)(now - ui.chart_tick) >= 2U * REFRESH_MS)
            lv_chart_set_next_value(ui.chart, ui.history, LV_CHART_POINT_NONE);
        lv_chart_set_next_value(ui.chart, ui.history, pf ? s->position_deci_deg : LV_CHART_POINT_NONE);
        ui.chart_tick = now; ui.have_chart_tick = true;
    }
}
