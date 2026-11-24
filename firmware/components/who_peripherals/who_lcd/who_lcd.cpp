#include "who_lcd.hpp"
#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "p4c5_cam_board.h"

static const char *TAG = "who_lcd";

#if BSP_CONFIG_NO_GRAPHIC_LIB

namespace who {
namespace lcd {
esp_lcd_panel_handle_t WhoLCD::get_lcd_panel_handle()
{
    return m_panel_handle; // user
}

void WhoLCD::init()
{
    //--------LCD Init-----------//
    // 配置背光控制引脚，默认拉低避免上电瞬间点亮s
    transpose_buffer = (uint16_t *)heap_caps_malloc(BSP_LCD_H_RES * BSP_LCD_V_RES * 2, MALLOC_CAP_SPIRAM);

    bsp_display_new(&m_panel_handle, &m_io_handle);
    // 填充屏幕为黑色
    memset(transpose_buffer, 0xFFFF, BSP_LCD_H_RES * BSP_LCD_V_RES * 2);
    esp_lcd_panel_draw_bitmap(m_panel_handle, 0, 0, BSP_LCD_V_RES, BSP_LCD_H_RES, transpose_buffer);
}

void WhoLCD::deinit()
{
    heap_caps_free(transpose_buffer);
}

void WhoLCD::draw_bitmap(const uint16_t *data, int width, int height, int x_start, int y_start)
{
    // for (int x = 0; x < width; x++) {
    //     for (int y = 0; y < height; y++) {
    //         // 180度旋转：读取对角位置的像素
    //         int src_x = width - 1 - x;
    //         int src_y = height - 1 - y;
    //         transpose_buffer[x * height + y] = data[src_y * width + src_x];
    //     }
    // }       

    // esp_lcd_panel_draw_bitmap(m_panel_handle, x_start, y_start, height, width, transpose_buffer);
    esp_lcd_panel_draw_bitmap(m_panel_handle, x_start, y_start, height, width, data);
}

} // namespace lcd
} // namespace who

#endif
