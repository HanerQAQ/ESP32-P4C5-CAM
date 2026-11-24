#include "who_apriltag.hpp"
#include "p4c5_cam_board.h"

#include "apriltag.h"
#include "common/homography.h"
#include "common/image_u8.h"
#include "common/matd.h"
#include "common/zarray.h"
#if WHO_APRILTAG_USE_TAG16H5
#include "tag16h5.h"
#else
#include "tag36h11.h"
#endif

#include <math.h>

static const char *TAG = "who_apriltag";

#define APRILTAG_RAD_TO_DEG (180.0f / (float)M_PI)

namespace who {
namespace apriltag {
WhoApriltag::WhoApriltag(const std::string &name, frame_cap::WhoFrameCapNode *frame_cap_node) :
    task::WhoTask(name), 
    m_frame_cap_node(frame_cap_node),
    m_pose_params_set(false),
    m_fx(0.0f),
    m_fy(0.0f),
    m_cx(0.0f),
    m_cy(0.0f)
{
    frame_cap_node->add_new_frame_signal_subscriber(this);
    const size_t gray_buf_size = BSP_LCD_H_RES * BSP_LCD_V_RES;
    gray_buf = (uint8_t *)heap_caps_malloc(gray_buf_size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (gray_buf == NULL) {
        ESP_LOGW(TAG,
                 "No contiguous internal RAM for %u-byte gray buffer, falling back to PSRAM",
                 (unsigned)gray_buf_size);
        gray_buf = (uint8_t *)heap_caps_malloc(gray_buf_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    assert(gray_buf != NULL);
#if WHO_APRILTAG_USE_TAG16H5
    tf = tag16h5_create();
#else
    tf = tag36h11_create();
#endif
    td = apriltag_detector_create();
    apriltag_detector_add_family_bits(td, tf , 1);  // 将标签家族添加到检测器
    td->quad_decimate = 2.0;      // 增大采样倍数，降低检测分辨率，提高速度
    td->quad_sigma = 0.0f;        // 增大高斯模糊，减少噪声影响
    td->nthreads = 1;             // 使用单线程，避免栈溢出问题
    td->debug = 0;                // 关闭调试模式
    td->refine_edges = 0;         // 关闭边缘细化，大幅提高速度
    ESP_LOGI(TAG, "AprilTag detector initialized, family: %s", tf->name);
}

WhoApriltag::~WhoApriltag()
{
    apriltag_detector_destroy(td);
#if WHO_APRILTAG_USE_TAG16H5
    tag16h5_destroy(tf);
#else
    tag36h11_destroy(tf);
#endif
    heap_caps_free(gray_buf);
}

void WhoApriltag::task()
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
        if (!fb) {
            continue;
        }

        const uint16_t *src = reinterpret_cast<const uint16_t *>(fb->buf);
        for (uint32_t y = 0; y < fb->height; y++) {
            uint8_t *dst_row = gray_buf + y * fb->width;
            const uint16_t *src_row = src + y * fb->width;
            for (uint32_t x = 0; x < fb->width; x++) {
                const uint16_t rgb565 = src_row[x];
                const uint8_t r = static_cast<uint8_t>(((rgb565 >> 11) & 0x1F) * 255 / 31);
                const uint8_t g = static_cast<uint8_t>(((rgb565 >> 5) & 0x3F) * 255 / 63);
                const uint8_t b = static_cast<uint8_t>((rgb565 & 0x1F) * 255 / 31);
                dst_row[x] = static_cast<uint8_t>((77 * r + 150 * g + 29 * b) >> 8);
            }
        }
        image_u8_t im = {
            .width = fb->width,
            .height = fb->height,
            .stride = fb->width,
            .buf = gray_buf  
        };        

        zarray_t *detections = apriltag_detector_detect(td, &im);
        result_t result = {
            .timestamp = fb->timestamp,
            .tags = {},
        };

        for (int i = 0; i < zarray_size(detections); i++) {
            apriltag_detection_t *det;
            zarray_get(detections, i, &det);

            // ESP_LOGI(TAG,
            //          "tag id=%d family=%s center=[%.1f %.1f] hamming=%d margin=%.2f",
            //          det->id,
            //          det->family->name,
            //          det->c[0],
            //          det->c[1],
            //          det->hamming,
            //          det->decision_margin);
            
            int min_x = (int)det->p[0][0];
            int max_x = (int)det->p[0][0];
            int min_y = (int)det->p[0][1];
            int max_y = (int)det->p[0][1];
            
            for (int j = 1; j < 4; j++) {
                if (det->p[j][0] < min_x) min_x = (int)det->p[j][0];
                if (det->p[j][0] > max_x) max_x = (int)det->p[j][0];
                if (det->p[j][1] < min_y) min_y = (int)det->p[j][1];
                if (det->p[j][1] > max_y) max_y = (int)det->p[j][1];
            }

            const float fx = 125.7f;
            const float fy = 125.7f;
            const float cx = 160.0f;
            const float cy = 120.0f;
            matd_t *pose = homography_to_pose(det->H, -fx, fy, cx, cy);

            const float x_translation = MATD_EL(pose, 0, 3);
            const float y_translation = MATD_EL(pose, 1, 3);
            const float z_translation = MATD_EL(pose, 2, 3);
            const float x_rotation = atan2f(MATD_EL(pose, 2, 1), MATD_EL(pose, 2, 2)) * APRILTAG_RAD_TO_DEG;
            const double r21 = MATD_EL(pose, 2, 1);
            const double r22 = MATD_EL(pose, 2, 2);
            const float y_rotation =
                atan2f(-MATD_EL(pose, 2, 0), sqrtf((r21 * r21) + (r22 * r22))) * APRILTAG_RAD_TO_DEG;
            const float z_rotation = atan2f(MATD_EL(pose, 1, 0), MATD_EL(pose, 0, 0)) * APRILTAG_RAD_TO_DEG;
            const float rotation = atan2f(det->p[1][1] - det->p[0][1], det->p[1][0] - det->p[0][0]) *
                                   APRILTAG_RAD_TO_DEG;

            matd_destroy(pose);

            // ESP_LOGI(TAG,
            //          "tag id=%d rotation=%.4f translation=[%.4f %.4f %.4f] rotation_xyz=[%.4f %.4f %.4f] deg",
            //          det->id,
            //          rotation,
            //          x_translation,
            //          y_translation,
            //          z_translation,
            //          x_rotation,
            //          y_rotation,
            //          z_rotation);

            result.tags.push_back({
                .id = det->id,
                .x1 = min_x,
                .y1 = min_y,
                .x2 = max_x,
                .y2 = max_y,
                .rotation = rotation,
                .x_translation = x_translation,
                .y_translation = y_translation,
                .z_translation = z_translation,
                .x_rotation = x_rotation,
                .y_rotation = y_rotation,
                .z_rotation = z_rotation,
            });
        }

        if (m_detect_result_cb) {
            m_detect_result_cb(result);
        }

        apriltag_detections_destroy(detections);
    }
    xEventGroupSetBits(m_event_group, TASK_STOPPED);
    vTaskDelete(NULL);
}

void WhoApriltag::set_apriltag_result_cb(const std::function<void(const std::string &)> &result_cb)
{
    m_result_cb = result_cb;
}

void WhoApriltag::set_apriltag_detect_result_cb(const std::function<void(const result_t &)> &result_cb)
{
    m_detect_result_cb = result_cb;
}

void WhoApriltag::set_apriltag_pose_params(float fx, float fy, float cx, float cy)
{
    m_fx = fx;
    m_fy = fy;
    m_cx = cx;
    m_cy = cy;
    m_pose_params_set = true;
}

void WhoApriltag::set_cleanup_func(const std::function<void()> &cleanup_func)
{
    m_cleanup = cleanup_func;
}

void WhoApriltag::cleanup()
{
    if (m_cleanup) {
        m_cleanup();
    }
}
} // namespace apriltag
} // namespace who
