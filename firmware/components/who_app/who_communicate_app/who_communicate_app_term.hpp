#pragma once

#include "who_uart_app_base.hpp"

namespace who {
namespace app {

class WhoCommunicateAppTerm : public WhoCommunicateAppBase {
public:
    explicit WhoCommunicateAppTerm(const communicate::WhoColorAdjustConfig &config = {},
                                   frame_cap::WhoFrameCapNode *frame_cap_node = nullptr);
    bool run() override;

protected:
    virtual void uart_recv_cb(const uint8_t *data, size_t len);
};

} // namespace app
} // namespace who
