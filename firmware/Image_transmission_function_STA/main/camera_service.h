#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

/** Initialize the camera/video devices. Call immediately after boot. */
esp_err_t camera_service_init(void);

/** Open the configured camera devices and start their HTTP servers. */
esp_err_t camera_service_start(void);

/** Camera runtime controls (mirror, flip, quality, resolution, stats) */
esp_err_t camera_service_set_hflip(int hflip);
esp_err_t camera_service_set_vflip(int vflip);
int camera_service_get_hflip(void);
int camera_service_get_vflip(void);
esp_err_t camera_service_set_quality(int quality);
int camera_service_get_quality(void);
esp_err_t camera_service_set_resolution(uint32_t width, uint32_t height);
void camera_service_get_resolution(uint32_t *width, uint32_t *height);
float camera_service_get_fps(void);

#ifdef __cplusplus
}
#endif
