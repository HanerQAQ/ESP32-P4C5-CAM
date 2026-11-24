#include "who_apriltag_app_lcd.hpp"
#include "who_lvgl_utils.hpp"
#include "who_yield2idle.hpp"
#include <algorithm>

static const char *TAG = "who_apriltag_lcd";

namespace who {
namespace app {
WhoApriltagAppLCD::WhoApriltagAppLCD(frame_cap::WhoFrameCap *frame_cap,
                                    frame_cap::WhoFrameCapNode *lcd_disp_frame_cap_node) :
    WhoApriltagAppTerm(frame_cap),
    m_res_mutex(xSemaphoreCreateMutex()),
    m_has_new_result(false),
    m_result(),
    m_disp_result()
{
    if (!lcd_disp_frame_cap_node) {
        lcd_disp_frame_cap_node = frame_cap->get_last_node();
    }
    m_lcd_disp = new lcd_disp::WhoFrameLCDDisp("LCDDisp", lcd_disp_frame_cap_node);
    WhoApp::add_task(m_lcd_disp);

    m_lcd_disp->set_lcd_disp_cb(std::bind(&WhoApriltagAppLCD::lcd_disp_cb, this, std::placeholders::_1));
    m_apriltag->set_apriltag_detect_result_cb(
        std::bind(&WhoApriltagAppLCD::apriltag_detect_result_cb, this, std::placeholders::_1));
    m_apriltag->set_cleanup_func(std::bind(&WhoApriltagAppLCD::cleanup, this));
}

WhoApriltagAppLCD::~WhoApriltagAppLCD()
{
    // delete m_result_lcd_disp;
    // bsp_display_lock(0);
    // lv_obj_del(m_label);
    // bsp_display_unlock();
    vSemaphoreDelete(m_res_mutex);
}

bool WhoApriltagAppLCD::run()
{
    bool ret = WhoYield2Idle::get_instance()->run();
    for (const auto &frame_cap_node : m_frame_cap->get_all_nodes()) {
        ret &= frame_cap_node->run(4096, m_frame_cap_task_priority, 0);
    }
    ret &= m_lcd_disp->run(4096, m_lcd_task_priority, 0);
    ret &= m_apriltag->run(25600, m_apriltag_task_priority, 1);
    return ret;
}

void WhoApriltagAppLCD::set_task_priorities(UBaseType_t frame_cap_priority,
                                             UBaseType_t apriltag_priority,
                                             UBaseType_t lcd_priority)
{
    m_frame_cap_task_priority = frame_cap_priority;
    m_apriltag_task_priority = apriltag_priority;
    m_lcd_task_priority = lcd_priority;
}

void WhoApriltagAppLCD::apriltag_detect_result_cb(const apriltag::WhoApriltag::result_t &result)
{
    xSemaphoreTake(m_res_mutex, portMAX_DELAY);
    m_result = result;
    m_has_new_result = true;
    xSemaphoreGive(m_res_mutex);
}

void WhoApriltagAppLCD::lcd_disp_cb(who::cam::cam_fb_t *fb)
{
    // m_result_lcd_disp->lcd_disp_cb(fb);
    xSemaphoreTake(m_res_mutex, portMAX_DELAY);
    if (m_has_new_result) {
        m_disp_result = m_result;
        m_result = {};
        m_has_new_result = false;
    }
    xSemaphoreGive(m_res_mutex);

    draw_apriltag_result_on_canvas(m_disp_result);
}

void WhoApriltagAppLCD::cleanup()
{
    // m_result_lcd_disp->cleanup();
    xSemaphoreTake(m_res_mutex, portMAX_DELAY);
    m_result = {};
    m_disp_result = {};
    m_has_new_result = false;
    xSemaphoreGive(m_res_mutex);
}

void WhoApriltagAppLCD::draw_apriltag_result_on_canvas(const apriltag::WhoApriltag::result_t &result)
{
    lv_obj_t *canvas = m_lcd_disp->get_canvas();

    if (result.tags.empty()) {
 
        return;
    }

    lv_draw_rect_dsc_t rect_dsc;
    lv_draw_rect_dsc_init(&rect_dsc);
    rect_dsc.bg_opa = LV_OPA_TRANSP;
    rect_dsc.border_width = 3;
    rect_dsc.border_color = lv_color_hex(0x00ff00);

    lv_draw_label_dsc_t label_dsc;
    lv_draw_label_dsc_init(&label_dsc);
    label_dsc.color = lv_color_hex(0x0000ff);
    label_dsc.font = &lv_font_montserrat_14;

    lv_layer_t layer;
    lv_canvas_init_layer(canvas, &layer);

    for (const auto &tag : result.tags) {   
        lv_area_t coords_rect = {tag.x1, tag.y1, tag.x2, tag.y2};
        lv_draw_rect(&layer, &rect_dsc, &coords_rect);

        char id_text[16];
        snprintf(id_text, sizeof(id_text), "%d", tag.id);
        lv_point_t char_pos = {tag.x1 + 4, tag.y1 + 4};
        const char *p = id_text;
        while (*p) {
            uint32_t letter = *p;
            uint32_t letter_next = *(p + 1);
            lv_draw_character(&layer, &label_dsc, &char_pos, letter);
            char_pos.x += lv_font_get_glyph_width(label_dsc.font, letter, letter_next);
            p++;
        }
    }

    lv_canvas_finish_layer(canvas, &layer);
}
} // namespace app
} // namespace who
