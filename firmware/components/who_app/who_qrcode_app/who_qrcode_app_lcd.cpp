#include "who_qrcode_app_lcd.hpp"
#include "who_yield2idle.hpp"
#include <cassert>

LV_FONT_DECLARE(montserrat_bold_20);

namespace who {
namespace app {
namespace {
static bool is_earlier(const struct timeval &t1, const struct timeval &t2)
{
    if (t1.tv_sec == t2.tv_sec) {
        return t1.tv_usec < t2.tv_usec;
    }
    return t1.tv_sec < t2.tv_sec;
}

static void draw_text(lv_layer_t *layer, lv_draw_label_dsc_t *label_dsc, int x, int y, const char *text)
{
    lv_point_t char_pos = {x, y};
    const char *p = text;
    while (*p) {
        uint32_t letter = *p;
        uint32_t letter_next = *(p + 1);
        lv_draw_character(layer, label_dsc, &char_pos, letter);
        char_pos.x += lv_font_get_glyph_width(label_dsc->font, letter, letter_next);
        p++;
    }
}

static void draw_qrcode_result_on_canvas(lv_obj_t *canvas, const qrcode::WhoQRCode::result_t &result)
{
    if (result.codes.empty()) {
        return;
    }

    lv_draw_rect_dsc_t rect_dsc;
    lv_draw_rect_dsc_init(&rect_dsc);
    rect_dsc.bg_opa = LV_OPA_TRANSP;
    rect_dsc.border_width = 2;
    rect_dsc.border_color = lv_color_hex(0x00FF00);

    lv_draw_label_dsc_t label_dsc;
    lv_draw_label_dsc_init(&label_dsc);
    label_dsc.color = lv_color_hex(0xFF0000);
    label_dsc.font = &montserrat_bold_20;

    lv_layer_t layer;
    lv_canvas_init_layer(canvas, &layer);

    for (const auto &code : result.codes) {
        lv_area_t coords_rect = {
            static_cast<int32_t>(code.x),
            static_cast<int32_t>(code.y),
            static_cast<int32_t>(code.x + code.w),
            static_cast<int32_t>(code.y + code.h),
        };
        lv_draw_rect(&layer, &rect_dsc, &coords_rect);
        draw_text(&layer, &label_dsc, code.x + 5, code.y + 5, code.payload.c_str());
    }

    lv_canvas_finish_layer(canvas, &layer);
}
} // namespace

WhoQRCodeAppLCD::WhoQRCodeAppLCD(frame_cap::WhoFrameCap *frame_cap,
                                 frame_cap::WhoFrameCapNode *lcd_disp_frame_cap_node) :
    WhoQRCodeAppTerm(frame_cap),
    m_res_mutex(xSemaphoreCreateMutex())
{
    assert(m_res_mutex);
    if (!lcd_disp_frame_cap_node) {
        lcd_disp_frame_cap_node = frame_cap->get_last_node();
    }
    m_lcd_disp = new lcd_disp::WhoFrameLCDDisp("LCDDisp", lcd_disp_frame_cap_node);
    WhoApp::add_task(m_lcd_disp);
    m_lcd_disp->set_lcd_disp_cb(std::bind(&WhoQRCodeAppLCD::lcd_disp_cb, this, std::placeholders::_1));

    m_qrcode->set_qrcode_detect_result_cb(
        std::bind(&WhoQRCodeAppLCD::qrcode_detect_result_cb, this, std::placeholders::_1));
    m_qrcode->set_cleanup_func(std::bind(&WhoQRCodeAppLCD::cleanup, this));
}

WhoQRCodeAppLCD::~WhoQRCodeAppLCD()
{
    vSemaphoreDelete(m_res_mutex);
}

bool WhoQRCodeAppLCD::run()
{
    bool ret = WhoYield2Idle::get_instance()->run();
    for (const auto &frame_cap_node : m_frame_cap->get_all_nodes()) {
        ret &= frame_cap_node->run(4096, m_frame_cap_task_priority, 0);
    }
    ret &= m_lcd_disp->run(2560, m_lcd_task_priority, 0);
    ret &= m_qrcode->run(25600, m_qrcode_task_priority, 1);
    return ret;
}

void WhoQRCodeAppLCD::set_task_priorities(UBaseType_t frame_cap_priority,
                                           UBaseType_t qrcode_priority,
                                           UBaseType_t lcd_priority)
{
    m_frame_cap_task_priority = frame_cap_priority;
    m_qrcode_task_priority = qrcode_priority;
    m_lcd_task_priority = lcd_priority;
}

void WhoQRCodeAppLCD::qrcode_result_cb(const std::string &result)
{
    WhoQRCodeAppTerm::qrcode_result_cb(result);
}

void WhoQRCodeAppLCD::qrcode_detect_result_cb(const qrcode::WhoQRCode::result_t &result)
{
    xSemaphoreTake(m_res_mutex, portMAX_DELAY);
    m_results.push(result);
    xSemaphoreGive(m_res_mutex);
}

void WhoQRCodeAppLCD::lcd_disp_cb(who::cam::cam_fb_t *fb)
{
    xSemaphoreTake(m_res_mutex, portMAX_DELAY);
    while (!m_results.empty()) {
        qrcode::WhoQRCode::result_t result = m_results.front();
        if (!is_earlier(fb->timestamp, result.timestamp)) {
            m_disp_result = result;
            m_results.pop();
        } else {
            break;
        }
    }
    const auto disp_result = m_disp_result;
    xSemaphoreGive(m_res_mutex);

    draw_qrcode_result_on_canvas(m_lcd_disp->get_canvas(), disp_result);
}

void WhoQRCodeAppLCD::cleanup()
{
    xSemaphoreTake(m_res_mutex, portMAX_DELAY);
    std::queue<qrcode::WhoQRCode::result_t>().swap(m_results);
    m_disp_result = {};
    xSemaphoreGive(m_res_mutex);
}
} // namespace app
} // namespace who
