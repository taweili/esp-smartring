/*
 * ringkit — shared look & geometry for the 360x360 round panel.
 *
 * Every application pulls its colours, panel geometry and motion timings from
 * here so the whole family of examples feels like one device.
 */
#pragma once

#include "lvgl.h"

/* ------------------------------------------------------------- panel */

#define RING_PANEL_W 360
#define RING_PANEL_H 360
#define RING_CX      (RING_PANEL_W / 2)  /* 180 */
#define RING_CY      (RING_PANEL_H / 2)  /* 180 */
#define RING_RADIUS  180                 /* round panel radius (px) */

/* ------------------------------------------------------------- colours */

#define RING_COLOR_BG    lv_color_black()
#define RING_COLOR_FG    lv_color_white()
/** Fixed accent green used by the glyph grid. */
#define RING_COLOR_GREEN lv_color_hex(0x00E676)

/** HSV saturation / value for hue-cycling accents. */
#define RING_HUE_SAT 90
#define RING_HUE_VAL 95

/* ------------------------------------------------------------- motion */

/** Layered glyph reveal (examples/glyph-anime). */
#define RING_MS_REVEAL_OPEN  450
#define RING_MS_REVEAL_CLOSE 350
