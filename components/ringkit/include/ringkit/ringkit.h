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
#include "ringkit/ring_wifi.h"
#include "ringkit/ring_bt.h"
#include "ringkit/ring_net.h"
#include "ringkit/ring_ws.h"
#include "ringkit/ring_chat.h"

#include "ringkit/ring_ui.h"
#include "ringkit/ring_rand.h"
#include "ringkit/ring_secret.h"

/* Shared internal infrastructure, also public so apps can reuse it. */
#include "ringkit/ring_ca.h"
#include "ringkit/ring_time.h"
#include "ringkit/ring_nvs.h"
#include "ringkit/ring_json.h"
#include "ringkit/ring_byte_ring.h"
#include "ringkit/ring_resample.h"
#include "ringkit/ring_http.h"

/** Skeleton version, for logging. */
#define RINGKIT_VERSION "1.0.0"
