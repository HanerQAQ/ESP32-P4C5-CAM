#pragma once
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_st7789.h"
#include "esp_lcd_panel_vendor.h"
#include <driver/spi_common.h>
#include <string.h>

#include "who_lvgl_lcd.hpp"

#if BSP_CONFIG_NO_GRAPHIC_LIB

namespace who {
namespace lcd {
class WhoLCD {
public:
    WhoLCD() { init(); }
    ~WhoLCD() { deinit(); }
    void init();
    void deinit();
    esp_lcd_panel_handle_t get_lcd_panel_handle();
    void draw_bitmap(const uint16_t *data, int width, int height, int x_start, int y_start);

private:
    esp_lcd_panel_handle_t m_panel_handle;
    esp_lcd_panel_io_handle_t m_io_handle;
    uint16_t *transpose_buffer;

};
} // namespace lcd
} // namespace who

#endif

