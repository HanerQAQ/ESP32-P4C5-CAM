#pragma once

#include "esp_err.h"

/** Initialize the camera/video devices. Call immediately after boot. */
esp_err_t camera_service_init(void);

/** Open the configured camera devices and start their HTTP servers. */
esp_err_t camera_service_start(void);
