#include "who_frame_lcd_disp.hpp"
#include "esp_log.h"
#include "esp_heap_caps.h"

static const char *TAG = "WhoFrameLCDDisp";

using namespace who::lcd;

namespace who {
namespace lcd_disp {
WhoFrameLCDDisp::WhoFrameLCDDisp(const std::string &name, frame_cap::WhoFrameCapNode *frame_cap_node, int peek_index) :
    task::WhoTask(name), m_lcd(new lcd::WhoLCD()), m_frame_cap_node(frame_cap_node), m_peek_index(peek_index)
{
    frame_cap_node->add_new_frame_signal_subscriber(this);
    const int width = frame_cap_node->get_fb_width();
    const int height = frame_cap_node->get_fb_height();
    m_canvas_buffer_size = static_cast<size_t>(width) * height * sizeof(uint16_t);
    m_canvas_buffer = heap_caps_calloc(1, m_canvas_buffer_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (m_canvas_buffer == nullptr) {
        ESP_LOGE(TAG, "Failed to allocate canvas buffer (%u bytes)",
                 static_cast<unsigned>(m_canvas_buffer_size));
        ESP_ERROR_CHECK(ESP_ERR_NO_MEM);
    }

    bsp_display_lock(0);
    m_canvas = lv_canvas_create(lv_scr_act());
    assert(m_canvas);
    lv_canvas_set_buffer(m_canvas, m_canvas_buffer, width, height, LV_COLOR_FORMAT_NATIVE);
    bsp_display_unlock();
}

WhoFrameLCDDisp::~WhoFrameLCDDisp()
{
    bsp_display_lock(0);
    lv_obj_del(m_canvas);
    bsp_display_unlock();
    heap_caps_free(m_canvas_buffer);
    m_canvas_buffer = nullptr;
    delete m_lcd;
}

void WhoFrameLCDDisp::set_lcd_disp_cb(const std::function<void(who::cam::cam_fb_t *)> &lcd_disp_cb)
{
    m_lcd_disp_cb = lcd_disp_cb;
}


lv_obj_t *WhoFrameLCDDisp::get_canvas()
{
    return m_canvas;
}


void WhoFrameLCDDisp::task()
{
    while (true) {
        EventBits_t event_bits =
            xEventGroupWaitBits(m_event_group, NEW_FRAME | TASK_PAUSE | TASK_STOP, pdTRUE, pdFALSE, portMAX_DELAY);
        if (event_bits & TASK_STOP) {
            break;
        } else if (event_bits & TASK_PAUSE) {
            xEventGroupSetBits(m_event_group, TASK_PAUSED);
            EventBits_t pause_event_bits =
                xEventGroupWaitBits(m_event_group, TASK_RESUME | TASK_STOP, pdTRUE, pdFALSE, portMAX_DELAY);
            if (pause_event_bits & TASK_STOP) {
                break;
            } else {
                continue;
            }
        }
        bsp_display_lock(0);
        who::cam::cam_fb_t frame = {};
        // The producer can retire its frame descriptor on the next capture. Copy both
        // descriptor and pixels under its lock, keeping LVGL bound to owned storage.
        if (!m_frame_cap_node->cam_fb_snapshot(frame, m_peek_index, m_canvas_buffer, m_canvas_buffer_size) ||
            frame.format != who::cam::cam_fb_fmt_t::CAM_FB_FMT_RGB565 ||
            frame.width != m_frame_cap_node->get_fb_width() ||
            frame.height != m_frame_cap_node->get_fb_height() || frame.len != m_canvas_buffer_size) {
            bsp_display_unlock();
            continue;
        }
        if (m_lcd_disp_cb) {
            m_lcd_disp_cb(&frame);
        }
        lv_obj_invalidate(m_canvas);
        bsp_display_unlock();

        // vTaskDelay(1);
    }
    xEventGroupSetBits(m_event_group, TASK_STOPPED);
    vTaskDelete(NULL);
}
} // namespace lcd_disp
} // namespace who
