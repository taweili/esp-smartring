/*
 * Host-test stub for esp_log.h — logs are compiled out.
 *
 * The arguments are intentionally not evaluated: the tests exercise the
 * logic, not the logging.
 */
#pragma once

#define ESP_LOGI(tag, fmt, ...) ((void)0)
#define ESP_LOGW(tag, fmt, ...) ((void)0)
#define ESP_LOGE(tag, fmt, ...) ((void)0)
#define ESP_LOGD(tag, fmt, ...) ((void)0)
#define ESP_LOGV(tag, fmt, ...) ((void)0)
