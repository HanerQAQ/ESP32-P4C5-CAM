#ifndef ESP32_P4C5_CAM_PORT_H_
#define ESP32_P4C5_CAM_PORT_H_

#include "ESP32_P4C5_CAM.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ESP32_P4C5_CAM_I2C_ADDRESS       (0x54U)
#define ESP32_P4C5_CAM_I2C_ADDRESS_HAL   (ESP32_P4C5_CAM_I2C_ADDRESS << 1)
#define ESP32_P4C5_CAM_I2C_CLOCK_HZ      (400000U)
#define ESP32_P4C5_CAM_I2C_BUFFER_SIZE   (1024U)

#ifndef ESP32_P4C5_CAM_I2C_HANDLE
#define ESP32_P4C5_CAM_I2C_HANDLE        hi2c2
#endif

void esp32_p4c5_cam_port_delay_ms(uint32_t ms);
uint32_t esp32_p4c5_cam_port_get_tick_ms(void);

bool esp32_p4c5_cam_port_init(esp32_p4c5_cam_handle_t *handle);
bool esp32_p4c5_cam_port_is_ready(uint32_t timeout_ms);

esp32_p4c5_cam_err_t esp32_p4c5_cam_port_i2c_write(const uint8_t *data, size_t len, uint32_t timeout_ms);
esp32_p4c5_cam_err_t esp32_p4c5_cam_port_i2c_read(uint8_t *data, size_t len, uint32_t timeout_ms);

#ifdef __cplusplus
}
#endif

#endif
