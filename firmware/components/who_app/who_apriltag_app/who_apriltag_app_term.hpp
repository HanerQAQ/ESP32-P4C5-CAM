#pragma once
#include "who_apriltag_app_base.hpp"

namespace who {
namespace app {
class WhoApriltagAppTerm : public WhoApriltagAppBase {
public:
    WhoApriltagAppTerm(frame_cap::WhoFrameCap *frame_cap);
    bool run() override;

protected:
    virtual void apriltag_result_cb(const std::string &result);
};
} // namespace app
} // namespace who
