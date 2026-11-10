/*
 * ringkit — button state machine. See ring_button_sm.h.
 *
 * Timing model:
 *  - a level must be stable for debounce_ms before it is accepted, so a
 *    contact bounce never produces a spurious edge;
 *  - PRESS on the accepted press edge;
 *  - LONG_PRESS once, when the hold reaches long_press_ms (0 disables it);
 *  - CLICK on a short release (immediately, not delayed);
 *  - DOUBLE_CLICK when a second press + release lands inside double_click_ms
 *    of the previous release (0 disables it).
 *
 * A long press suppresses the CLICK for that press; a long second press
 * suppresses the DOUBLE_CLICK.
 */
#include "ring_button_sm.h"

/* Accept a level only after it has been stable for debounce_ms. */
static void update_level(ring_button_sm_t *sm, bool raw, uint32_t now)
{
    if (raw != sm->raw_pending) {
        sm->raw_pending = raw;
        sm->raw_pending_since = now;
    }
    if (sm->raw_pending != sm->raw_level &&
        now - sm->raw_pending_since >= sm->debounce_ms) {
        sm->raw_level = sm->raw_pending;
    }
}

void ring_button_sm_init(ring_button_sm_t *sm, uint32_t debounce_ms,
                         uint32_t long_press_ms, uint32_t double_click_ms)
{
    sm->debounce_ms = debounce_ms;
    sm->long_press_ms = long_press_ms;
    sm->double_click_ms = double_click_ms;
    sm->state = RING_BUTTON_SM_IDLE;
    sm->raw_level = false;
    sm->raw_pending = false;
    sm->raw_pending_since = 0;
    sm->press_start = 0;
    sm->release_time = 0;
    sm->long_emitted = false;
    sm->pressed = false;
    sm->held_ms = 0;
}

ring_button_evt_t ring_button_sm_feed(ring_button_sm_t *sm, bool raw, uint32_t now)
{
    update_level(sm, raw, now);
    sm->pressed = sm->raw_level;

    ring_button_evt_t evt = RING_BUTTON_EVT_NONE;

    switch (sm->state) {
    case RING_BUTTON_SM_IDLE:
        sm->held_ms = 0;
        if (sm->raw_level) {
            sm->press_start = now;
            sm->long_emitted = false;
            sm->state = RING_BUTTON_SM_PRESSED;
            evt = RING_BUTTON_EVT_PRESS;
        }
        break;

    case RING_BUTTON_SM_PRESSED:
        sm->held_ms = sm->raw_level ? (now - sm->press_start) : 0;
        if (!sm->raw_level) {
            sm->release_time = now;
            sm->held_ms = 0;
            if (sm->long_emitted || sm->double_click_ms == 0) {
                /* Long press: no click. Double-click disabled: click now. */
                sm->state = RING_BUTTON_SM_IDLE;
                if (!sm->long_emitted) {
                    evt = RING_BUTTON_EVT_CLICK;
                }
            } else {
                sm->state = RING_BUTTON_SM_WAIT_DOUBLE;
                evt = RING_BUTTON_EVT_CLICK;
            }
        } else if (!sm->long_emitted && sm->long_press_ms > 0 &&
                   now - sm->press_start >= sm->long_press_ms) {
            sm->long_emitted = true;
            evt = RING_BUTTON_EVT_LONG_PRESS;
        }
        break;

    case RING_BUTTON_SM_WAIT_DOUBLE:
        sm->held_ms = 0;
        if (sm->raw_level) {
            sm->press_start = now;
            sm->long_emitted = false;
            sm->state = RING_BUTTON_SM_PRESSED2;
            evt = RING_BUTTON_EVT_PRESS;
        } else if (now - sm->release_time >= sm->double_click_ms) {
            sm->state = RING_BUTTON_SM_IDLE;
        }
        break;

    case RING_BUTTON_SM_PRESSED2:
        sm->held_ms = sm->raw_level ? (now - sm->press_start) : 0;
        if (!sm->raw_level) {
            sm->state = RING_BUTTON_SM_IDLE;
            sm->held_ms = 0;
            if (!sm->long_emitted) {
                evt = RING_BUTTON_EVT_DOUBLE_CLICK;
            }
        } else if (!sm->long_emitted && sm->long_press_ms > 0 &&
                   now - sm->press_start >= sm->long_press_ms) {
            sm->long_emitted = true;
            evt = RING_BUTTON_EVT_LONG_PRESS;
        }
        break;

    default:
        sm->state = RING_BUTTON_SM_IDLE;
        break;
    }

    return evt;
}
