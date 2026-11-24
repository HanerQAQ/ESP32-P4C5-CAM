#pragma once

#include "who_app.hpp"
#include "who_color_adjust_task.hpp"

namespace who {
namespace app {

class WhoCommunicateAppBase : public WhoApp {
public:
    explicit WhoCommunicateAppBase(const communicate::WhoColorAdjustConfig &config = {});

    bool send(const uint8_t *data, size_t len, TickType_t timeout = 0);
    bool send(const char *data, TickType_t timeout = 0);
    void set_recv_cb(const communicate::WhoColorAdjustTask::recv_cb_t &recv_cb);
    void set_settings_cbs(const communicate::WhoColorAdjustTask::settings_getter_t &getter,
                          const communicate::WhoColorAdjustTask::settings_saver_t &saver);
    void set_frame_cap_node(frame_cap::WhoFrameCapNode *frame_cap_node);
    who::cam::cam_fb_t *get_frame(int index = -1);

protected:
    communicate::WhoColorAdjustTask *m_color_adjust_task;
};

} // namespace app
} // namespace who
