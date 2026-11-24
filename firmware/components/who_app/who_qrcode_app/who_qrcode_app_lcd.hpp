#pragma once
#include "who_frame_lcd_disp.hpp"
#include "who_qrcode_app_term.hpp"
#include <queue>

namespace who {
namespace app {
class WhoQRCodeAppLCD : public WhoQRCodeAppTerm {
public:
    WhoQRCodeAppLCD(frame_cap::WhoFrameCap *frame_cap,
                    frame_cap::WhoFrameCapNode *lcd_disp_frame_cap_node = nullptr);
    ~WhoQRCodeAppLCD();
    bool run() override;
    void set_task_priorities(UBaseType_t frame_cap_priority,
                             UBaseType_t qrcode_priority,
                             UBaseType_t lcd_priority);

protected:
    virtual void qrcode_result_cb(const std::string &result);
    virtual void qrcode_detect_result_cb(const qrcode::WhoQRCode::result_t &result);
    virtual void lcd_disp_cb(who::cam::cam_fb_t *fb);
    virtual void cleanup();

private:
    lcd_disp::WhoFrameLCDDisp *m_lcd_disp;
    SemaphoreHandle_t m_res_mutex;
    std::queue<qrcode::WhoQRCode::result_t> m_results;
    qrcode::WhoQRCode::result_t m_disp_result;
    UBaseType_t m_frame_cap_task_priority = 2;
    UBaseType_t m_qrcode_task_priority = 2;
    UBaseType_t m_lcd_task_priority = 2;
};
} // namespace app
} // namespace who
