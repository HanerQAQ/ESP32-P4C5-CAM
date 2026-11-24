#include "hardware_test.h"
// #include "detection_service.h"
#include <stdio.h>
#include <inttypes.h>
#include <string.h>
#include "sdkconfig.h"
#include "driver/gpio.h"
#include "driver/i2c_slave.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* States: 0 waiting, 1 passed, 2 unconfigured, 3 initialization error. */
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static int s_camera, s_boot = 2, s_i2c = 2;
static int s_boot_level = -1;
static uint32_t s_boot_polls, s_boot_presses;
static i2c_slave_dev_handle_t s_slave;
static const char s_expected[] = "ESP32P4C5-MINI";

static void set_state(int *state, int value)
{
    portENTER_CRITICAL(&s_lock);
    *state = value;
    portEXIT_CRITICAL(&s_lock);
}

void hardware_test_camera_pass(void)
{
    set_state(&s_camera, 1);
}

void hardware_test_camera_error(void)
{
    set_state(&s_camera, 3);
}

static bool receive(i2c_slave_dev_handle_t handle,
                    const i2c_slave_rx_done_event_data_t *event, void *context)
{
    (void)handle;
    (void)context;
    /* One raw I2C write transaction, no register prefix or terminator. */
    if (event && event->length == sizeof(s_expected) - 1 &&
        memcmp(event->buffer, s_expected, sizeof(s_expected) - 1) == 0) {
        portENTER_CRITICAL_ISR(&s_lock);
        s_i2c = 1;
        portEXIT_CRITICAL_ISR(&s_lock);
    }
    return false;
}

static void boot_task(void *arg)
{
    (void)arg;
    unsigned stable_samples = 0;
    int previous = 1;
    bool pressed = false;
    ESP_LOGI("hardware_test", "BOOT monitor started: P4 GPIO%d, active LOW", CONFIG_TEST_BOOT_GPIO);
    while (true) {
        int level = gpio_get_level(CONFIG_TEST_BOOT_GPIO);
        portENTER_CRITICAL(&s_lock);
        s_boot_level = level;
        ++s_boot_polls;
        portEXIT_CRITICAL(&s_lock);
        if (level != previous) {
            previous = level;
            stable_samples = 0;
        }
        if (stable_samples < 3) ++stable_samples;
        if (stable_samples == 3) {
            if (level == 0 && !pressed) {
                pressed = true;
                set_state(&s_boot, 1);
                portENTER_CRITICAL(&s_lock);
                ++s_boot_presses;
                portEXIT_CRITICAL(&s_lock);
                // detection_service_toggle_mode();
                ESP_LOGI("hardware_test", "BOOT pressed: video/AprilTag mode toggled");
            } else if (level == 1) {
                pressed = false;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

void hardware_test_init(void)
{
    const int scl = CONFIG_TEST_I2C_SCL_GPIO;
    const int sda = CONFIG_TEST_I2C_SDA_GPIO;
    const int boot = CONFIG_TEST_BOOT_GPIO;
    const int cam_scl = CONFIG_EXAMPLE_MIPI_CSI_SCCB_I2C_SCL_PIN;
    const int cam_sda = CONFIG_EXAMPLE_MIPI_CSI_SCCB_I2C_SDA_PIN;
    ESP_LOGI("hardware_test", "BOOT diagnostic v1: built %s %s; P4 GPIO%d; camera SCL/SDA=%d/%d; external=%d/%d",
             __DATE__, __TIME__, boot, cam_scl, cam_sda, scl, sda);

    if (boot >= 0) {
        esp_err_t err = ESP_ERR_INVALID_ARG;
        if (GPIO_IS_VALID_GPIO(boot) && boot != cam_scl && boot != cam_sda &&
            boot != scl && boot != sda) {
            gpio_config_t cfg = {
                .pin_bit_mask = 1ULL << boot, .mode = GPIO_MODE_INPUT,
                .pull_up_en = GPIO_PULLUP_ENABLE,
            };
            err = gpio_config(&cfg);
            if (err == ESP_OK) {
                set_state(&s_boot, 0);
                if (xTaskCreate(boot_task, "boot_test", 3072, NULL, 4, NULL) != pdPASS) {
                    err = ESP_ERR_NO_MEM;
                }
            }
        }
        if (err != ESP_OK) {
            set_state(&s_boot, 3);
            ESP_LOGE("hardware_test", "BOOT init: %s", esp_err_to_name(err));
        }
    }
    if (scl >= 0 && sda >= 0) {
        esp_err_t err = ESP_ERR_INVALID_ARG;
        if (GPIO_IS_VALID_OUTPUT_GPIO(scl) && GPIO_IS_VALID_OUTPUT_GPIO(sda) &&
            scl != sda && scl != cam_scl && scl != cam_sda &&
            sda != cam_scl && sda != cam_sda && boot != scl && boot != sda) {
            i2c_slave_config_t cfg = {
                .i2c_port = I2C_NUM_1,
                .sda_io_num = sda, .scl_io_num = scl,
                .clk_source = I2C_CLK_SRC_DEFAULT,
                .send_buf_depth = 256, .receive_buf_depth = 256,
                .slave_addr = 0x54, .addr_bit_len = I2C_ADDR_BIT_LEN_7,
                .flags.enable_internal_pullup = true,
            };
            err = i2c_new_slave_device(&cfg, &s_slave);
            if (err == ESP_OK) {
                const i2c_slave_event_callbacks_t callbacks = {.on_receive = receive};
                set_state(&s_i2c, 0);
                err = i2c_slave_register_event_callbacks(s_slave, &callbacks, NULL);
                if (err != ESP_OK) {
                    i2c_del_slave_device(s_slave);
                    s_slave = NULL;
                }
            }
        }
        if (err != ESP_OK) {
            set_state(&s_i2c, 3);
            ESP_LOGE("hardware_test", "External I2C init: %s", esp_err_to_name(err));
        }
    }
}

esp_err_t hardware_test_json(char *buffer, size_t size)
{
    int camera, boot, i2c, level;
    uint32_t polls, presses;
    portENTER_CRITICAL(&s_lock);
    camera = s_camera; boot = s_boot; i2c = s_i2c;
    level = s_boot_level; polls = s_boot_polls; presses = s_boot_presses;
    portEXIT_CRITICAL(&s_lock);
    int n = snprintf(buffer, size,
        "{\"camera\":%d,\"boot\":%d,\"i2c\":%d,\"all_passed\":%s,"
        "\"boot_gpio\":%d,\"boot_level\":%d,\"boot_polls\":%" PRIu32 ",\"boot_presses\":%" PRIu32 "}",
        camera, boot, i2c, camera == 1 && boot == 1 && i2c == 1 ? "true" : "false",
        CONFIG_TEST_BOOT_GPIO, level, polls, presses);
    return n >= 0 && (size_t)n < size ? ESP_OK : ESP_ERR_NO_MEM;
}
