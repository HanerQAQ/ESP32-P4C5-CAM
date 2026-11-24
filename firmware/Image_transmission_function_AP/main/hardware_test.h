#pragma once
#include <stddef.h>
#include "esp_err.h"

void hardware_test_init(void);
void hardware_test_camera_pass(void);
void hardware_test_camera_error(void);
esp_err_t hardware_test_json(char *buffer, size_t size);
