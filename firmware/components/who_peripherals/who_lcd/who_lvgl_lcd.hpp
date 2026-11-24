#pragma once

#include "p4c5_cam_board.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_types.h"

#if !BSP_CONFIG_NO_GRAPHIC_LIB
namespace who {
namespace lcd {
class WhoLCD {
public:
    
    WhoLCD(const lvgl_port_cfg_t &lvgl_port_cfg = {4, 8192, 0, 500, MALLOC_CAP_INTERNAL, 5}) { init(lvgl_port_cfg); }
    ~WhoLCD() { deinit(); }
    void init(const lvgl_port_cfg_t &lvgl_port_cfg);
    void deinit();
    uint16_t *get_transpose_buffer() { return transpose_buffer; }
    esp_lcd_panel_handle_t get_panel() { return m_panel; }
    void set_pending_flushes(int pending_flushes) { m_pending_flushes = pending_flushes; }
    bool notify_flush_done()
    {
        if (m_pending_flushes <= 0) {
            return false;
        }
        int pending_flushes = m_pending_flushes - 1;
        m_pending_flushes = pending_flushes;
        return pending_flushes == 0;
    }
    
private:
    uint16_t * transpose_buffer; // 用于存储转置后的像素数据
    esp_lcd_panel_handle_t m_panel;
    esp_lcd_panel_io_handle_t m_io;
    lv_display_t *m_disp;
    volatile int m_pending_flushes;
};
} // namespace lcd
} // namespace who

#endif
