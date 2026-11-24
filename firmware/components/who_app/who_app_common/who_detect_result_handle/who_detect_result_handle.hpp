#pragma once
#include "who_detect.hpp"
#include <queue>
#include <string>
#include <vector>
#include "p4c5_cam_board.h"

namespace who {
namespace detect {
void draw_detect_results_on_img(const dl::image::img_t &img,
                                const std::list<dl::detect::result_t> &detect_res,
                                const std::vector<std::vector<uint8_t>> &palette);
#if !BSP_CONFIG_NO_GRAPHIC_LIB
void draw_detect_results_on_canvas(lv_obj_t *canvas,
                                   const std::list<dl::detect::result_t> &detect_res,
                                   const std::vector<lv_color_t> &palette,
                                   const std::vector<std::string> &category_labels = {});
#if defined(CONFIG_HAND_GESTURE_CLS_MODEL_LOCATION)
void draw_recognition_results_on_canvas(lv_obj_t *canvas,
                                        const std::list<dl::detect::result_t> &detect_res,
                                        const std::vector<lv_color_t> &palette,
                                        const std::vector<std::string> &recognition_results);
#endif
#endif
void print_detect_results(const std::list<dl::detect::result_t> &detect_res);
} // namespace detect

namespace lcd_disp {
class WhoDetectResultLCDDisp {
public:
#if !BSP_CONFIG_NO_GRAPHIC_LIB
    WhoDetectResultLCDDisp(task::WhoTask *task, lv_obj_t *canvas, const std::vector<std::vector<uint8_t>> &palette);
#else
    WhoDetectResultLCDDisp(task::WhoTask *task, const std::vector<std::vector<uint8_t>> &palette);
#endif
    ~WhoDetectResultLCDDisp();
    void save_detect_result(const detect::WhoDetect::result_t &result);
#if defined(CONFIG_HAND_GESTURE_CLS_MODEL_LOCATION)
    void save_recognition_result(const detect::WhoDetect::result_t &detect_result,
                                 const std::vector<std::string> &recognition_results);
#endif
    void set_palette(const std::vector<std::vector<uint8_t>> &palette);
    void set_category_labels(const std::vector<std::string> &category_labels);
    void lcd_disp_cb(who::cam::cam_fb_t *fb);
    void cleanup();

private:
    // Bound queued frames when rendering falls behind inference. Keep the latest
    // few timestamps for camera/result synchronization and discard older frames.
    static constexpr size_t MAX_PENDING_RESULTS = 4;
    task::WhoTask *m_task;
    SemaphoreHandle_t m_res_mutex;
    std::queue<detect::WhoDetect::result_t> m_results;
    detect::WhoDetect::result_t m_result;
#if defined(CONFIG_HAND_GESTURE_CLS_MODEL_LOCATION)
    typedef struct {
        detect::WhoDetect::result_t detect_result;
        std::vector<std::string> recognition_results;
    } recognition_result_t;
    std::queue<recognition_result_t> m_recognition_results;
    std::vector<std::string> m_recognition_result;
#endif
#if BSP_CONFIG_NO_GRAPHIC_LIB
    std::vector<std::vector<uint8_t>> m_rgb888_palette;
    std::vector<std::vector<uint8_t>> m_rgb565_palette;
#else
    lv_obj_t *m_canvas;
    std::vector<lv_color_t> m_palette;
#endif
    std::vector<std::string> m_category_labels;
};
} // namespace lcd_disp
} // namespace who
