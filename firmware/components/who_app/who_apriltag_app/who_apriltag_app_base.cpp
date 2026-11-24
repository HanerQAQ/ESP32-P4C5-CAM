#include "who_apriltag_app_base.hpp"

namespace who {
namespace app {
WhoApriltagAppBase::WhoApriltagAppBase(frame_cap::WhoFrameCap *frame_cap) :
    m_frame_cap(frame_cap), m_apriltag(new apriltag::WhoApriltag("Apriltag", m_frame_cap->get_last_node()))
{
    WhoApp::add_task_group(m_frame_cap);
    WhoApp::add_task(m_apriltag);
}
} // namespace app
} // namespace who
