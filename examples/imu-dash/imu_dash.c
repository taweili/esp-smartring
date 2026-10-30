/*
 * imu-dash — live IMU dashboard for the QMI8658A.
 *
 * Shows EVERY channel the BSP exposes on one screen:
 *   - a bubble level (the round panel is a natural spirit-level) driven by the
 *     screen-plane tilt vector from ring_imu_tilt()
 *   - the three board-axis accelerometer components (m/s²) on the left
 *   - the three board-axis gyroscope components (°/s) on the right
 *   - die temperature (°C, driver convention raw/256) at the bottom
 *   - the level-calibration state + observables (std_g, tilt_deg, elapsed)
 *
 * The calibration reference implementation is exercised here for the first
 * time: tapping anywhere starts/restarts it. Before it passes, the bubble
 * shows the raw (un-zeroed) tilt; once it passes on a flat surface the bubble
 * re-centres — which makes the calibration visibly "work".
 *
 * Refresh is a single lv_timer at 10 Hz. The BSP samples at 250 Hz and EMA
 * filters, so the UI only reads smoothed values. lv_timer callbacks run on the
 * LVGL task inside lv_timer_handler(), which already holds the (recursive)
 * adapter lock, so RING_UI_LOCKED inside the callback is re-entrant-safe; the
 * ring_imu_* reads take the BSP's own mutex and are independent of it.
 */
#include "ring_example.h"

#include "ringkit/ringkit.h"
#include "esp_log.h"

#include <math.h>
#include <stdio.h>

#define TAG "imu-dash"

#define UI_PERIOD_MS 100   /* 10 Hz UI refresh (BSP already samples at 250 Hz) */

/* ---- bubble level geometry (panel coordinates, 360x360) ---- */
#define BUBBLE_CX 180
#define BUBBLE_CY 118
#define BUBBLE_R  70       /* ring outer radius (px) */
#define DOT_R     8        /* level dot radius (px)   */
#define GRAVITY   9.807f   /* m/s², one g             */

/* ---- numeric columns ---- */
#define COL_L_NAME_X   16
#define COL_L_VALUE_X  64
#define COL_R_NAME_X   196
#define COL_R_VALUE_X  244
#define ROW_Y0         232
#define ROW_DY         32

static const char *const AXIS[3] = { "X", "Y", "Z" };

static lv_obj_t *s_dot;        /* the level dot (moves with tilt)  */
static lv_obj_t *s_a_val[3];   /* accel value labels, board axes   */
static lv_obj_t *s_g_val[3];   /* gyro value labels, board axes    */
static lv_obj_t *s_temp;       /* temperature label                */
static lv_obj_t *s_calib;      /* calibration status line          */
static uint32_t  s_tick;       /* tick counter, for telemetry       */

/* ------------------------------------------------------------------------- */
/* Small shape helpers (raw lv_obj avoids lv_line's precise-point API)       */
/* ------------------------------------------------------------------------- */

static lv_obj_t *make_circle(lv_obj_t *parent, int cx, int cy, int r,
                             lv_color_t color, bool filled)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_set_size(o, r * 2, r * 2);
    lv_obj_set_pos(o, cx - r, cy - r);
    lv_obj_set_style_radius(o, r, 0);
    lv_obj_set_style_border_width(o, filled ? 0 : 2, 0);
    lv_obj_set_style_border_color(o, color, 0);
    lv_obj_set_style_bg_color(o, color, 0);
    lv_obj_set_style_bg_opa(o, filled ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    return o;
}

static lv_obj_t *make_label(lv_obj_t *parent, const char *text,
                            const lv_font_t *font, lv_color_t color,
                            int x, int y)
{
    lv_obj_t *l = ring_ui_add_label(parent, text, font, color);
    lv_obj_set_pos(l, x, y);
    return l;
}

/* ------------------------------------------------------------------------- */
/* Periodic refresh                                                          */
/* ------------------------------------------------------------------------- */

static void ui_tick(lv_timer_t *timer)
{
    (void)timer;

    RING_UI_LOCKED(-1) {
        float ax, ay, az, gx, gy, gz, tx, ty, temp, std_g, tilt_deg;
        uint32_t elapsed_ms;

        ring_imu_accel(&ax, &ay, &az);
        ring_imu_gyro(&gx, &gy, &gz);
        ring_imu_temp(&temp);
        ring_imu_tilt(&tx, &ty);
        ring_imu_calib_info(&std_g, &tilt_deg, &elapsed_ms);

        char buf[96];
        const float a[3] = { ax, ay, az };
        const float g[3] = { gx, gy, gz };
        for (int i = 0; i < 3; i++) {
            snprintf(buf, sizeof buf, "%+.2f", a[i]);
            lv_label_set_text(s_a_val[i], buf);
            snprintf(buf, sizeof buf, "%+.1f", g[i]);
            lv_label_set_text(s_g_val[i], buf);
        }

        /* temperature: driver convention raw/256 °C (QMI8658A, 1 LSB = 1/256 °C) */
        snprintf(buf, sizeof buf, "TEMP %.1fC", temp);
        lv_label_set_text(s_temp, buf);

        ring_imu_calib_t st = ring_imu_calib_poll();
        switch (st) {
        case RING_IMU_CALIB_RUNNING:
            snprintf(buf, sizeof buf, "CALIBRATING %.1fs  std %.2fg  tilt %.1fdeg",
                     elapsed_ms / 1000.0f, std_g, tilt_deg);
            break;
        case RING_IMU_CALIB_PASSED:
            snprintf(buf, sizeof buf, "CALIB PASSED  std %.2fg  tilt %.1fdeg",
                     std_g, tilt_deg);
            break;
        case RING_IMU_CALIB_FAILED:
            snprintf(buf, sizeof buf, "CALIB FAILED  tap to retry");
            break;
        default:
            snprintf(buf, sizeof buf, "CALIB IDLE  tap to start");
            break;
        }
        lv_label_set_text(s_calib, buf);

        /* Bubble: in-plane gravity component (m/s²) maps to a dot offset.
         * The dot rolls toward the "low" side, i.e. the direction a ball
         * would roll. Clamped to the ring. */
        int lim = BUBBLE_R - DOT_R;
        int ox = (int)lroundf(tx / GRAVITY * lim);
        int oy = (int)lroundf(ty / GRAVITY * lim);
        if (ox >  lim) ox =  lim;
        if (ox < -lim) ox = -lim;
        if (oy >  lim) oy =  lim;
        if (oy < -lim) oy = -lim;
        lv_obj_set_pos(s_dot, BUBBLE_CX + ox - DOT_R, BUBBLE_CY + oy - DOT_R);

        /* Low-rate telemetry (every ~10 s) so a host can sanity-check the
         * sensor path from the serial console without touching the display. */
        if (++s_tick % 100 == 0) {
            ESP_LOGI(TAG, "accel %+.2f %+.2f %+.2f m/s2 | gyro %+.1f %+.1f %+.1f dps | temp %.1fC | tilt %+.2f %+.2f",
                     ax, ay, az, gx, gy, gz, temp, tx, ty);
        }
    }
}

/* ------------------------------------------------------------------------- */
/* Input: tap anywhere to (re)start the level calibration                    */
/* ------------------------------------------------------------------------- */

static void input_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
        ring_imu_calib_start();
        ESP_LOGI(TAG, "level calibration (re)started by tap");
    }
}

/* ------------------------------------------------------------------------- */
/* Build                                                                     */
/* ------------------------------------------------------------------------- */

esp_err_t ring_example_start(void)
{
    RING_UI_LOCKED(-1) {
        lv_obj_t *scr = ring_ui_screen();
        lv_color_t dim = lv_color_hex(0x808080);

        /* Title */
        lv_obj_t *title = ring_ui_add_label(scr, "IMU", &lv_font_montserrat_24,
                                            lv_color_white());
        ring_ui_place_centered(title, RING_CX, 22);

        /* Bubble level: ring outline + centre reference + moving dot */
        make_circle(scr, BUBBLE_CX, BUBBLE_CY, BUBBLE_R, dim, false); /* ring  */
        make_circle(scr, BUBBLE_CX, BUBBLE_CY, 2, dim, true);         /* centre */
        s_dot = make_circle(scr, BUBBLE_CX, BUBBLE_CY, DOT_R,
                            RING_COLOR_GREEN, true);

        /* Column headers */
        lv_obj_t *hdr = ring_ui_add_label(scr, "ACCEL", &lv_font_montserrat_14,
                                          dim);
        ring_ui_place_centered(hdr, 78, 214);
        hdr = ring_ui_add_label(scr, "GYRO", &lv_font_montserrat_14, dim);
        ring_ui_place_centered(hdr, 258, 214);

        /* Axis rows: name + value, left = accel, right = gyro */
        for (int i = 0; i < 3; i++) {
            int y = ROW_Y0 + i * ROW_DY;
            char name[8];
            snprintf(name, sizeof name, "A%s", AXIS[i]);
            make_label(scr, name, &lv_font_montserrat_20, dim,
                       COL_L_NAME_X, y);
            s_a_val[i] = make_label(scr, "--", &lv_font_montserrat_20,
                                    lv_color_white(), COL_L_VALUE_X, y);

            snprintf(name, sizeof name, "G%s", AXIS[i]);
            make_label(scr, name, &lv_font_montserrat_20, dim,
                       COL_R_NAME_X, y);
            s_g_val[i] = make_label(scr, "--", &lv_font_montserrat_20,
                                    lv_color_white(), COL_R_VALUE_X, y);
        }

        /* Temperature + calibration status */
        s_temp = ring_ui_add_label(scr, "TEMP --", &lv_font_montserrat_16,
                                   lv_color_white());
        ring_ui_place_centered(s_temp, RING_CX, 322);
        s_calib = ring_ui_add_label(scr, "CALIB IDLE", &lv_font_montserrat_14,
                                    dim);
        ring_ui_place_centered(s_calib, RING_CX, 344);

        /* Tap anywhere to start level calibration */
        lv_obj_add_event_cb(scr, input_cb, LV_EVENT_CLICKED, NULL);

        /* First paint immediately, then refresh at 10 Hz */
        ui_tick(NULL);
        lv_timer_create(ui_tick, UI_PERIOD_MS, NULL);
    }

    ESP_LOGI(TAG, "ready: accel + gyro + temp + tilt + calib dashboard");
    return ESP_OK;
}

const char *ring_example_name(void)
{
    return "imu-dash";
}
