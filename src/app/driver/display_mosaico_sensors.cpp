// ESP-Mosaico sensor detail pages: full-screen graphical views reached by
// tapping a card on the sensors overview. The dock is hidden while a detail
// page is up (display_mosaico.cpp handles that), so the page gets y 56..472;
// the page container is deliberately non-scrollable so the up-swipe-back
// gesture is never suppressed by a scrollable ancestor.
//
// All shared UI context (theme colors, CJK-fallback fonts, tr(), snapshot
// access) comes from display_mosaico.cpp via display_mosaico_sensors.h.

#include "board_pins.h"

#if NRL_BOARD == NRL_BOARD_ESP_MOSAICO

#include "display_mosaico_sensors.h"

#include <lvgl.h>
#include <cmath>
#include <stdio.h>
#include <string.h>

namespace {

// Keep in sync with the AMOLED Dark theme in display_mosaico.cpp.
constexpr uint32_t kColorCard     = 0x12151B;
constexpr uint32_t kColorBorder   = 0x232A35;
constexpr uint32_t kColorAccent   = 0x22D3EE;
constexpr uint32_t kColorViolet   = 0xA78BFA;
constexpr uint32_t kColorText     = 0xF2F6FA;
constexpr uint32_t kColorSub      = 0x8A97A8;
constexpr uint32_t kColorGood     = 0x4ADE80;
constexpr uint32_t kColorWarn     = 0xF5B453;
constexpr uint32_t kColorBad      = 0xF87171;
constexpr uint32_t kColorBtnPress = 0x1B2A33;
constexpr uint32_t kColorBlue     = 0x60A5FA;

constexpr int kPageW = 480;
constexpr int kPageH = 416;  // content area with the dock hidden (y 56..472)

constexpr uint32_t kDetailFastRefreshMs = 100u;  // compass + bubble level
constexpr uint32_t kDetailSlowRefreshMs = 250u;  // gyro / mag / battery

// Magnetic interference threshold, same as the sensors overview page.
constexpr float kMagInterferenceUt = 15.0f;

int s_sensor = MOSAICO_SENSOR_DETAIL_ACCEL;

// Compass.
lv_obj_t *s_compass_needle = nullptr;
lv_point_precise_t s_compass_pts[2] = {};  // stored: lv_line keeps the pointer
lv_obj_t *s_compass_deg = nullptr;
lv_obj_t *s_compass_dir = nullptr;

// Bubble level.
lv_obj_t *s_level_bubble = nullptr;
lv_obj_t *s_level_vals = nullptr;
int s_level_base_x = 0;
int s_level_base_y = 0;

// Gyro history chart.
lv_obj_t *s_gyro_vals = nullptr;
lv_chart_series_t *s_gyro_ser_x = nullptr;
lv_chart_series_t *s_gyro_ser_y = nullptr;
lv_chart_series_t *s_gyro_ser_z = nullptr;
lv_obj_t *s_gyro_chart = nullptr;

// Magnetometer.
lv_obj_t *s_mag_arc = nullptr;
lv_obj_t *s_mag_big = nullptr;
lv_obj_t *s_mag_vals = nullptr;
lv_obj_t *s_mag_warn = nullptr;

// Battery.
lv_obj_t *s_batt_arc = nullptr;
lv_obj_t *s_batt_pct = nullptr;
lv_obj_t *s_batt_charge = nullptr;
lv_obj_t *s_batt_vals = nullptr;

const char *tr(const char *text)
{
    return MosaicoUi_Tr(text);
}

lv_obj_t *makeLabel(lv_obj_t *parent, const char *text, int font_px, uint32_t color)
{
    return MosaicoUi_MakeLabel(parent, text, MosaicoUi_Font(font_px), color);
}

void backEvent(lv_event_t *)
{
    MosaicoUi_ExitSensorDetail();
}

// Shared chrome: transparent full-size non-scrollable page, a "< Back" touch
// target (fallback for the up-swipe) and the page title. Returns the page.
lv_obj_t *buildChrome(lv_obj_t *content, const char *title)
{
    lv_obj_t *page = lv_obj_create(content);
    lv_obj_set_pos(page, 0, 0);
    lv_obj_set_size(page, kPageW, kPageH);
    lv_obj_set_style_bg_opa(page, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(page, 0, 0);
    lv_obj_set_style_radius(page, 0, 0);
    lv_obj_set_style_pad_all(page, 0, 0);
    lv_obj_set_scrollable(page, false);
    lv_obj_set_gesture_bubble(page, true);

    lv_obj_t *back = lv_button_create(page);
    lv_obj_set_scrollable(back, false);
    lv_obj_set_pos(back, 8, 6);
    lv_obj_set_size(back, 116, 40);
    lv_obj_set_style_radius(back, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(back, lv_color_hex(kColorCard), 0);
    lv_obj_set_style_bg_color(back, lv_color_hex(kColorBtnPress), LV_STATE_PRESSED);
    lv_obj_set_style_border_color(back, lv_color_hex(kColorBorder), 0);
    lv_obj_set_style_border_width(back, 1, 0);
    lv_obj_set_gesture_bubble(back, true);
    lv_obj_add_event_cb(back, backEvent, LV_EVENT_CLICKED, nullptr);
    char back_text[32];
    snprintf(back_text, sizeof(back_text), LV_SYMBOL_LEFT " %s", tr("Back"));
    lv_obj_t *back_label = makeLabel(back, back_text, 20, kColorAccent);
    lv_obj_center(back_label);

    lv_obj_t *title_label = makeLabel(page, tr(title), 20, kColorSub);
    lv_obj_align(title_label, LV_ALIGN_TOP_MID, 0, 16);
    return page;
}

// Round readout ring: non-interactive arc used as a gauge background.
void styleGaugeArc(lv_obj_t *arc, uint32_t indicator_color)
{
    lv_obj_set_clickable(arc, false);
    lv_obj_set_scrollable(arc, false);
    lv_obj_set_gesture_bubble(arc, true);
    lv_arc_set_rotation(arc, 270);
    lv_arc_set_bg_angles(arc, 0, 360);
    lv_obj_set_style_arc_color(arc, lv_color_hex(kColorBorder), LV_PART_MAIN);
    lv_obj_set_style_arc_color(arc, lv_color_hex(indicator_color), LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(arc, 14, LV_PART_MAIN);
    lv_obj_set_style_arc_width(arc, 14, LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(arc, true, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(arc, LV_OPA_TRANSP, LV_PART_KNOB);
    lv_obj_set_style_pad_all(arc, 0, LV_PART_KNOB);
}

// ---- Tilt-compensated heading -------------------------------------------------
// Standard e-compass formula: roll/pitch from the BMI270 gravity vector, the
// mag2 field rotated into the horizontal plane, then the same atan2(y, x)
// convention as the flat fallback in mosaico_sensors.cpp so the dial does not
// jump when the compensation drops out. Returns false without a usable
// gravity or field reference.
bool tiltCompensatedHeading(const MosaicoSensorSnapshot &snap, float *out_deg)
{
    if (!snap.mag2_valid || !snap.imu_valid) {
        return false;
    }
    const float ax = snap.accel_x_g;
    const float ay = snap.accel_y_g;
    const float az = snap.accel_z_g;
    if (ax * ax + ay * ay + az * az < 0.25f) {
        return false;  // free-fall / accelerating: no gravity reference
    }
    const float roll = atan2f(ay, az);
    const float pitch = atan2f(-ax, sqrtf(ay * ay + az * az));
    const float cr = cosf(roll);
    const float sr = sinf(roll);
    const float cp = cosf(pitch);
    const float sp = sinf(pitch);
    const float xh = snap.mag2_x_ut * cp + snap.mag2_y_ut * sr * sp +
                     snap.mag2_z_ut * cr * sp;
    const float yh = snap.mag2_y_ut * cr - snap.mag2_z_ut * sr;
    float deg = atan2f(yh, xh) * 57.2957795131f;
    if (deg < 0.0f) {
        deg += 360.0f;
    }
    *out_deg = deg;
    return true;
}

// Best available heading: tilt-compensated when possible, else the flat
// snapshot value. False when no magnetometer data at all.
bool currentHeading(const MosaicoSensorSnapshot &snap, float *out_deg)
{
    if (tiltCompensatedHeading(snap, out_deg)) {
        return true;
    }
    if (snap.mag2_valid) {
        *out_deg = snap.heading_deg;
        return true;
    }
    return false;
}

// ---- Compass (HEADING) ---------------------------------------------------------

constexpr int kCompassDial = 300;
constexpr int kCompassCx = kPageW / 2;
constexpr int kCompassCy = 236;
constexpr int kCompassNeedleR = 96;

void buildCompass(lv_obj_t *page)
{
    lv_obj_t *scale = lv_scale_create(page);
    lv_obj_set_scrollable(scale, false);
    lv_obj_set_gesture_bubble(scale, true);
    lv_obj_set_size(scale, kCompassDial, kCompassDial);
    lv_obj_set_pos(scale, kCompassCx - kCompassDial / 2, kCompassCy - kCompassDial / 2);
    lv_obj_set_style_pad_all(scale, 26, 0);
    lv_scale_set_mode(scale, LV_SCALE_MODE_ROUND_OUTER);
    lv_scale_set_range(scale, 0, 360);
    lv_scale_set_angle_range(scale, 360);
    lv_scale_set_rotation(scale, 270);  // value 0 (N) at the top
    lv_scale_set_total_tick_count(scale, 25);   // minor tick every 15 deg
    lv_scale_set_major_tick_every(scale, 2);    // label every 30 deg
    // The trailing "" suppresses the duplicate "360" label on top of "0".
    static const char *s_ticks[14];
    static const char *const kTickText[13] = {
        "0", "30", "60", "90", "120", "150", "180", "210", "240", "270",
        "300", "330", "",
    };
    for (int i = 0; i < 13; ++i) {
        s_ticks[i] = kTickText[i];
    }
    s_ticks[13] = nullptr;
    lv_scale_set_text_src(scale, s_ticks);
    lv_obj_set_style_text_font(scale, MosaicoUi_Font(14), LV_PART_INDICATOR);
    lv_obj_set_style_text_color(scale, lv_color_hex(kColorSub), LV_PART_INDICATOR);
    lv_obj_set_style_length(scale, 10, LV_PART_INDICATOR);
    lv_obj_set_style_length(scale, 5, LV_PART_ITEMS);
    lv_obj_set_style_line_color(scale, lv_color_hex(kColorSub), LV_PART_INDICATOR);
    lv_obj_set_style_line_color(scale, lv_color_hex(kColorBorder), LV_PART_ITEMS);
    lv_obj_set_style_line_color(scale, lv_color_hex(kColorBorder), LV_PART_MAIN);
    lv_obj_set_style_line_width(scale, 2, LV_PART_MAIN);

    // Cardinal letters inside the dial ring.
    static const char *const kCardinals[4] = {"N", "E", "S", "W"};
    for (int i = 0; i < 4; ++i) {
        const float rad = static_cast<float>(i) * 1.57079632679f;
        const int x = kCompassCx + static_cast<int>(82.0f * sinf(rad));
        const int y = kCompassCy - static_cast<int>(82.0f * cosf(rad));
        lv_obj_t *letter = makeLabel(page, tr(kCardinals[i]), 20,
                                     i == 0 ? kColorBad : kColorText);
        lv_obj_set_width(letter, 28);
        lv_obj_set_style_text_align(letter, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_pos(letter, x - 14, y - 12);
    }

    // Needle: a plain lv_line; lv_line keeps the point array pointer, so the
    // refresh only mutates the two endpoints and invalidates.
    s_compass_needle = lv_line_create(page);
    lv_obj_set_style_line_width(s_compass_needle, 4, 0);
    lv_obj_set_style_line_color(s_compass_needle, lv_color_hex(kColorBad), 0);
    lv_obj_set_style_line_rounded(s_compass_needle, true, 0);
    lv_obj_set_gesture_bubble(s_compass_needle, true);
    lv_obj_set_pos(s_compass_needle, kCompassCx - kCompassNeedleR,
                   kCompassCy - kCompassNeedleR);
    s_compass_pts[0].x = kCompassNeedleR;
    s_compass_pts[0].y = kCompassNeedleR;
    s_compass_pts[1].x = kCompassNeedleR;
    s_compass_pts[1].y = 0;
    lv_line_set_points(s_compass_needle, s_compass_pts, 2);

    lv_obj_t *hub = lv_obj_create(page);
    lv_obj_set_scrollable(hub, false);
    lv_obj_set_size(hub, 10, 10);
    lv_obj_set_style_radius(hub, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(hub, lv_color_hex(kColorText), 0);
    lv_obj_set_style_border_width(hub, 0, 0);
    lv_obj_set_pos(hub, kCompassCx - 5, kCompassCy - 5);

    s_compass_deg = makeLabel(page, "--", 28, kColorText);
    lv_obj_set_width(s_compass_deg, 160);
    lv_obj_set_style_text_align(s_compass_deg, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(s_compass_deg, kCompassCx - 80, kCompassCy + 26);
    s_compass_dir = makeLabel(page, "", 20, kColorViolet);
    lv_obj_set_width(s_compass_dir, 160);
    lv_obj_set_style_text_align(s_compass_dir, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(s_compass_dir, kCompassCx - 80, kCompassCy + 64);
}

void refreshCompass(const MosaicoSensorSnapshot &snap, bool ok)
{
    if (s_compass_deg == nullptr) {
        return;
    }
    float heading = 0.0f;
    if (ok && currentHeading(snap, &heading)) {
        const float rad = heading * 0.01745329251f;
        const float s = sinf(rad);
        const float c = cosf(rad);
        s_compass_pts[0].x = kCompassNeedleR - kCompassNeedleR * 0.22f * s;
        s_compass_pts[0].y = kCompassNeedleR + kCompassNeedleR * 0.22f * c;
        s_compass_pts[1].x = kCompassNeedleR + kCompassNeedleR * s;
        s_compass_pts[1].y = kCompassNeedleR - kCompassNeedleR * c;
        lv_obj_invalidate(s_compass_needle);
        char text[16];
        snprintf(text, sizeof(text), "%.0f\xC2\xB0", static_cast<double>(heading));
        lv_label_set_text(s_compass_deg, text);
        lv_label_set_text(s_compass_dir, MosaicoUi_CompassPoint(heading));
    } else {
        lv_label_set_text(s_compass_deg, "--");
        lv_label_set_text(s_compass_dir, tr("sensor absent"));
    }
}

// ---- Bubble level (ACCEL) -------------------------------------------------------

constexpr int kLevelDial = 280;
constexpr int kLevelCx = kPageW / 2;
constexpr int kLevelCy = 226;
constexpr int kLevelBubbleD = 26;
constexpr float kLevelTravel = 104.0f;  // bubble excursion at +/-1 g

lv_obj_t *levelRing(lv_obj_t *page, int diameter, uint32_t color, int width)
{
    lv_obj_t *ring = lv_obj_create(page);
    lv_obj_set_scrollable(ring, false);
    lv_obj_set_gesture_bubble(ring, true);
    lv_obj_set_size(ring, diameter, diameter);
    lv_obj_set_style_radius(ring, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(ring, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_color(ring, lv_color_hex(color), 0);
    lv_obj_set_style_border_width(ring, width, 0);
    lv_obj_set_pos(ring, kLevelCx - diameter / 2, kLevelCy - diameter / 2);
    return ring;
}

void buildLevel(lv_obj_t *page)
{
    levelRing(page, kLevelDial, kColorBorder, 2);
    levelRing(page, 150, kColorBorder, 1);
    lv_obj_t *dot = lv_obj_create(page);
    lv_obj_set_scrollable(dot, false);
    lv_obj_set_size(dot, 8, 8);
    lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(dot, lv_color_hex(kColorSub), 0);
    lv_obj_set_style_border_width(dot, 0, 0);
    lv_obj_set_pos(dot, kLevelCx - 4, kLevelCy - 4);

    s_level_bubble = lv_obj_create(page);
    lv_obj_set_scrollable(s_level_bubble, false);
    lv_obj_set_size(s_level_bubble, kLevelBubbleD, kLevelBubbleD);
    lv_obj_set_style_radius(s_level_bubble, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(s_level_bubble, lv_color_hex(kColorAccent), 0);
    lv_obj_set_style_border_width(s_level_bubble, 0, 0);
    s_level_base_x = kLevelCx - kLevelBubbleD / 2;
    s_level_base_y = kLevelCy - kLevelBubbleD / 2;
    lv_obj_set_pos(s_level_bubble, s_level_base_x, s_level_base_y);

    s_level_vals = makeLabel(page, "--", 16, kColorText);
    lv_obj_set_width(s_level_vals, kPageW - 32);
    lv_obj_set_style_text_align(s_level_vals, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(s_level_vals, 16, 372);
}

void refreshLevel(const MosaicoSensorSnapshot &snap, bool ok)
{
    if (s_level_bubble == nullptr) {
        return;
    }
    if (ok && snap.imu_valid) {
        // Bubble rides to the high side, like a real spirit level.
        float bx = snap.accel_x_g;
        float by = snap.accel_y_g;
        bx = bx > 1.0f ? 1.0f : (bx < -1.0f ? -1.0f : bx);
        by = by > 1.0f ? 1.0f : (by < -1.0f ? -1.0f : by);
        lv_obj_set_pos(s_level_bubble,
                       s_level_base_x + static_cast<int>(bx * kLevelTravel),
                       s_level_base_y + static_cast<int>(by * kLevelTravel));
        char text[64];
        snprintf(text, sizeof(text), "X %+.2f    Y %+.2f    Z %+.2f",
                 static_cast<double>(snap.accel_x_g),
                 static_cast<double>(snap.accel_y_g),
                 static_cast<double>(snap.accel_z_g));
        lv_label_set_text(s_level_vals, text);
    } else {
        lv_obj_set_pos(s_level_bubble, s_level_base_x, s_level_base_y);
        lv_label_set_text(s_level_vals, tr("sensor absent"));
    }
}

// ---- Gyro history (GYRO) ---------------------------------------------------------

constexpr int32_t kGyroChartRange = 360;  // +/- dps full scale

void buildGyro(lv_obj_t *page)
{
    s_gyro_vals = makeLabel(page, "--", 16, kColorText);
    lv_obj_set_width(s_gyro_vals, kPageW - 32);
    lv_obj_set_style_text_align(s_gyro_vals, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(s_gyro_vals, 16, 58);

    s_gyro_chart = lv_chart_create(page);
    lv_obj_set_scrollable(s_gyro_chart, false);
    lv_obj_set_gesture_bubble(s_gyro_chart, true);
    lv_obj_set_pos(s_gyro_chart, 16, 92);
    lv_obj_set_size(s_gyro_chart, kPageW - 32, 300);
    lv_obj_set_style_bg_color(s_gyro_chart, lv_color_hex(kColorCard), 0);
    lv_obj_set_style_border_color(s_gyro_chart, lv_color_hex(kColorBorder), 0);
    lv_obj_set_style_border_width(s_gyro_chart, 1, 0);
    lv_obj_set_style_radius(s_gyro_chart, 12, 0);
    lv_obj_set_style_line_width(s_gyro_chart, 2, LV_PART_ITEMS);
    lv_chart_set_type(s_gyro_chart, LV_CHART_TYPE_LINE);
    lv_chart_set_point_count(s_gyro_chart, 60);
    lv_chart_set_update_mode(s_gyro_chart, LV_CHART_UPDATE_MODE_SHIFT);
    lv_chart_set_axis_range(s_gyro_chart, LV_CHART_AXIS_PRIMARY_Y,
                            -kGyroChartRange, kGyroChartRange);
    lv_chart_set_div_line_count(s_gyro_chart, 5, 0);
    s_gyro_ser_x = lv_chart_add_series(s_gyro_chart, lv_color_hex(kColorBad),
                                       LV_CHART_AXIS_PRIMARY_Y);
    s_gyro_ser_y = lv_chart_add_series(s_gyro_chart, lv_color_hex(kColorGood),
                                       LV_CHART_AXIS_PRIMARY_Y);
    s_gyro_ser_z = lv_chart_add_series(s_gyro_chart, lv_color_hex(kColorBlue),
                                       LV_CHART_AXIS_PRIMARY_Y);
    for (int i = 0; i < 60; ++i) {
        lv_chart_set_next_value(s_gyro_chart, s_gyro_ser_x, 0);
        lv_chart_set_next_value(s_gyro_chart, s_gyro_ser_y, 0);
        lv_chart_set_next_value(s_gyro_chart, s_gyro_ser_z, 0);
    }
}

void refreshGyro(const MosaicoSensorSnapshot &snap, bool ok)
{
    if (s_gyro_chart == nullptr) {
        return;
    }
    if (ok && snap.imu_valid) {
        lv_chart_set_next_value(s_gyro_chart, s_gyro_ser_x,
                                static_cast<int32_t>(lroundf(snap.gyro_x_dps)));
        lv_chart_set_next_value(s_gyro_chart, s_gyro_ser_y,
                                static_cast<int32_t>(lroundf(snap.gyro_y_dps)));
        lv_chart_set_next_value(s_gyro_chart, s_gyro_ser_z,
                                static_cast<int32_t>(lroundf(snap.gyro_z_dps)));
        char text[80];
        snprintf(text, sizeof(text), "X %+.0f    Y %+.0f    Z %+.0f dps",
                 static_cast<double>(snap.gyro_x_dps),
                 static_cast<double>(snap.gyro_y_dps),
                 static_cast<double>(snap.gyro_z_dps));
        lv_label_set_text(s_gyro_vals, text);
    } else {
        lv_label_set_text(s_gyro_vals, tr("sensor absent"));
    }
}

// ---- Magnetometer (MAG) -----------------------------------------------------------

constexpr int kMagArcD = 264;
constexpr int kMagCx = kPageW / 2;
constexpr int kMagCy = 226;

float magFieldUt(const MosaicoSensorSnapshot &snap)
{
    const float x = snap.mag2_valid ? snap.mag2_x_ut : snap.mag3_x_ut;
    const float y = snap.mag2_valid ? snap.mag2_y_ut : snap.mag3_y_ut;
    const float z = snap.mag2_valid ? snap.mag2_z_ut : snap.mag3_z_ut;
    return sqrtf(x * x + y * y + z * z);
}

void buildMag(lv_obj_t *page)
{
    s_mag_warn = makeLabel(page, "", 16, kColorWarn);
    lv_obj_set_width(s_mag_warn, kPageW - 32);
    lv_obj_set_style_text_align(s_mag_warn, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(s_mag_warn, 16, 56);

    s_mag_arc = lv_arc_create(page);
    lv_obj_set_size(s_mag_arc, kMagArcD, kMagArcD);
    lv_obj_set_pos(s_mag_arc, kMagCx - kMagArcD / 2, kMagCy - kMagArcD / 2);
    lv_arc_set_range(s_mag_arc, 0, 100);
    styleGaugeArc(s_mag_arc, kColorViolet);
    lv_arc_set_value(s_mag_arc, 0);

    s_mag_big = makeLabel(page, "--", 28, kColorText);
    lv_obj_set_width(s_mag_big, 220);
    lv_obj_set_style_text_align(s_mag_big, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(s_mag_big, kMagCx - 110, kMagCy - 18);

    s_mag_vals = makeLabel(page, "--", 14, kColorSub);
    lv_obj_set_width(s_mag_vals, kPageW - 32);
    lv_obj_set_style_text_align(s_mag_vals, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(s_mag_vals, 16, 366);
}

void refreshMag(const MosaicoSensorSnapshot &snap, bool ok)
{
    if (s_mag_arc == nullptr) {
        return;
    }
    if (ok && (snap.mag2_valid || snap.mag3_valid)) {
        const float field = magFieldUt(snap);
        lv_arc_set_value(s_mag_arc,
                         static_cast<int32_t>(field > 100.0f ? 100.0f : field));
        char text[128];
        snprintf(text, sizeof(text), "%.1f uT", static_cast<double>(field));
        lv_label_set_text(s_mag_big, text);
        snprintf(text, sizeof(text), "#2 X %+.1f Y %+.1f Z %+.1f\n#3 X %+.1f Y %+.1f Z %+.1f",
                 static_cast<double>(snap.mag2_x_ut), static_cast<double>(snap.mag2_y_ut),
                 static_cast<double>(snap.mag2_z_ut),
                 static_cast<double>(snap.mag3_x_ut), static_cast<double>(snap.mag3_y_ut),
                 static_cast<double>(snap.mag3_z_ut));
        lv_label_set_text(s_mag_vals, text);

        const float dx = snap.mag2_x_ut - snap.mag3_x_ut;
        const float dy = snap.mag2_y_ut - snap.mag3_y_ut;
        const float dz = snap.mag2_z_ut - snap.mag3_z_ut;
        const float diff = sqrtf(dx * dx + dy * dy + dz * dz);
        const bool interference =
            snap.mag2_valid && snap.mag3_valid && diff > kMagInterferenceUt;
        lv_obj_set_style_arc_color(s_mag_arc,
                                   lv_color_hex(interference ? kColorBad : kColorViolet),
                                   LV_PART_INDICATOR);
        if (interference) {
            snprintf(text, sizeof(text), "%s %.0f uT", tr("Interference"),
                     static_cast<double>(diff));
            lv_label_set_text(s_mag_warn, text);
        } else {
            lv_label_set_text(s_mag_warn, "");
        }
    } else {
        lv_arc_set_value(s_mag_arc, 0);
        lv_label_set_text(s_mag_big, "--");
        lv_label_set_text(s_mag_vals, tr("sensor absent"));
        lv_label_set_text(s_mag_warn, "");
    }
}

// ---- Battery (BATTERY) ---------------------------------------------------------------

constexpr int kBattArcD = 264;
constexpr int kBattCx = kPageW / 2;
constexpr int kBattCy = 222;

void buildBattery(lv_obj_t *page)
{
    s_batt_arc = lv_arc_create(page);
    lv_obj_set_size(s_batt_arc, kBattArcD, kBattArcD);
    lv_obj_set_pos(s_batt_arc, kBattCx - kBattArcD / 2, kBattCy - kBattArcD / 2);
    lv_arc_set_range(s_batt_arc, 0, 100);
    styleGaugeArc(s_batt_arc, kColorAccent);
    lv_arc_set_value(s_batt_arc, 0);

    s_batt_charge = makeLabel(page, "", 20, kColorGood);
    lv_obj_set_width(s_batt_charge, 120);
    lv_obj_set_style_text_align(s_batt_charge, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(s_batt_charge, kBattCx - 60, kBattCy - 56);

    s_batt_pct = makeLabel(page, "--", 28, kColorText);
    lv_obj_set_width(s_batt_pct, 200);
    lv_obj_set_style_text_align(s_batt_pct, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(s_batt_pct, kBattCx - 100, kBattCy - 18);

    s_batt_vals = makeLabel(page, "--", 16, kColorSub);
    lv_obj_set_width(s_batt_vals, kPageW - 32);
    lv_obj_set_style_text_align(s_batt_vals, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(s_batt_vals, 16, 366);
}

void refreshBattery(const MosaicoSensorSnapshot &snap, bool ok)
{
    if (s_batt_arc == nullptr) {
        return;
    }
    if (ok && snap.gauge_present) {
        const int soc = snap.battery_soc_percent;
        lv_arc_set_value(s_batt_arc, soc);
        // Red when nearly empty, green while charging, accent otherwise.
        const uint32_t color =
            snap.battery_charging ? kColorGood : (soc < 20 ? kColorBad : kColorAccent);
        lv_obj_set_style_arc_color(s_batt_arc, lv_color_hex(color), LV_PART_INDICATOR);
        char text[96];
        snprintf(text, sizeof(text), "%d%%", soc);
        lv_label_set_text(s_batt_pct, text);
        lv_label_set_text(s_batt_charge,
                          snap.battery_charging ? LV_SYMBOL_CHARGE : "");
        snprintf(text, sizeof(text), "%u mV    %+d mA%s%s",
                 static_cast<unsigned>(snap.battery_mv),
                 static_cast<int>(snap.battery_current_ma),
                 snap.battery_charging ? "  " : "",
                 snap.battery_charging ? tr("Charging") : "");
        lv_label_set_text(s_batt_vals, text);
    } else {
        lv_arc_set_value(s_batt_arc, 0);
        lv_label_set_text(s_batt_pct, "--");
        lv_label_set_text(s_batt_charge, "");
        lv_label_set_text(s_batt_vals, tr("No gauge"));
    }
}

} // namespace

void MosaicoSensorDetail_Build(lv_obj_t *content, int sensor)
{
    // All widgets from a previous build were destroyed by lv_obj_clean().
    s_compass_needle = nullptr;
    s_compass_deg = nullptr;
    s_compass_dir = nullptr;
    s_level_bubble = nullptr;
    s_level_vals = nullptr;
    s_gyro_vals = nullptr;
    s_gyro_ser_x = nullptr;
    s_gyro_ser_y = nullptr;
    s_gyro_ser_z = nullptr;
    s_gyro_chart = nullptr;
    s_mag_arc = nullptr;
    s_mag_big = nullptr;
    s_mag_vals = nullptr;
    s_mag_warn = nullptr;
    s_batt_arc = nullptr;
    s_batt_pct = nullptr;
    s_batt_charge = nullptr;
    s_batt_vals = nullptr;

    if (sensor < 0 || sensor >= MOSAICO_SENSOR_DETAIL_COUNT) {
        sensor = MOSAICO_SENSOR_DETAIL_ACCEL;
    }
    s_sensor = sensor;

    static const char *const kTitles[MOSAICO_SENSOR_DETAIL_COUNT] = {
        "ACCEL (g)", "GYRO (dps)", "HEADING", "MAGNETOMETER (uT)", "BATTERY",
    };
    lv_obj_t *page = buildChrome(content, kTitles[s_sensor]);
    switch (s_sensor) {
        case MOSAICO_SENSOR_DETAIL_ACCEL: buildLevel(page); break;
        case MOSAICO_SENSOR_DETAIL_GYRO: buildGyro(page); break;
        case MOSAICO_SENSOR_DETAIL_HEADING: buildCompass(page); break;
        case MOSAICO_SENSOR_DETAIL_MAG: buildMag(page); break;
        case MOSAICO_SENSOR_DETAIL_BATTERY: buildBattery(page); break;
        default: break;
    }
    MosaicoSensorDetail_Refresh();
}

void MosaicoSensorDetail_Refresh(void)
{
    MosaicoSensorSnapshot snap = {};
    const bool ok = MosaicoUi_SensorSnapshot(&snap);
    switch (s_sensor) {
        case MOSAICO_SENSOR_DETAIL_ACCEL: refreshLevel(snap, ok); break;
        case MOSAICO_SENSOR_DETAIL_GYRO: refreshGyro(snap, ok); break;
        case MOSAICO_SENSOR_DETAIL_HEADING: refreshCompass(snap, ok); break;
        case MOSAICO_SENSOR_DETAIL_MAG: refreshMag(snap, ok); break;
        case MOSAICO_SENSOR_DETAIL_BATTERY: refreshBattery(snap, ok); break;
        default: break;
    }
}

uint32_t MosaicoSensorDetail_RefreshMs(void)
{
    return (s_sensor == MOSAICO_SENSOR_DETAIL_HEADING ||
            s_sensor == MOSAICO_SENSOR_DETAIL_ACCEL)
               ? kDetailFastRefreshMs
               : kDetailSlowRefreshMs;
}

#endif // NRL_BOARD == NRL_BOARD_ESP_MOSAICO
