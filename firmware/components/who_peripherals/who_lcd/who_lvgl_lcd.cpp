#include "who_lvgl_lcd.hpp"
#include "esp_attr.h"
#include "esp_err.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "who_lvgl_lcd";

#if !BSP_CONFIG_NO_GRAPHIC_LIB
/* Render the LCD in 80-line strips. The smaller internal DMA staging buffer
 * leaves headroom for the ESP32-C5 ESP-Hosted SDIO receive path. */
static constexpr int TRANSPOSE_LINES = 80;
static constexpr int TRANSPOSE_BUFFER_PIXELS = BSP_LCD_H_RES * TRANSPOSE_LINES;
static DMA_ATTR uint16_t s_transpose_buffer[TRANSPOSE_BUFFER_PIXELS];

static bool who_lvgl_flush_ready_cb(esp_lcd_panel_io_handle_t panel_io,
                                    esp_lcd_panel_io_event_data_t *edata,
                                    void *user_ctx)
{
    lv_display_t *disp = static_cast<lv_display_t *>(user_ctx);
    who::lcd::WhoLCD *lcd = static_cast<who::lcd::WhoLCD *>(lv_display_get_user_data(disp));
    if (lcd != NULL && lcd->notify_flush_done()) {
        lv_display_flush_ready(disp);
    }
    return false;
}

static void who_lvgl_flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    who::lcd::WhoLCD *lcd = static_cast<who::lcd::WhoLCD *>(lv_display_get_user_data(disp));
    if (lcd == NULL) {
        ESP_LOGE(TAG, "LCD is not initialized");
        lv_display_flush_ready(disp);
        return;
    }

    esp_lcd_panel_handle_t panel = lcd->get_panel();
    if (panel == NULL) {
        ESP_LOGE(TAG, "LCD panel is not initialized");
        lv_display_flush_ready(disp);
        return;
    }

    uint16_t *transpose_buffer = lcd->get_transpose_buffer();
    if (transpose_buffer == NULL) {
        ESP_LOGE(TAG, "Transpose buffer is not initialized");
        lv_display_flush_ready(disp);
        return;
    }

    uint16_t *px_map_u16 = reinterpret_cast<uint16_t *>(px_map);
    int width = area->x2 - area->x1 + 1;
    int height = area->y2 - area->y1 + 1;
    if (width * height > TRANSPOSE_BUFFER_PIXELS) {
        ESP_LOGE(TAG, "Flush area %dx%d exceeds the LCD staging buffer", width, height);
        lv_display_flush_ready(disp);
        return;
    }

    int x_start = BSP_LCD_V_RES - area->y2 - 1;
    int x_end = BSP_LCD_V_RES - area->y1;
    int y_start = area->x1;
    int y_end = area->x2 + 1;

    for (int src_x = 0; src_x < width; src_x++) {
        uint16_t *dst = transpose_buffer + src_x * height + height - 1;
        const uint16_t *src = px_map_u16 + src_x;
        for (int src_y = 0; src_y < height; src_y++) {
            *dst-- = *src;
            src += width;
        }
    }

    lcd->set_pending_flushes(1);
    esp_err_t ret = esp_lcd_panel_draw_bitmap(panel, x_start, y_start, x_end, y_end, transpose_buffer);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to flush LVGL buffer to LCD: %s", esp_err_to_name(ret));
        lcd->set_pending_flushes(0);
        lv_display_flush_ready(disp);
    }
}

namespace who {
namespace lcd {
void WhoLCD::init(const lvgl_port_cfg_t &lvgl_port_cfg)
{
    m_disp = NULL;
    m_panel = NULL;
    m_io = NULL;
    m_pending_flushes = 0;

    transpose_buffer = s_transpose_buffer;

    bsp_display_cfg_t cfg = {.lvgl_port_cfg = lvgl_port_cfg,
                             .buffer_size = TRANSPOSE_BUFFER_PIXELS,
                             .double_buffer = false,
                             .flags = {
                                 .buff_dma = true,
                                 .buff_spiram = true,
                                 .sw_rotate = false,
                             }};

    m_disp = bsp_display_start_with_config(&cfg);
    m_panel = bsp_display_get_panel();
    m_io = bsp_display_get_io();
    if (m_disp != NULL) {
        lv_display_set_user_data(m_disp, this);
        lv_display_set_flush_cb(m_disp, who_lvgl_flush_cb);
        if (m_io != NULL) {
            const esp_lcd_panel_io_callbacks_t cbs = {
                .on_color_trans_done = who_lvgl_flush_ready_cb,
            };
            ESP_ERROR_CHECK(esp_lcd_panel_io_register_event_callbacks(m_io, &cbs, m_disp));
        }

        // Let the LVGL port task clear the screen before enabling ST7789
        // scan-out. Avoid lv_refr_now() here because the port owns refreshes.
        bsp_display_lock(0);
        lv_obj_t *screen = lv_display_get_screen_active(m_disp);
        lv_obj_set_style_bg_color(screen, lv_color_black(), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_invalidate(screen);
        bsp_display_unlock();
        ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(m_panel, true));
    }
}

void WhoLCD::deinit()
{
    if (m_disp) {
        bsp_display_stop(m_disp);
    }
    transpose_buffer = NULL;
    m_panel = NULL;
    m_io = NULL;
    m_disp = NULL;
    m_pending_flushes = 0;
}

} // namespace lcd
} // namespace who
#endif
