#pragma once
#include "who_frame_cap.hpp"
#include <vector>

// Set to 1 to detect tag16h5; set to 0 to detect tag36h11.
#ifndef WHO_APRILTAG_USE_TAG16H5
#define WHO_APRILTAG_USE_TAG16H5 0
#endif

typedef struct apriltag_detector apriltag_detector_t;
typedef struct apriltag_family apriltag_family_t;

namespace who {
namespace apriltag {
class WhoApriltag : public task::WhoTask {
public:
    static inline constexpr EventBits_t NEW_FRAME = frame_cap::WhoFrameCapNode::NEW_FRAME;
    typedef struct {
        int id;
        int x1;
        int y1;
        int x2;
        int y2;
        float rotation;
        float x_translation;
        float y_translation;
        float z_translation;
        float x_rotation;
        float y_rotation;
        float z_rotation;
    } tag_t;
    typedef struct {
        struct timeval timestamp;
        std::vector<tag_t> tags;
    } result_t;

    WhoApriltag(const std::string &name, frame_cap::WhoFrameCapNode *frame_cap_node);
    ~WhoApriltag();
    void set_apriltag_result_cb(const std::function<void(const std::string &)> &result_cb);
    void set_apriltag_detect_result_cb(const std::function<void(const result_t &)> &result_cb);
    void set_apriltag_pose_params(float fx, float fy, float cx, float cy);
    void set_cleanup_func(const std::function<void()> &cleanup_func);

private:
    void task() override;
    void cleanup() override;

    apriltag_detector_t *td;
    apriltag_family_t *tf;
    uint8_t *gray_buf;
    frame_cap::WhoFrameCapNode *m_frame_cap_node;
    bool m_pose_params_set;
    float m_fx;
    float m_fy;
    float m_cx;
    float m_cy;
    
    std::function<void(const std::string &)> m_result_cb;
    std::function<void(const result_t &)> m_detect_result_cb;
    std::function<void()> m_cleanup;
};
} // namespace apriltag
} // namespace who
