/*
 * ringkit — button state machine (private, pure logic).
 *
 * No GPIO and no FreeRTOS: the caller feeds the raw level and a monotonic
 * millisecond timestamp and gets back at most one event. Keeping the debounce,
 * long-press and double-click timing in one pure module makes the behaviour
 * explicit and independent of the poll rate.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "ringkit/ring_button.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    RING_BUTTON_SM_IDLE = 0,   /**< waiting for a press                          */
    RING_BUTTON_SM_PRESSED,    /**< accepted press, counting the hold            */
    RING_BUTTON_SM_WAIT_DOUBLE,/**< released, CLICK sent, watching for a 2nd press */
    RING_BUTTON_SM_PRESSED2,   /**< second press of a double-click               */
} ring_button_sm_state_t;

typedef struct {
    uint32_t debounce_ms;
    uint32_t long_press_ms;
    uint32_t double_click_ms;

    ring_button_sm_state_t state;
    bool     raw_level;         /**< accepted (debounced) level                  */
    bool     raw_pending;       /**< candidate level                             */
    uint32_t raw_pending_since; /**< ms when raw_pending last changed            */
    uint32_t press_start;       /**< ms of the current/last accepted press       */
    uint32_t release_time;      /**< ms of the last accepted release             */
    bool     long_emitted;      /**< LONG_PRESS already sent for this press      */

    volatile bool     pressed;  /**< snapshot for ring_button_is_pressed()       */
    volatile uint32_t held_ms;  /**< snapshot for ring_button_held_ms()          */
} ring_button_sm_t;

void ring_button_sm_init(ring_button_sm_t *sm, uint32_t debounce_ms,
                         uint32_t long_press_ms, uint32_t double_click_ms);

/**
 * @brief Feed one sample.
 *
 * @param sm         state.
 * @param raw_pressed  raw (undebounced) level, true = pressed.
 * @param now_ms      monotonic milliseconds.
 * @return the event to report, or RING_BUTTON_EVT_NONE.
 */
ring_button_evt_t ring_button_sm_feed(ring_button_sm_t *sm, bool raw_pressed,
                                      uint32_t now_ms);

#ifdef __cplusplus
}
#endif
