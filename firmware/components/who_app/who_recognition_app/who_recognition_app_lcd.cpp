#include "who_recognition_app_lcd.hpp"
#include "hand_detect.hpp"
#include "hand_gesture_recognition.hpp"
#include "who_lvgl_utils.hpp"
#include "who_yield2idle.hpp"
#include <cassert>
LV_FONT_DECLARE(montserrat_bold_26);

namespace who {
namespace app {
WhoRecognitionAppLCD::WhoRecognitionAppLCD(frame_cap::WhoFrameCap *frame_cap, 
                                           frame_cap::WhoFrameCapNode *lcd_disp_frame_cap_node) :
    WhoRecognitionAppBase(frame_cap)
{
#if defined(CONFIG_HAND_GESTURE_CLS_MODEL_LOCATION)
    m_result_mutex = xSemaphoreCreateMutex();
    assert(m_result_mutex);
#endif
    if (!lcd_disp_frame_cap_node) {
        lcd_disp_frame_cap_node = frame_cap->get_last_node();
    }
    m_lcd_disp = new lcd_disp::WhoFrameLCDDisp("LCDDisp", lcd_disp_frame_cap_node);
    WhoApp::add_task(m_lcd_disp);
    m_lcd_disp->set_lcd_disp_cb(std::bind(&WhoRecognitionAppLCD::lcd_disp_cb, this, std::placeholders::_1));

    m_recognition->set_recognizer(new HandGestureRecognizer());
    m_recognition->set_detect_model(new HandDetect());

    bsp_display_lock(0);
    m_label = create_lvgl_label("", &montserrat_bold_26);
    const lv_font_t *font = lv_obj_get_style_text_font(m_label, LV_PART_MAIN);
    lv_obj_align(m_label, LV_ALIGN_TOP_MID, 0, font->line_height);
    bsp_display_unlock();

    int disp_n_frames = 30;

    auto recognition_task = m_recognition->get_recognition_task();
    auto detect_task = m_recognition->get_detect_task();

    m_text_result_lcd_disp = new lcd_disp::WhoTextResultLCDDisp(recognition_task, m_label, disp_n_frames);
    m_detect_result_lcd_disp =
        new lcd_disp::WhoDetectResultLCDDisp(detect_task, m_lcd_disp->get_canvas(), {{0, 255, 0}});
    recognition_task->set_recognition_result_cb(
        std::bind(&WhoRecognitionAppLCD::recognition_result_cb, this, std::placeholders::_1));
#if defined(CONFIG_HAND_GESTURE_CLS_MODEL_LOCATION)
    recognition_task->set_recognition_result_cb(
        std::bind(&WhoRecognitionAppLCD::recognition_detail_result_cb, this, std::placeholders::_1));
#endif
    recognition_task->set_detect_result_cb(
        std::bind(&WhoRecognitionAppLCD::detect_result_cb, this, std::placeholders::_1));
    recognition_task->set_cleanup_func(std::bind(&WhoRecognitionAppLCD::recognition_cleanup, this));
    detect_task->set_detect_result_cb(std::bind(&WhoRecognitionAppLCD::detect_result_cb, this, std::placeholders::_1));
    detect_task->set_cleanup_func(std::bind(&WhoRecognitionAppLCD::detect_cleanup, this));
}

WhoRecognitionAppLCD::~WhoRecognitionAppLCD()
{
    delete m_text_result_lcd_disp;
    delete m_detect_result_lcd_disp;
#if defined(CONFIG_HAND_GESTURE_CLS_MODEL_LOCATION)
    vSemaphoreDelete(m_result_mutex);
#endif
    bsp_display_lock(0);
    lv_obj_del(m_label);
    bsp_display_unlock();
}

bool WhoRecognitionAppLCD::run()
{
    bool ret = WhoYield2Idle::get_instance()->run();
    for (const auto &frame_cap_node : m_frame_cap->get_all_nodes()) {
        ret &= frame_cap_node->run(4096, m_frame_cap_task_priority, 0);
    }
    ret &= m_lcd_disp->run(4096, m_lcd_task_priority, 0);
    ret &= m_recognition->get_detect_task()->run(4096, m_detect_task_priority, 1);
    ret &= m_recognition->get_recognition_task()->run(4096, m_recognition_task_priority, 1);
    return ret;
}

void WhoRecognitionAppLCD::set_task_priorities(UBaseType_t frame_cap_priority,
                                                UBaseType_t detect_priority,
                                                UBaseType_t recognition_priority,
                                                UBaseType_t lcd_priority)
{
    m_frame_cap_task_priority = frame_cap_priority;
    m_detect_task_priority = detect_priority;
    m_recognition_task_priority = recognition_priority;
    m_lcd_task_priority = lcd_priority;
}

void WhoRecognitionAppLCD::recognition_result_cb(const std::string &result)
{
    (void)result;
}

#if defined(CONFIG_HAND_GESTURE_CLS_MODEL_LOCATION)
recognition::WhoRecognitionCore::result_t WhoRecognitionAppLCD::get_recognition_result()
{
    xSemaphoreTake(m_result_mutex, portMAX_DELAY);
    auto result = m_last_recognition_result;
    xSemaphoreGive(m_result_mutex);
    return result;
}

void WhoRecognitionAppLCD::recognition_detail_result_cb(const recognition::WhoRecognitionCore::result_t &result)
{
    m_detect_result_lcd_disp->save_recognition_result(result.detect_result, result.recognition_results);
    xSemaphoreTake(m_result_mutex, portMAX_DELAY);
    m_last_recognition_result = result;
    xSemaphoreGive(m_result_mutex);
}
#endif

void WhoRecognitionAppLCD::detect_result_cb(const detect::WhoDetect::result_t &result)
{
    m_detect_result_lcd_disp->save_detect_result(result);
}

void WhoRecognitionAppLCD::lcd_disp_cb(who::cam::cam_fb_t *fb)
{
    m_detect_result_lcd_disp->lcd_disp_cb(fb);
    m_text_result_lcd_disp->lcd_disp_cb(fb);
}

void app::WhoRecognitionAppLCD::recognition_cleanup()
{
    m_text_result_lcd_disp->cleanup();
}

void app::WhoRecognitionAppLCD::detect_cleanup()
{
    m_detect_result_lcd_disp->cleanup();
}
} // namespace app
} // namespace who
