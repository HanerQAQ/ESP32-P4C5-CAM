#include "driver/ledc.h"
#include "driver/uart.h"
#include "esp_err.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"
#include "p4c5_cam_board.h"

static const char *TAG = "p4c5_cam_board";

#if (BSP_CONFIG_NO_GRAPHIC_LIB == 0)
static lv_indev_t *disp_indev = NULL;
#endif // (BSP_CONFIG_NO_GRAPHIC_LIB == 0)


static bool i2c_initialized = false;
static bool s_voice_uart_initialized = false;
// static i2c_master_bus_handle_t i2c_handle = NULL;  // I2C Handle

static bsp_lcd_handles_t disp_handles;
static esp_lcd_panel_io_handle_t tp_io_handle = NULL;

static esp_err_t bsp_display_clear_and_disable(esp_lcd_panel_handle_t panel)
{
    const size_t line_buffer_size = BSP_LCD_V_RES * sizeof(uint16_t);
    uint16_t *black_line =
        heap_caps_calloc(1, line_buffer_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (black_line == NULL) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = ESP_OK;
    for (int y = 0; y < BSP_LCD_H_RES; ++y) {
        err = esp_lcd_panel_draw_bitmap(panel, 0, y, BSP_LCD_V_RES, y + 1, black_line);
        if (err != ESP_OK) {
            break;
        }
    }
    /*
     * Panel commands are sent only after queued SPI color transactions
     * finish, so the PSRAM buffer can be released after this returns.
     */
    esp_err_t disable_err = esp_lcd_panel_disp_on_off(panel, false);
    if (err == ESP_OK) {
        err = disable_err;
    }
    heap_caps_free(black_line);
    return err;
}

esp_err_t bsp_buttons_init(void)
{
    const gpio_config_t button_config = {
        .pin_bit_mask = (1ULL << BSP_BUTTON_A) | (1ULL << BSP_BUTTON_B),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    ESP_RETURN_ON_ERROR(gpio_config(&button_config), TAG, "configure buttons");
    ESP_LOGI(TAG, "Buttons initialized");
    return ESP_OK;
}

esp_err_t bsp_i2c_slave_init(const i2c_slave_config_t *config,
                             const i2c_slave_event_callbacks_t *callbacks,
                             void *user_data,
                             i2c_slave_dev_handle_t *handle)
{
    ESP_RETURN_ON_FALSE(config && callbacks && handle, ESP_ERR_INVALID_ARG, TAG, "invalid I2C slave arguments");
    ESP_RETURN_ON_FALSE(!i2c_initialized, ESP_ERR_INVALID_STATE, TAG, "I2C already initialized");
    ESP_RETURN_ON_ERROR(i2c_new_slave_device(config, handle), TAG, "create I2C slave failed");

    esp_err_t err = gpio_set_drive_capability(config->scl_io_num, GPIO_DRIVE_CAP_3);
    if (err == ESP_OK) {
        err = gpio_set_drive_capability(config->sda_io_num, GPIO_DRIVE_CAP_3);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "set I2C GPIO drive capability failed: %s", esp_err_to_name(err));
        i2c_del_slave_device(*handle);
        *handle = NULL;
        return err;
    }

    err = i2c_slave_register_event_callbacks(*handle, callbacks, user_data);
    if (err != ESP_OK) {
        i2c_del_slave_device(*handle);
        *handle = NULL;
        return err;
    }
    i2c_initialized = true;
    return ESP_OK;
}

esp_err_t bsp_i2c_slave_deinit(i2c_slave_dev_handle_t handle)
{
    if (!i2c_initialized) {
        return ESP_OK;
    }
    ESP_RETURN_ON_FALSE(handle, ESP_ERR_INVALID_ARG, TAG, "invalid I2C slave handle");
    ESP_RETURN_ON_ERROR(i2c_del_slave_device(handle), TAG, "delete I2C slave failed");
    i2c_initialized = false;
    return ESP_OK;
}

esp_err_t bsp_ci1302_uart_init(void)
{
    if (s_voice_uart_initialized) {
        return ESP_OK;
    }

    gpio_config_t voice_config = {
        .pin_bit_mask = (1ULL << BSP_VOICE_POWER) | (1ULL << BSP_VOICE_ENABLE),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    ESP_LOGI(TAG,
             "init voice UART%d: tx=%d rx=%d baud=%d",
             BSP_VOICE_UART_NUM,
             BSP_VOICE_UART_TX,
             BSP_VOICE_UART_RX,
             BSP_VOICE_UART_BAUD);

    uart_config_t uart_config = {
        .baud_rate = BSP_VOICE_UART_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .rx_flow_ctrl_thresh = 0,
        .source_clk = UART_SCLK_DEFAULT,
    };

    ESP_RETURN_ON_ERROR(uart_driver_install(BSP_VOICE_UART_NUM, 256, 256, 0, NULL, 0),
                        TAG,
                        "install voice UART failed");
    esp_err_t err = uart_param_config(BSP_VOICE_UART_NUM, &uart_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "configure voice UART failed: %s", esp_err_to_name(err));
    }
    if (err == ESP_OK) {
        err = uart_set_pin(BSP_VOICE_UART_NUM,
                           BSP_VOICE_UART_TX,
                           BSP_VOICE_UART_RX,
                           UART_PIN_NO_CHANGE,
                           UART_PIN_NO_CHANGE);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "set voice UART pins failed: %s", esp_err_to_name(err));
        }
    }
    if (err == ESP_OK) {
        err = gpio_set_drive_capability(BSP_VOICE_POWER, GPIO_DRIVE_CAP_3);
        if (err == ESP_OK) {
            err = gpio_set_drive_capability(BSP_VOICE_ENABLE, GPIO_DRIVE_CAP_3);
        }
        gpio_set_drive_capability(BSP_VOICE_UART_TX, GPIO_DRIVE_CAP_3);

        if (err != ESP_OK) {
            ESP_LOGE(TAG, "set voice UART GPIO drive capability failed: %s", esp_err_to_name(err));
        }
    }
    if (err != ESP_OK) {
        uart_driver_delete(BSP_VOICE_UART_NUM);
        return err;
    }
    ESP_RETURN_ON_ERROR(gpio_config(&voice_config), TAG, "configure voice GPIO failed");
    ESP_RETURN_ON_ERROR(gpio_set_level(BSP_VOICE_POWER, 1), TAG, "power voice board failed");
    ESP_RETURN_ON_ERROR(gpio_set_level(BSP_VOICE_ENABLE, 1), TAG, "enable voice board failed");

    ESP_LOGI(TAG, "CI1302 UART initialized");
    s_voice_uart_initialized = true;
    return ESP_OK;
}

esp_err_t bsp_ci1302_uart_deinit(void)
{
    if (!s_voice_uart_initialized) {
        return ESP_OK;
    }
    ESP_RETURN_ON_ERROR(uart_wait_tx_done(BSP_VOICE_UART_NUM, pdMS_TO_TICKS(100)), TAG, "wait voice UART failed");
    ESP_RETURN_ON_ERROR(uart_driver_delete(BSP_VOICE_UART_NUM), TAG, "delete voice UART failed");
    s_voice_uart_initialized = false;
    return ESP_OK;
}

esp_err_t bsp_display_brightness_init(void)
{
    // Setup LEDC peripheral for PWM backlight control
    const ledc_channel_config_t LCD_backlight_channel = {
        .gpio_num = BSP_LCD_BACKLIGHT,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LCD_LEDC_CH,
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = 1,
        .duty = 0,
        .hpoint = 0
    };

    const ledc_timer_config_t LCD_backlight_timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_10_BIT,
        .timer_num = 1,
        .freq_hz = 5000,
        .clk_cfg = LEDC_AUTO_CLK
    };

    ESP_ERROR_CHECK(ledc_timer_config(&LCD_backlight_timer));
    ESP_ERROR_CHECK(ledc_channel_config(&LCD_backlight_channel));
    return ESP_OK;
}

esp_err_t bsp_display_brightness_deinit(void)
{
    const ledc_timer_config_t LCD_backlight_timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .timer_num = 1,
        .deconfigure = 1
    };
    ESP_ERROR_CHECK(ledc_timer_pause(LEDC_LOW_SPEED_MODE, 1));
    ESP_ERROR_CHECK(ledc_timer_config(&LCD_backlight_timer));
    return ESP_OK;
}


esp_err_t bsp_display_brightness_set(int brightness_percent)
{
    if (brightness_percent > 100) {
        brightness_percent = 100;
    }
    if (brightness_percent < 0) {
        brightness_percent = 0;
    }

    ESP_LOGI(TAG, "Setting LCD backlight: %d%%", brightness_percent);
    uint32_t duty_cycle = (1023 * brightness_percent) / 100; // LEDC resolution set to 10bits, thus: 100% = 1023
    ESP_ERROR_CHECK(ledc_set_duty(LEDC_LOW_SPEED_MODE, LCD_LEDC_CH, duty_cycle));
    ESP_ERROR_CHECK(ledc_update_duty(LEDC_LOW_SPEED_MODE, LCD_LEDC_CH));
    return ESP_OK;
}


esp_err_t bsp_display_backlight_on(void)
{
    return bsp_display_brightness_set(100);
}


esp_err_t bsp_display_backlight_off(void)
{
    return bsp_display_brightness_set(0);
}


esp_err_t bsp_display_new(esp_lcd_panel_handle_t *ret_panel, esp_lcd_panel_io_handle_t *ret_io)
{
    esp_err_t ret = ESP_OK;
    bsp_lcd_handles_t handles;
    ret = bsp_display_new_with_handles(&handles);

    *ret_panel = handles.panel;
    *ret_io = handles.io;

    return ret;
}

esp_err_t bsp_display_new_with_handles(bsp_lcd_handles_t *ret_handles)
{
    esp_err_t ret = ESP_OK;
    esp_lcd_panel_io_handle_t io = NULL;
    esp_lcd_panel_handle_t disp_panel = NULL;

    bsp_display_brightness_init();
    
    spi_bus_config_t buscfg = {.mosi_io_num = BSP_LCD_MOSI_GPIO,
                               .miso_io_num = GPIO_NUM_NC,
                               .sclk_io_num = BSP_LCD_CLK_GPIO,
                               .quadwp_io_num = GPIO_NUM_NC,
                               .quadhd_io_num = GPIO_NUM_NC,
                               .max_transfer_sz = BSP_LCD_H_RES * BSP_LCD_V_RES * (BSP_LCD_BITS_PER_PIXEL / 8)};
    ESP_ERROR_CHECK(spi_bus_initialize(BSP_LCD_SPI_PORT, &buscfg, SPI_DMA_CH_AUTO));
    disp_handles.spi_host = BSP_LCD_SPI_PORT;

    esp_lcd_panel_io_spi_config_t io_config = {.cs_gpio_num = BSP_LCD_CS_GPIO,
                                               .dc_gpio_num = BSP_LCD_DC_GPIO,
                                               .spi_mode = 0,
                                               .pclk_hz = 80 * 1000 * 1000,
                                               .trans_queue_depth = BSP_LCD_SPI_TRANS_QUEUE_DEPTH,
                                               .lcd_cmd_bits = 8,
                                               .lcd_param_bits = 8};

    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(disp_handles.spi_host, &io_config, &io));

    esp_lcd_panel_dev_config_t panel_config = {.reset_gpio_num = BSP_LCD_RST_GPIO,
                                               .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
                                               .data_endian = LCD_RGB_DATA_ENDIAN_LITTLE,
                                               .bits_per_pixel = BSP_LCD_BITS_PER_PIXEL};

    ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(io, &panel_config, &disp_panel));
    esp_lcd_panel_reset(disp_panel);
    esp_lcd_panel_init(disp_panel);

    esp_lcd_panel_invert_color(disp_panel, true);
    esp_lcd_panel_swap_xy(disp_panel, false);
    esp_lcd_panel_mirror(disp_panel, false, false);
    ESP_ERROR_CHECK(bsp_display_clear_and_disable(disp_panel));
    bsp_display_backlight_on();
    
    /* Return all handles */
    ret_handles->io = io;
    disp_handles.io = io;

    ret_handles->panel = disp_panel;
    ret_handles->control = NULL;

    disp_handles.panel = disp_panel;
    disp_handles.control = NULL;

    return ret;
}

void bsp_display_delete(void)
{
    if (disp_handles.panel) {
        esp_lcd_panel_del(disp_handles.panel);
        disp_handles.panel = NULL;
    }
    if (disp_handles.io) {
        esp_lcd_panel_io_del(disp_handles.io);
        disp_handles.io = NULL;
    }
    ESP_ERROR_CHECK(spi_bus_free(disp_handles.spi_host));
    bsp_display_brightness_deinit();

}

#if (BSP_CONFIG_NO_GRAPHIC_LIB == 0)
static lv_display_t *bsp_display_lcd_init(const bsp_display_cfg_t *cfg)
{
    assert(cfg != NULL);
    bsp_display_new_with_handles(&disp_handles);

    uint32_t display_hres = 0;
    uint32_t display_vres = 0;

    display_hres = BSP_LCD_H_RES;
    display_vres = BSP_LCD_V_RES;

    // display_hres = BSP_LCD_V_RES; //交换了xy
    // display_vres = BSP_LCD_H_RES;

    ESP_LOGI(TAG, "Display resolution %ldx%ld", display_hres, display_vres);

    /* Add LCD screen */
    ESP_LOGD(TAG, "Add LCD screen");
    const lvgl_port_display_cfg_t disp_cfg = {
        .io_handle = disp_handles.io,
        .panel_handle = disp_handles.panel,
        .control_handle = disp_handles.control,
        .buffer_size = cfg->buffer_size,
        .double_buffer = cfg->double_buffer,
        .hres = display_hres,
        .vres = display_vres,
        .monochrome = false,
        /* Rotation values must be same as used in esp_lcd for initial settings of the screen */
        .rotation = {
            .swap_xy = false,
            .mirror_x = false,
            .mirror_y = false,
        },
        .color_format = LV_COLOR_FORMAT_RGB565,
        .flags = {
            .buff_dma = cfg->flags.buff_dma,
            .buff_spiram = cfg->flags.buff_spiram,
#if LVGL_VERSION_MAJOR >= 9
            .swap_bytes = false,
#endif
            .sw_rotate = cfg->flags.sw_rotate, /* Only SW rotation is supported for 90° and 270° */
// #if CONFIG_BSP_DISPLAY_LVGL_FULL_REFRESH
//             .full_refresh = true,
// #elif CONFIG_BSP_DISPLAY_LVGL_DIRECT_MODE
//             .direct_mode = true,
// #endif
        }
    };

    return lvgl_port_add_disp(&disp_cfg);
}

lv_display_t *bsp_display_start(void)
{
    bsp_display_cfg_t cfg = {
        .lvgl_port_cfg = ESP_LVGL_PORT_INIT_CONFIG(),
        .buffer_size = BSP_LCD_DRAW_BUFF_SIZE,
        .double_buffer = BSP_LCD_DRAW_BUFF_DOUBLE,
        .flags = {
            .buff_dma = true,
            .buff_spiram = false,
            .sw_rotate = true,
        }
    };
    return bsp_display_start_with_config(&cfg);
}

lv_display_t *bsp_display_start_with_config(const bsp_display_cfg_t *cfg)
{
    lv_display_t *disp;

    assert(cfg != NULL);
    ESP_ERROR_CHECK(lvgl_port_init(&cfg->lvgl_port_cfg));
    disp = bsp_display_lcd_init(cfg);
    return disp;
}

void bsp_display_stop(lv_display_t *display)
{
    /* Deinit LVGL */
    lvgl_port_remove_disp(display);
    lvgl_port_deinit();

    /* Deinit display */
    bsp_display_delete();
}

void bsp_display_rotate(lv_display_t *disp, lv_disp_rotation_t rotation)
{
    lv_disp_set_rotation(disp, rotation);
}

bool bsp_display_lock(uint32_t timeout_ms)
{
    return lvgl_port_lock(timeout_ms);
}

void bsp_display_unlock(void)
{
    lvgl_port_unlock();
}

esp_lcd_panel_handle_t bsp_display_get_panel(void)
{
    return disp_handles.panel;
}

esp_lcd_panel_io_handle_t bsp_display_get_io(void)
{
    return disp_handles.io;
}

#endif // (BSP_CONFIG_NO_GRAPHIC_LIB == 0)
