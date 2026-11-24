#pragma once

#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t detection_service_init(uint32_t width, uint32_t height);
void detection_service_toggle_mode(void);
void detection_service_process_rgb565(const uint8_t *frame, size_t frame_size);
esp_err_t detection_service_get_json(char *buffer, size_t buffer_size);

#ifdef __cplusplus
}
#endif
