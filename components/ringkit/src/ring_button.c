/*
 * ringkit — BOOT button. See ring_button.h.
 *
 * Design notes:
 *  - One polling task (default 10 ms) feeds the pure state machine. The
 *    button is the only GPIO input on the board, so a small poll loop is
 *    simpler and more robust than an ISR + queue, and the debounce lives in
 *    the SM either way.
 *  - The task's context is not the LVGL task. Callbacks that touch LVGL must
 *    take RING_UI_LOCKED(-1) themselves.
 *  - ring_button_is_pressed() / ring_button_held_ms() read volatile snapshots
 *    written by the task, for a UI timer.
 */
#include "ringkit/ring_button.h"
#include "ring_button_sm.h"

#include "esp_log.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#define TAG "ring_button"

#define RING_BUTTON_TASK_PRIO  5
#define RING_BUTTON_STOP_MS    1000

static struct {
    ring_button_config_t cfg;
    ring_button_sm_t     sm;
    TaskHandle_t         task;
    SemaphoreHandle_t    stop_sem;
    bool                 running;
} s_ctx;

static void button_task(void *arg)
{
    (void)arg;

    while (s_ctx.running) {
        int level = gpio_get_level(s_ctx.cfg.gpio);
        bool pressed = s_ctx.cfg.active_high ? (level != 0) : (level == 0);
        uint32_t now = xTaskGetTickCount() * portTICK_PERIOD_MS;

        ring_button_evt_t evt = ring_button_sm_feed(&s_ctx.sm, pressed, now);
        if (evt != RING_BUTTON_EVT_NONE && s_ctx.cfg.cb != NULL) {
            s_ctx.cfg.cb(evt, s_ctx.cfg.user);
        }

        vTaskDelay(pdMS_TO_TICKS(s_ctx.cfg.poll_ms));
    }

    /* Signal that the loop has been left (no callback can be running now)
     * before the task handle goes away. */
    xSemaphoreGive(s_ctx.stop_sem);
    vTaskDelete(NULL);
}

esp_err_t ring_button_init(const ring_button_config_t *cfg)
{
    if (cfg == NULL || cfg->cb == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (cfg->gpio == GPIO_NUM_NC || cfg->gpio > GPIO_NUM_48) {
        return ESP_ERR_INVALID_ARG;
    }
    if (cfg->debounce_ms == 0 || cfg->poll_ms == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (cfg->long_press_ms != 0 && cfg->long_press_ms <= cfg->debounce_ms) {
        return ESP_ERR_INVALID_ARG;
    }
    if (cfg->double_click_ms != 0 && cfg->double_click_ms <= cfg->debounce_ms) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_ctx.task != NULL) {
        return ESP_OK;   /* idempotent */
    }

    s_ctx.cfg = *cfg;
    ring_button_sm_init(&s_ctx.sm, cfg->debounce_ms, cfg->long_press_ms,
                        cfg->double_click_ms);
    s_ctx.stop_sem = xSemaphoreCreateBinary();
    if (s_ctx.stop_sem == NULL) {
        return ESP_ERR_NO_MEM;
    }
    s_ctx.running = true;

    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << cfg->gpio),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = cfg->active_high ? GPIO_PULLUP_DISABLE : GPIO_PULLUP_ENABLE,
        .pull_down_en = cfg->active_high ? GPIO_PULLDOWN_ENABLE : GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&io_conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "gpio_config(GPIO%d) failed: %s",
                 (int)cfg->gpio, esp_err_to_name(err));
        s_ctx.running = false;
        vSemaphoreDelete(s_ctx.stop_sem);
        s_ctx.stop_sem = NULL;
        return err;
    }

    BaseType_t ok = xTaskCreate(button_task, "ring-button", cfg->stack_size,
                                NULL, RING_BUTTON_TASK_PRIO, &s_ctx.task);
    if (ok != pdPASS) {
        s_ctx.task = NULL;
        s_ctx.running = false;
        vSemaphoreDelete(s_ctx.stop_sem);
        s_ctx.stop_sem = NULL;
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "GPIO%d button task started (debounce %u ms, long %u ms, double %u ms)",
             (int)cfg->gpio, cfg->debounce_ms, cfg->long_press_ms,
             cfg->double_click_ms);
    return ESP_OK;
}

void ring_button_deinit(void)
{
    if (s_ctx.task == NULL) {
        return;
    }
    s_ctx.running = false;
    if (xSemaphoreTake(s_ctx.stop_sem, pdMS_TO_TICKS(RING_BUTTON_STOP_MS)) != pdTRUE) {
        ESP_LOGW(TAG, "button task did not stop within %d ms; leaving it running",
                 RING_BUTTON_STOP_MS);
        s_ctx.running = true;
        return;
    }
    s_ctx.task = NULL;
    vSemaphoreDelete(s_ctx.stop_sem);
    s_ctx.stop_sem = NULL;
    gpio_reset_pin(s_ctx.cfg.gpio);
    ESP_LOGI(TAG, "button task stopped");
}

bool ring_button_is_pressed(void)
{
    return s_ctx.sm.pressed;
}

uint32_t ring_button_held_ms(void)
{
    return s_ctx.sm.held_ms;
}
