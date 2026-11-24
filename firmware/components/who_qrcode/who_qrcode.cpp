#include "who_qrcode.hpp"
#include "quirc.h"
#include "who_qrcode.hpp"
#include "p4c5_cam_board.h"
#include <algorithm>
#include <utility>

namespace who {
namespace qrcode {
WhoQRCode::WhoQRCode(const std::string &name, frame_cap::WhoFrameCapNode *frame_cap_node) :
    task::WhoTask(name), m_frame_cap_node(frame_cap_node), m_qr(quirc_new())
{
    frame_cap_node->add_new_frame_signal_subscriber(this);
    uint16_t w = BSP_LCD_H_RES / 2, h = BSP_LCD_V_RES / 2;
    // uint16_t w = BSP_LCD_V_RES / 2, h = BSP_LCD_H_RES / 2;  //横屏
    quirc_resize(m_qr, w, h);
}

WhoQRCode::~WhoQRCode()
{
    quirc_destroy(m_qr);
}

void WhoQRCode::task()
{
    while (true) {
        EventBits_t event_bits =
            xEventGroupWaitBits(m_event_group, NEW_FRAME | TASK_PAUSE | TASK_STOP, pdTRUE, pdFALSE, portMAX_DELAY);
        if (event_bits & TASK_STOP) {
            break;
        } else if (event_bits & TASK_PAUSE) {
            xEventGroupSetBits(m_event_group, TASK_PAUSED);
            EventBits_t pause_event_bits =
                xEventGroupWaitBits(m_event_group, TASK_RESUME | TASK_STOP, pdTRUE, pdFALSE, portMAX_DELAY);
            if (pause_event_bits & TASK_STOP) {
                break;
            } else {
                continue;
            }
        }
        auto fb = m_frame_cap_node->cam_fb_peek();
        int w, h;
        uint8_t *qr_buf = quirc_begin(m_qr, &w, &h);
        dl::image::img_t dst_img = {
            .data = qr_buf, .width = (uint16_t)w, .height = (uint16_t)h, .pix_type = dl::image::DL_IMAGE_PIX_TYPE_GRAY};
        m_image_transformer.set_src_img(*fb).set_dst_img(dst_img).transform();
        quirc_end(m_qr);
        int num_codes = quirc_count(m_qr);
        result_t result = {};
        result.timestamp = fb->timestamp;
        for (int i = 0; i < num_codes; i++) {
            struct quirc_code code;
            struct quirc_data decoded_data;
            quirc_decode_error_t err;
            quirc_extract(m_qr, i, &code);
            err = quirc_decode(&code, &decoded_data);
            if (err == QUIRC_ERROR_DATA_ECC) {
                quirc_flip(&code);
                err = quirc_decode(&code, &decoded_data);
            }
            if (err) {
                continue;
            }

            int min_x = code.corners[0].x;
            int min_y = code.corners[0].y;
            int max_x = code.corners[0].x;
            int max_y = code.corners[0].y;
            for (const auto &corner : code.corners) {
                min_x = std::min(min_x, corner.x);
                min_y = std::min(min_y, corner.y);
                max_x = std::max(max_x, corner.x);
                max_y = std::max(max_y, corner.y);
            }

            const float scale_x = w > 0 ? static_cast<float>(fb->width) / static_cast<float>(w) : 1.0f;
            const float scale_y = h > 0 ? static_cast<float>(fb->height) / static_cast<float>(h) : 1.0f;
            code_t item = {};
            item.x = static_cast<uint16_t>(std::max(0.0f, min_x * scale_x));
            item.y = static_cast<uint16_t>(std::max(0.0f, min_y * scale_y));
            item.w = static_cast<uint16_t>(std::max(0.0f, (max_x - min_x) * scale_x));
            item.h = static_cast<uint16_t>(std::max(0.0f, (max_y - min_y) * scale_y));
            item.payload.assign(reinterpret_cast<const char *>(decoded_data.payload), decoded_data.payload_len);
            result.codes.push_back(std::move(item));
        }

        if (!result.codes.empty() && m_result_cb) {
            m_result_cb(result.codes.front().payload);
        }
        if (m_detect_result_cb) {
            m_detect_result_cb(result);
        }
    }
    xEventGroupSetBits(m_event_group, TASK_STOPPED);
    vTaskDelete(NULL);
}

void WhoQRCode::set_qrcode_result_cb(const std::function<void(const std::string &)> &result_cb)
{
    m_result_cb = result_cb;
}

void WhoQRCode::set_qrcode_detect_result_cb(const std::function<void(const result_t &)> &result_cb)
{
    m_detect_result_cb = result_cb;
}

void WhoQRCode::set_cleanup_func(const std::function<void()> &cleanup_func)
{
    m_cleanup = cleanup_func;
}

void WhoQRCode::cleanup()
{
    if (m_cleanup) {
        m_cleanup();
    }
}
} // namespace qrcode
} // namespace who
