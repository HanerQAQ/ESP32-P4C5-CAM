#include "who_apriltag_app_term.hpp"
#include "who_yield2idle.hpp"

namespace who {
namespace app {
WhoApriltagAppTerm::WhoApriltagAppTerm(frame_cap::WhoFrameCap *frame_cap) : WhoApriltagAppBase(frame_cap)
{
    m_apriltag->set_apriltag_result_cb(std::bind(&WhoApriltagAppTerm::apriltag_result_cb, this, std::placeholders::_1));
}

bool WhoApriltagAppTerm::run()
{
    bool ret = WhoYield2Idle::get_instance()->run();
    for (const auto &frame_cap_node : m_frame_cap->get_all_nodes()) {
        ret &= frame_cap_node->run(4096, 2, 0);
    }
    ret &= m_apriltag->run(4096, 2, 1);
    return ret;
}

void WhoApriltagAppTerm::apriltag_result_cb(const std::string &result)
{
}
} // namespace app
} // namespace who
