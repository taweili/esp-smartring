/*
 * ringkit — BOOT button (GPIO0) event decoder.
 *
 * The Smart Ring Plus has exactly one button the ESP32 can read: the BOOT
 * button, wired to GPIO0 (active-low, 10 kΩ pull-up to 3V3, button to GND).
 * The other tactile switch is the power key, which goes to the EC190707 PMIC
 * and is not a GPIO — firmware cannot read it, only simulate its long press
 * through GPIO47 (ring_power_shutdown()).
 *
 * ring_button turns the raw level into four events:
 *   RING_BUTTON_EVT_PRESS         debounced press-down
 *   RING_BUTTON_EVT_LONG_PRESS    held for long_press_ms, once per press
 *   RING_BUTTON_EVT_CLICK         short press + release (fires immediately)
 *   RING_BUTTON_EVT_DOUBLE_CLICK  a second press + release within
 *                                 double_click_ms of the previous release
 *
 * CLICK fires on release; DOUBLE_CLICK fires on the second release, so a
 * double-click produces CLICK then DOUBLE_CLICK. A long press suppresses the
 * CLICK for that press; a long second press suppresses the DOUBLE_CLICK.
 *
 * Caveat: GPIO0 is a strapping pin. Holding the button while the ring powers
 * up or resets enters the ROM bootloader — that cannot be disabled.
 *
 * The callback runs on ring_button's own task, never the LVGL task. Take
 * RING_UI_LOCKED(-1) before touching LVGL objects.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "driver/gpio.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Decoded button events. NONE is only returned internally / by the SM. */
typedef enum {
    RING_BUTTON_EVT_NONE = -1,
    RING_BUTTON_EVT_PRESS = 0,
    RING_BUTTON_EVT_LONG_PRESS,
    RING_BUTTON_EVT_CLICK,
    RING_BUTTON_EVT_DOUBLE_CLICK,
    RING_BUTTON_EVT_COUNT   /**< number of real events (not a valid event) */
} ring_button_evt_t;

/** Event callback (ring_button task context; take RING_UI_LOCKED for LVGL). */
typedef void (*ring_button_cb_t)(ring_button_evt_t evt, void *user);

/** Everything ring_button needs at boot. Lives for the lifetime of the app. */
typedef struct {
    gpio_num_t       gpio;            /**< default GPIO_NUM_0 (BOOT) */
    bool             active_high;     /**< default false (BOOT is active-low) */
    uint32_t         debounce_ms;     /**< default 20 */
    uint32_t         long_press_ms;   /**< default 800; 0 disables */
    uint32_t         double_click_ms; /**< default 300; 0 disables */
    uint32_t         poll_ms;         /**< default 10 */
    uint32_t         stack_size;      /**< default 3072 bytes */
    ring_button_cb_t cb;              /**< required */
    void            *user;
} ring_button_config_t;

/** Default config: BOOT button (GPIO0, active-low) with the standard timings. */
#define RING_BUTTON_CONFIG_DEFAULT(cb_, user_)                      \
    {                                                               \
        .gpio = GPIO_NUM_0,                                         \
        .active_high = false,                                       \
        .debounce_ms = 20,                                          \
        .long_press_ms = 800,                                       \
        .double_click_ms = 300,                                     \
        .poll_ms = 10,                                              \
        .stack_size = 3072,                                         \
        .cb = (cb_),                                                \
        .user = (user_),                                            \
    }

/**
 * @brief Start the button task and decode @p cfg->gpio.
 *
 * Idempotent: a second call before ring_button_deinit() returns ESP_OK.
 * @return ESP_ERR_INVALID_ARG on a bad config, ESP_ERR_NO_MEM on task creation.
 */
esp_err_t ring_button_init(const ring_button_config_t *cfg);

/** Stop the button task and reset the GPIO. Safe to call when not running. */
void ring_button_deinit(void);

/** Snapshot of the accepted level (true = pressed). Safe from any task. */
bool ring_button_is_pressed(void);

/** Milliseconds the button has been held in the current press (0 if idle). */
uint32_t ring_button_held_ms(void);

#ifdef __cplusplus
}
#endif
