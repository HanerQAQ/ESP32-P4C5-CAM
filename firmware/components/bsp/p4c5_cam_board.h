#pragma once

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/i2c_slave.h"
#include "driver/uart.h"
#include "esp_err.h"
#include <driver/spi_common.h>
#include "config.h"
#include "display.h"
#include "lvgl.h"
#include "esp_lvgl_port.h"


/** 
 *  @brief I2C BSP API
 *  @{
 */
#define BSP_I2C1_SCL           (GPIO_NUM_29)    //camera
#define BSP_I2C1_SDA           (GPIO_NUM_28)
#define BSP_I2C1_NUM           (0)

#define BSP_I2C2_SCL           (GPIO_NUM_47)
#define BSP_I2C2_SDA           (GPIO_NUM_46)
#define BSP_I2C2_NUM           (1)

#define BSP_CSI_PWDN           (GPIO_NUM_54)
#define BSP_CSI_RST            (GPIO_NUM_53)

#define BSP_BUTTON_A           (GPIO_NUM_26)
#define BSP_BUTTON_B           (GPIO_NUM_35)

#define BSP_VOICE_POWER        (GPIO_NUM_11)
#define BSP_VOICE_ENABLE       (GPIO_NUM_12)
#define BSP_VOICE_UART_NUM     (UART_NUM_1)
#define BSP_VOICE_UART_TX      (GPIO_NUM_31)
#define BSP_VOICE_UART_RX      (GPIO_NUM_30)

#define BSP_VOICE_UART_BAUD    (115200)

/** 
 *  @brief Display BSP API
 *  @{
 */

#define BSP_LCD_SPI_PORT                (SPI3_HOST)  
#define BSP_LCD_DC_GPIO                 (GPIO_NUM_22)  
#define BSP_LCD_CS_GPIO                 (GPIO_NUM_7)  
#define BSP_LCD_RST_GPIO                (GPIO_NUM_NC)  
#define BSP_LCD_CLK_GPIO                (GPIO_NUM_9)  
#define BSP_LCD_MOSI_GPIO               (GPIO_NUM_20)  
#define BSP_LCD_BACKLIGHT               (GPIO_NUM_10)  
#define BSP_LCD_SPI_TRANS_QUEUE_DEPTH   (4)

#define LCD_LEDC_CH           (1)

#if (BSP_CONFIG_NO_GRAPHIC_LIB == 0)

#define BSP_LCD_DRAW_BUFF_SIZE     (BSP_LCD_V_RES * BSP_LCD_H_RES) // Frame buffer size in pixels
#define BSP_LCD_DRAW_BUFF_DOUBLE   (0)

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize the board I2C slave interface.
 *
 * Creates an I2C slave device, configures the selected SCL and SDA pins for
 * maximum drive capability, and registers the application callbacks.
 *
 * @param[in] config I2C slave configuration.
 * @param[in] callbacks I2C slave event callbacks.
 * @param[in] user_data User context passed to the event callbacks.
 * @param[out] handle Handle of the created I2C slave device.
 *
 * @return
 *      - ESP_OK: I2C slave initialized successfully.
 *      - ESP_ERR_INVALID_ARG: One or more arguments are invalid.
 *      - ESP_ERR_INVALID_STATE: The I2C slave is already initialized.
 *      - Other error codes returned by the GPIO or I2C drivers.
 */
esp_err_t bsp_i2c_slave_init(const i2c_slave_config_t *config,
                             const i2c_slave_event_callbacks_t *callbacks,
                             void *user_data,
                             i2c_slave_dev_handle_t *handle);

/**
 * @brief Deinitialize the board I2C slave interface.
 *
 * @param[in] handle Handle returned by bsp_i2c_slave_init().
 *
 * @return
 *      - ESP_OK: The interface was deinitialized or was not initialized.
 *      - ESP_ERR_INVALID_ARG: The supplied handle is invalid.
 *      - Other error codes returned by the I2C driver.
 */
esp_err_t bsp_i2c_slave_deinit(i2c_slave_dev_handle_t handle);

/**
 * @brief Initialize the board buttons.
 *
 * Configures BSP_BUTTON_A and BSP_BUTTON_B as active-low inputs with internal
 * pull-ups enabled and GPIO interrupts disabled.
 *
 * @return
 *      - ESP_OK: Buttons initialized successfully.
 *      - Other error codes returned by gpio_config().
 */
esp_err_t bsp_buttons_init(void);

/**
 * @brief Initialize the CI1302 UART and control GPIOs.
 *
 * Installs and configures the CI1302 UART driver, assigns the TX/RX pins, and
 * enables the CI1302 power and enable signals. Repeated calls are safe.
 *
 * @return
 *      - ESP_OK: CI1302 UART initialized successfully or already initialized.
 *      - Other error codes returned by the UART or GPIO drivers.
 */
esp_err_t bsp_ci1302_uart_init(void);

/**
 * @brief Deinitialize the CI1302 UART.
 *
 * Waits for pending UART transmission to finish and then removes the UART
 * driver. Calling this function before initialization is safe.
 *
 * @return
 *      - ESP_OK: CI1302 UART deinitialized successfully or was not initialized.
 *      - Other error codes returned by the UART driver.
 */
esp_err_t bsp_ci1302_uart_deinit(void);

/**
 * @brief BSP display configuration structure
 *
 */
typedef struct {
    lvgl_port_cfg_t lvgl_port_cfg;  /*!< LVGL port configuration */
    uint32_t        buffer_size;    /*!< Size of the buffer for the screen in pixels */
    bool            double_buffer;  /*!< True, if should be allocated two buffers */
    struct {
        unsigned int buff_dma: 1;    /*!< Allocated LVGL buffer will be DMA capable */
        unsigned int buff_spiram: 1; /*!< Allocated LVGL buffer will be in PSRAM */
        unsigned int sw_rotate: 1;   /*!< Use software rotation (slower), The feature is unavailable under avoid-tear mode */
    } flags;
} bsp_display_cfg_t;

/**
 * @brief Initialize display
 *
 * This function initializes MIPI-DSI, display controller and starts LVGL handling task.
 * LCD backlight must be enabled separately by calling bsp_display_brightness_set()
 *
 * @return Pointer to LVGL display or NULL when error occured
 */
lv_display_t *bsp_display_start(void);

/**
 * @brief Initialize display
 *
 * This function initializes MIPI-DSI, display controller and starts LVGL handling task.
 * LCD backlight must be enabled separately by calling bsp_display_brightness_set()
 *
 * @param cfg display configuration
 *
 * @return Pointer to LVGL display or NULL when error occured
 */
lv_display_t *bsp_display_start_with_config(const bsp_display_cfg_t *cfg);

/**
 * @brief Deinitialize display
 *
 * This function deinitializes MIPI-DSI, display controller and stops LVGL.
 *
 * @param display Pointer to LVGL display
 */
void bsp_display_stop(lv_display_t *display);

/**
 * @brief Get pointer to input device (touch, buttons, ...)
 *
 * @note The LVGL input device is initialized in bsp_display_start() function.
 *
 * @return Pointer to LVGL input device or NULL when not initialized
 */
lv_indev_t *bsp_display_get_input_dev(void);

/**
 * @brief Take LVGL mutex
 *
 * @param timeout_ms Timeout in [ms]. 0 will block indefinitely.
 * @return true  Mutex was taken
 * @return false Mutex was NOT taken
 */
bool bsp_display_lock(uint32_t timeout_ms);

/**
 * @brief Give LVGL mutex
 *
 */
void bsp_display_unlock(void);

/**
 * @brief Rotate screen
 *
 * Display must be already initialized by calling bsp_display_start()
 *
 * @param[in] disp Pointer to LVGL display
 * @param[in] rotation Angle of the display rotation
 */
void bsp_display_rotate(lv_display_t *disp, lv_disp_rotation_t rotation);
#ifdef __cplusplus
}
#endif
#endif
