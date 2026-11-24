#pragma once

#include "dl_detect_base.hpp"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include <cstdint>

class ColorDetect : public dl::detect::Detect {
public:
    ColorDetect(uint16_t width, uint16_t height);
    ~ColorDetect();
    void register_color(const std::vector<uint8_t> &hsv_min,
                        const std::vector<uint8_t> &hsv_max,
                        const std::string &name,
                        int area_thr = 256,
                        const std::vector<uint8_t> &box_color = {});
    void clear_colors();
    void delete_color(int idx);
    void delete_color(const std::string &name);
    void enable_morphology(int kernel_size = 5);
    std::string get_color_name(int idx);
    std::list<dl::detect::result_t> &run(const dl::image::img_t &img) override;
    Detect &set_score_thr(float score_thr, int idx) override { return *this; }
    Detect &set_nms_thr(float nms_thr, int idx) override { return *this; }
    dl::Model *get_raw_model(int idx) override { return nullptr; }

private:
    void gen_xy_map();
    void make_hsv_mask(size_t color_index);
    void morph(bool erode);
    void collect_components(int category, int area_threshold);

    dl::image::img_t m_src_img;
    uint16_t m_width;
    uint16_t m_height;
    void *m_hsv_mask;
    void *m_mask_label;
    std::vector<std::vector<uint8_t>> m_hsv_min;
    std::vector<std::vector<uint8_t>> m_hsv_max;
    std::vector<std::vector<uint8_t>> m_box_colors;
    std::vector<int> m_area_thr;
    std::vector<std::string> m_color_names;
    float m_inv_scale_x;
    float m_inv_scale_y;
    int *m_x;
    int *m_y;
    bool m_gen_xy_map;
    bool m_morphology;
    int m_morph_kernel_size;
    uint8_t *m_morph_buffer;
    uint32_t *m_cc_queue;
    std::list<dl::detect::result_t> m_result;
    SemaphoreHandle_t m_mutex;
};
