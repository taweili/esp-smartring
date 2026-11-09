/*
 * ringkit — shared look & geometry for the 360x360 round panel.
 *
 * Every application pulls its colours, panel geometry and motion timings from
 * here so the whole family of examples feels like one device.
 */
#pragma once

#include "lvgl.h"

/* ------------------------------------------------------------- panel */

/* 1.8" round IPS (ST77916), 360x360 px, centre (180, 180), radius 180.
 * The active area is the circle inscribed in the square: the four square
 * corners are outside the round glass and are never visible. */
#define RING_PANEL_W 360
#define RING_PANEL_H 360
#define RING_CX      (RING_PANEL_W / 2)  /* 180 */
#define RING_CY      (RING_PANEL_H / 2)  /* 180 */
#define RING_RADIUS  180                 /* round panel radius (px) */

/* Radius inside which content is guaranteed readable. RING_RADIUS is the
 * physical edge; the bezel/glass transition eats a few px. */
#define RING_SAFE_RADIUS 172

/* Distance from a square corner to the round glass, measured along the
 * diagonal (RING_RADIUS * (sqrt(2) - 1) ~= 75 px). Content closer to a
 * corner than this is invisible. */
#define RING_CORNER_DEPTH 75

/** Squared distance from the panel centre to (x, y), for circle tests. */
#define RING_DIST2(x, y)                                             \
    (((x) - RING_CX) * ((x) - RING_CX) + ((y) - RING_CY) * ((y) - RING_CY))

/** True when (x, y) lies inside the visible circle with @p margin to spare. */
#define RING_IN_CIRCLE(x, y, margin)                            \
    (RING_DIST2((x), (y)) <=                               \
     (RING_RADIUS - (margin)) * (RING_RADIUS - (margin)))

/* ------------------------------------------------------------- colours */

#define RING_COLOR_BG    lv_color_black()
#define RING_COLOR_FG    lv_color_white()
/** Fixed accent green used by the glyph grid. */
#define RING_COLOR_GREEN lv_color_hex(0x00E676)

/** HSV saturation / value for hue-cycling accents. */
#define RING_HUE_SAT 90
#define RING_HUE_VAL 95

/* ------------------------------------------------------------- motion */

/** Layered glyph reveal (examples/00_glyph_grid_reveal). */
#define RING_MS_REVEAL_OPEN  1350  /* 3x slower than the original 450 */
#define RING_MS_REVEAL_CLOSE 1050  /* 3x slower than the original 350 */
