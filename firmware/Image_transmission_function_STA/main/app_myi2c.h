#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>

#define P4C5_CAM_I2C_SLAVE_ADDR 0x52
#define P4C5_CAM_I2C_PORT       I2C_NUM_1
#define P4C5_CAM_I2C_SCL_PIN    47
#define P4C5_CAM_I2C_SDA_PIN    46

esp_err_t app_myi2c_init(void);

#ifdef __cplusplus
}
#endif
