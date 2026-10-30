/*
 * ringkit — the Smart Ring Plus skeleton.
 *
 * Umbrella header: one include gives an application the whole board API
 * (board bring-up, display, touch, battery, power, audio, IMU, SD) plus the
 * LVGL helpers, shared theme and deterministic RNG.
 *
 * Applications never include "bsp/smartring_plus.h" directly — ringkit is the
 * only place that talks to the BSP.
 */
#pragma once

#include "ringkit/ring_types.h"
#include "ringkit/ring_theme.h"

#include "ringkit/ring_board.h"
#include "ringkit/ring_display.h"
#include "ringkit/ring_touch.h"
#include "ringkit/ring_battery.h"
#include "ringkit/ring_power.h"
#include "ringkit/ring_audio.h"
#include "ringkit/ring_imu.h"
#include "ringkit/ring_sd.h"

#include "ringkit/ring_ui.h"
#include "ringkit/ring_rand.h"

/** Skeleton version, for logging. */
#define RINGKIT_VERSION "1.0.0"
