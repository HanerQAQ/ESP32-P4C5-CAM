#pragma once
#include "who_detect_result_handle.hpp"
#include "who_frame_lcd_disp.hpp"
#include "who_recognition_app_base.hpp"
#include "who_text_result_handle.hpp"

namespace who {
namespace app {
class WhoRecognitionAppLCD : public WhoRecognitionAppBase {
public:
    WhoRecognitionAppLCD(frame_cap::WhoFrameCap *frame_cap, 
                         frame_cap::WhoFrameCapNode *lcd_disp_frame_cap_node);
    ~WhoRecognitionAppLCD();
    bool run() override;
    void set_task_priorities(UBaseType_t frame_cap_priority,
                             UBaseType_t detect_priority,
                             UBaseType_t recognition_priority,
                             UBaseType_t lcd_priority);
#if defined(CONFIG_HAND_GESTURE_CLS_MODEL_LOCATION)
    recognition::WhoRecognitionCore::result_t get_recognition_result();
#endif

protected:
    virtual void recognition_result_cb(const std::string &result);
#if defined(CONFIG_HAND_GESTURE_CLS_MODEL_LOCATION)
    virtual void recognition_detail_result_cb(const recognition::WhoRecognitionCore::result_t &result);
#endif
    virtual void detect_result_cb(const detect::WhoDetect::result_t &result);
    virtual void lcd_disp_cb(who::cam::cam_fb_t *fb);
    virtual void recognition_cleanup();
    virtual void detect_cleanup();

private:
    lcd_disp::WhoFrameLCDDisp *m_lcd_disp;
    lcd_disp::WhoTextResultLCDDisp *m_text_result_lcd_disp;
    lcd_disp::WhoDetectResultLCDDisp *m_detect_result_lcd_disp;
    lv_obj_t *m_label;
    UBaseType_t m_frame_cap_task_priority = 2;
    UBaseType_t m_detect_task_priority = 2;
    UBaseType_t m_recognition_task_priority = 2;
    UBaseType_t m_lcd_task_priority = 2;
#if defined(CONFIG_HAND_GESTURE_CLS_MODEL_LOCATION)
    SemaphoreHandle_t m_result_mutex;
    recognition::WhoRecognitionCore::result_t m_last_recognition_result;
#endif
};
} // namespace app
} // namespace who
