#pragma once
#include <stdio.h>
#define ESP_LOGE(tag, ...) do { (void)(tag); if (0) fprintf(stderr, __VA_ARGS__); } while (0)
#define ESP_LOGI ESP_LOGE
typedef enum { ESP_LOG_NONE, ESP_LOG_ERROR, ESP_LOG_WARN, ESP_LOG_INFO, ESP_LOG_DEBUG, ESP_LOG_VERBOSE } esp_log_level_t;
static inline esp_log_level_t esp_log_level_get(const char *tag) { (void)tag; return ESP_LOG_INFO; }
static inline void esp_log_level_set(const char *tag, esp_log_level_t level) { (void)tag; (void)level; }
