#include "who_communicate_app_term.hpp"
#include "who_yield2idle.hpp"

namespace who {
namespace app {

WhoCommunicateAppTerm::WhoCommunicateAppTerm(const communicate::WhoColorAdjustConfig &config,
                                             frame_cap::WhoFrameCapNode *frame_cap_node) :
    WhoCommunicateAppBase(config)
{
    set_frame_cap_node(frame_cap_node);
    set_recv_cb(std::bind(&WhoCommunicateAppTerm::uart_recv_cb, this, std::placeholders::_1, std::placeholders::_2));
}

bool WhoCommunicateAppTerm::run()
{
    // bool ret = WhoYield2Idle::get_instance()->run();
    bool ret = m_color_adjust_task->run(4096, 2, 0);
    return ret;
}

void WhoCommunicateAppTerm::uart_recv_cb(const uint8_t *data, size_t len)
{
    (void)data;
    (void)len;
}

} // namespace app
} // namespace who
