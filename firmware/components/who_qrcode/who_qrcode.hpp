#pragma once
#include "who_frame_cap.hpp"
#include <functional>
#include <string>
#include <vector>

struct quirc;
namespace who {
namespace qrcode {
class WhoQRCode : public task::WhoTask {
public:
    static inline constexpr EventBits_t NEW_FRAME = frame_cap::WhoFrameCapNode::NEW_FRAME;

    typedef struct {
        uint16_t x;
        uint16_t y;
        uint16_t w;
        uint16_t h;
        std::string payload;
    } code_t;

    typedef struct {
        std::vector<code_t> codes;
        struct timeval timestamp;
    } result_t;

    WhoQRCode(const std::string &name, frame_cap::WhoFrameCapNode *frame_cap_node);
    ~WhoQRCode();
    void set_qrcode_result_cb(const std::function<void(const std::string &)> &result_cb);
    void set_qrcode_detect_result_cb(const std::function<void(const result_t &)> &result_cb);
    void set_cleanup_func(const std::function<void()> &cleanup_func);

private:
    void task() override;
    void cleanup() override;

    frame_cap::WhoFrameCapNode *m_frame_cap_node;
    struct quirc *m_qr;
    dl::image::ImageTransformer m_image_transformer;
    std::function<void(const std::string &)> m_result_cb;
    std::function<void(const result_t &)> m_detect_result_cb;
    std::function<void()> m_cleanup;
};
} // namespace qrcode
} // namespace who
