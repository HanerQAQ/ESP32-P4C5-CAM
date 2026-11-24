#include "line_regression_detect.hpp"
#include "esp_log.h"
#include <algorithm>
#include <cassert>
#include <cmath>

static const char *TAG = "LineRegression";

namespace {
class MutexGuard {
public:
    explicit MutexGuard(SemaphoreHandle_t mutex) : m_mutex(mutex)
    {
        xSemaphoreTake(m_mutex, portMAX_DELAY);
    }

    ~MutexGuard()
    {
        xSemaphoreGive(m_mutex);
    }

private:
    SemaphoreHandle_t m_mutex;
};

void rgb888_to_hsv(uint8_t r, uint8_t g, uint8_t b, uint8_t hsv[3])
{
    uint8_t max_v = std::max({r, g, b});
    uint8_t min_v = std::min({r, g, b});
    int delta = max_v - min_v;

    int h = 0;
    if (delta != 0) {
        if (max_v == r) {
            h = 30 * (g - b) / delta;
            if (h < 0) {
                h += 180;
            }
        } else if (max_v == g) {
            h = 60 + 30 * (b - r) / delta;
        } else {
            h = 120 + 30 * (r - g) / delta;
        }
    }

    hsv[0] = static_cast<uint8_t>(std::clamp(h, 0, 180));
    hsv[1] = max_v == 0 ? 0 : static_cast<uint8_t>((255 * delta) / max_v);
    hsv[2] = max_v;
}
} // namespace

LineRegressionDetect::LineRegressionDetect(uint16_t width, uint16_t height) :
    m_width(width),
    m_height(height),
    m_roi_y_start_ratio(0.45f),
    m_roi_y_end_ratio(1.0f),
    m_hsv_min{170, 100, 100},
    m_hsv_max{10, 255, 255},
    m_min_pixels(120),
    m_x_stride(2),
    m_y_stride(2),
    m_mutex(xSemaphoreCreateMutex())
{
    assert(m_mutex);
}

LineRegressionDetect::~LineRegressionDetect()
{
    vSemaphoreDelete(m_mutex);
}

void LineRegressionDetect::set_roi(float y_start_ratio, float y_end_ratio)
{
    MutexGuard lock(m_mutex);
    m_roi_y_start_ratio = std::clamp(y_start_ratio, 0.0f, 1.0f);
    m_roi_y_end_ratio = std::clamp(y_end_ratio, m_roi_y_start_ratio, 1.0f);
}

void LineRegressionDetect::set_hsv_threshold(const std::vector<uint8_t> &hsv_min,
                                             const std::vector<uint8_t> &hsv_max)
{
    if (hsv_min.size() != 3 || hsv_max.size() != 3) {
        return;
    }
    MutexGuard lock(m_mutex);
    for (int i = 0; i < 3; i++) {
        m_hsv_min[i] = hsv_min[i];
        m_hsv_max[i] = hsv_max[i];
    }
}

void LineRegressionDetect::set_min_pixels(int min_pixels)
{
    MutexGuard lock(m_mutex);
    m_min_pixels = std::max(min_pixels, 2);
}

void LineRegressionDetect::set_stride(int x_stride, int y_stride)
{
    MutexGuard lock(m_mutex);
    m_x_stride = std::max(x_stride, 1);
    m_y_stride = std::max(y_stride, 1);
}

bool LineRegressionDetect::is_target_pixel(uint16_t pixel) const
{
#if !CONFIG_IDF_TARGET_ESP32P4
    pixel = __builtin_bswap16(pixel);
#endif
    int r = (pixel >> 11) & 0x1f;
    int g = (pixel >> 5) & 0x3f;
    int b = pixel & 0x1f;
    uint8_t hsv[3] = {};
    rgb888_to_hsv((r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2), hsv);

    bool hue_match = false;
    if (m_hsv_min[0] <= m_hsv_max[0]) {
        hue_match = hsv[0] >= m_hsv_min[0] && hsv[0] <= m_hsv_max[0];
    } else {
        hue_match = hsv[0] >= m_hsv_min[0] || hsv[0] <= m_hsv_max[0];
    }
    return hue_match &&
           hsv[1] >= m_hsv_min[1] && hsv[1] <= m_hsv_max[1] &&
           hsv[2] >= m_hsv_min[2] && hsv[2] <= m_hsv_max[2];
}

std::list<dl::detect::result_t> &LineRegressionDetect::run(const dl::image::img_t &img)
{
    MutexGuard lock(m_mutex);
    m_result.clear();
    if (!img.data || !img.width || !img.height || img.pix_type != dl::image::DL_IMAGE_PIX_TYPE_RGB565LE) {
        ESP_LOGE(TAG, "Invalid image. RGB565 input is required.");
        return m_result;
    }

    int roi_y0 = std::clamp((int)std::lround(img.height * m_roi_y_start_ratio), 0, (int)img.height - 1);
    int roi_y1 = std::clamp((int)std::lround(img.height * m_roi_y_end_ratio), roi_y0 + 1, (int)img.height);
    const uint16_t *pixels = static_cast<const uint16_t *>(img.data);

    int64_t n = 0;
    int64_t sum_x = 0;
    int64_t sum_y = 0;
    int64_t sum_yy = 0;
    int64_t sum_xy = 0;
    int min_x = img.width - 1;
    int max_x = 0;
    int min_y = roi_y1 - 1;
    int max_y = roi_y0;

    for (int y = roi_y0; y < roi_y1; y += m_y_stride) {
        const uint16_t *row = pixels + y * img.width;
        for (int x = y % m_x_stride; x < img.width; x += m_x_stride) {
            if (is_target_pixel(row[x])) {
                n++;
                sum_x += x;
                sum_y += y;
                sum_yy += y * y;
                sum_xy += x * y;
                min_x = std::min(min_x, x);
                max_x = std::max(max_x, x);
                min_y = std::min(min_y, y);
                max_y = std::max(max_y, y);
            }
        }
    }

    if (n < m_min_pixels) {
        return m_result;
    }

    int64_t denom = n * sum_yy - sum_y * sum_y;
    if (denom == 0) {
        return m_result;
    }

    float slope = (float)(n * sum_xy - sum_x * sum_y) / (float)denom;
    float intercept = ((float)sum_x - slope * (float)sum_y) / (float)n;

    int y0 = roi_y0;
    int y1 = roi_y1 - 1;
    int x0 = std::clamp((int)std::lround(slope * y0 + intercept), 0, (int)img.width - 1);
    int x1 = std::clamp((int)std::lround(slope * y1 + intercept), 0, (int)img.width - 1);

    int margin = 4;
    int box_x0 = std::clamp(std::min({x0, x1, min_x}) - margin, 0, (int)img.width - 1);
    int box_y0 = std::clamp(min_y - margin, 0, (int)img.height - 1);
    int box_x1 = std::clamp(std::max({x0, x1, max_x}) + margin, 0, (int)img.width - 1);
    int box_y1 = std::clamp(max_y + margin, 0, (int)img.height - 1);

    float score = std::min(1.0f, (float)n / (float)((roi_y1 - roi_y0) * img.width / 8));
    m_result.push_back({0, score, {box_x0, box_y0, box_x1, box_y1}, {x0, y0, x1, y1}});
    return m_result;
}
