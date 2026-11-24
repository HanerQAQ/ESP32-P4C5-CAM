#pragma once

#include "dl_detect_base.hpp"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <cstdint>
#include <list>
#include <vector>

class LineRegressionDetect : public dl::detect::Detect {
public:
    LineRegressionDetect(uint16_t width, uint16_t height);
    ~LineRegressionDetect();

    void set_roi(float y_start_ratio, float y_end_ratio = 1.0f);
    void set_hsv_threshold(const std::vector<uint8_t> &hsv_min, const std::vector<uint8_t> &hsv_max);
    void set_min_pixels(int min_pixels);
    void set_stride(int x_stride, int y_stride);

    std::list<dl::detect::result_t> &run(const dl::image::img_t &img) override;
    Detect &set_score_thr(float score_thr, int idx) override { return *this; }
    Detect &set_nms_thr(float nms_thr, int idx) override { return *this; }
    dl::Model *get_raw_model(int idx) override { return nullptr; }

private:
    bool is_target_pixel(uint16_t pixel) const;

    uint16_t m_width;
    uint16_t m_height;
    float m_roi_y_start_ratio;
    float m_roi_y_end_ratio;
    uint8_t m_hsv_min[3];
    uint8_t m_hsv_max[3];
    int m_min_pixels;
    int m_x_stride;
    int m_y_stride;
    std::list<dl::detect::result_t> m_result;
    SemaphoreHandle_t m_mutex;
};
