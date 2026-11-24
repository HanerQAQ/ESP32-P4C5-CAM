#pragma once
#include "who_app.hpp"
#include "who_apriltag.hpp"

namespace who {
namespace app {
class WhoApriltagAppBase : public WhoApp {
public:
    WhoApriltagAppBase(frame_cap::WhoFrameCap *frame_cap);

protected:
    frame_cap::WhoFrameCap *m_frame_cap;
    apriltag::WhoApriltag *m_apriltag;
};
} // namespace app
} // namespace who
