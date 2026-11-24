#include "who_uart_app_base.hpp"

namespace who {
namespace app {

WhoCommunicateAppBase::WhoCommunicateAppBase(const communicate::WhoColorAdjustConfig &config) :
    m_color_adjust_task(new communicate::WhoColorAdjustTask("ColorAdjust", config))
{
    WhoApp::add_task(m_color_adjust_task);
}

bool WhoCommunicateAppBase::send(const uint8_t *data, size_t len, TickType_t timeout)
{
    return m_color_adjust_task->send(data, len, timeout);
}

bool WhoCommunicateAppBase::send(const char *data, TickType_t timeout)
{
    return m_color_adjust_task->send(data, timeout);
}

void WhoCommunicateAppBase::set_recv_cb(const communicate::WhoColorAdjustTask::recv_cb_t &recv_cb)
{
    m_color_adjust_task->set_recv_cb(recv_cb);
}

void WhoCommunicateAppBase::set_settings_cbs(const communicate::WhoColorAdjustTask::settings_getter_t &getter,
                                             const communicate::WhoColorAdjustTask::settings_saver_t &saver)
{
    m_color_adjust_task->set_settings_cbs(getter, saver);
}

void WhoCommunicateAppBase::set_frame_cap_node(frame_cap::WhoFrameCapNode *frame_cap_node)
{
    m_color_adjust_task->set_frame_cap_node(frame_cap_node);
}

who::cam::cam_fb_t *WhoCommunicateAppBase::get_frame(int index)
{
    return m_color_adjust_task->get_frame(index);
}

} // namespace app
} // namespace who
