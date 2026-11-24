#pragma once
#include "who_frame_lcd_disp.hpp"
#include "who_apriltag_app_term.hpp"

namespace who {
namespace app {
class WhoApriltagAppLCD : public WhoApriltagAppTerm {
public:
    WhoApriltagAppLCD(frame_cap::WhoFrameCap *frame_cap,
                    frame_cap::WhoFrameCapNode *lcd_disp_frame_cap_node = nullptr);
    ~WhoApriltagAppLCD();
    bool run() override;
    void set_task_priorities(UBaseType_t frame_cap_priority,
                             UBaseType_t apriltag_priority,
                             UBaseType_t lcd_priority);

protected:
    virtual void apriltag_detect_result_cb(const apriltag::WhoApriltag::result_t &result);
    virtual void lcd_disp_cb(who::cam::cam_fb_t *fb);
    virtual void cleanup();

private:
    void draw_apriltag_result_on_canvas(const apriltag::WhoApriltag::result_t &result);

    lcd_disp::WhoFrameLCDDisp *m_lcd_disp;
    SemaphoreHandle_t m_res_mutex;
    bool m_has_new_result;
    apriltag::WhoApriltag::result_t m_result;
    apriltag::WhoApriltag::result_t m_disp_result;
    UBaseType_t m_frame_cap_task_priority = 2;
    UBaseType_t m_apriltag_task_priority = 2;
    UBaseType_t m_lcd_task_priority = 2;
};
} // namespace app
} // namespace who
