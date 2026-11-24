#include "who_detect_result_handle.hpp"
#if !BSP_CONFIG_NO_GRAPHIC_LIB
#include "who_lvgl_utils.hpp"
#endif
#include <utility>

LV_FONT_DECLARE(montserrat_bold_20);

#if defined(CONFIG_COCO_DETECT_MODEL_LOCATION)

static const char* COCO_CLASS_NAMES[] = {
    "person",           // 人
    "bicycle",          // 自行车
    "car",              // 汽车
    "motorcycle",       // 摩托车
    "airplane",         // 飞机
    "bus",              // 公交车
    "train",            // 火车
    "truck",            // 卡车
    "boat",             // 船
    "traffic light",    // 交通灯
    "fire hydrant",     // 消防栓
    "stop sign",        // 停车标志
    "parking meter",    // 停车计时器
    "bench",            // 长椅
    "bird",             // 鸟
    "cat",              // 猫
    "dog",              // 狗
    "horse",            // 马
    "sheep",            // 羊
    "cow",              // 牛
    "elephant",         // 大象
    "bear",             // 熊
    "zebra",            // 斑马
    "giraffe",          // 长颈鹿
    "backpack",         // 背包
    "umbrella",         // 雨伞
    "handbag",          // 手提包
    "tie",              // 领带
    "suitcase",         // 行李箱
    "frisbee",          // 飞盘
    "skis",             // 滑雪板
    "snowboard",        // 单板滑雪
    "sports ball",      // 运动球
    "kite",             // 风筝
    "baseball bat",     // 棒球棒
    "baseball glove",   // 棒球手套
    "skateboard",       // 滑板
    "surfboard",        // 冲浪板
    "tennis racket",    // 网球拍
    "bottle",           // 瓶子
    "wine glass",       // 酒杯
    "cup",              // 杯子
    "fork",             // 叉子
    "knife",            // 刀
    "spoon",            // 勺子
    "bowl",             // 碗
    "banana",           // 香蕉
    "apple",            // 苹果
    "sandwich",         // 三明治
    "orange",           // 橙子
    "broccoli",         // 西兰花
    "carrot",           // 胡萝卜
    "hot dog",          // 热狗
    "pizza",            // 披萨
    "donut",            // 甜甜圈
    "cake",             // 蛋糕
    "chair",            // 椅子
    "couch",            // 沙发
    "potted plant",     // 盆栽
    "bed",              // 床
    "dining table",     // 餐桌
    "toilet",           // 厕所
    "tv",               // 电视
    "laptop",           // 笔记本电脑
    "mouse",            // 鼠标
    "remote",           // 遥控器
    "keyboard",         // 键盘
    "cell phone",       // 手机
    "microwave",        // 微波炉
    "oven",             // 烤箱
    "toaster",          // 烤面包机
    "sink",             // 水槽
    "refrigerator",     // 冰箱
    "book",             // 书
    "clock",            // 时钟
    "vase",             // 花瓶
    "scissors",         // 剪刀
    "teddy bear",       // 泰迪熊
    "hair drier",       // 吹风机
    "toothbrush"        // 牙刷
};

#elif defined(CONFIG_COCO_POSE_MODEL_LOCATION)
const char *kpt_names[17] = {"nose",
                                "left eye",
                                "right eye",
                                "left ear",
                                "right ear",
                                "left shoulder",
                                "right shoulder",
                                "left elbow",
                                "right elbow",
                                "left wrist",
                                "right wrist",
                                "left hip",
                                "right hip",
                                "left knee",
                                "right knee",
                                "left ankle",
                                "right ankle"};

#endif

namespace who {
namespace detect {
void draw_detect_results_on_img(const dl::image::img_t &img,
                                const std::list<dl::detect::result_t> &detect_res,
                                const std::vector<std::vector<uint8_t>> &palette)
{
    for (const auto &res : detect_res) {
        if (res.keypoint.size() == 4) {
            continue;
        }
        size_t color_idx = res.category >= 0 && res.category < palette.size() ? res.category : 0;
        dl::image::draw_hollow_rectangle(img, res.box[0], res.box[1], res.box[2], res.box[3], palette[color_idx], 2);
    }
}

#if !BSP_CONFIG_NO_GRAPHIC_LIB
// Canvas draw tasks are retained until finish_layer(). Flush each object so
// crowded scenes cannot exhaust LVGL's fixed heap with a whole frame of tasks.
class CanvasLayerScope {
public:
    explicit CanvasLayerScope(lv_obj_t *canvas) : m_canvas(canvas)
    {
        lv_canvas_init_layer(m_canvas, &m_layer);
    }
    ~CanvasLayerScope() { lv_canvas_finish_layer(m_canvas, &m_layer); }
    CanvasLayerScope(const CanvasLayerScope &) = delete;
    CanvasLayerScope &operator=(const CanvasLayerScope &) = delete;
    lv_layer_t &get() { return m_layer; }

private:
    lv_obj_t *m_canvas;
    lv_layer_t m_layer;
};

static void draw_text(lv_layer_t *layer, lv_draw_label_dsc_t *label_dsc, int x, int y, const char *text)
{
    lv_point_t char_pos;
    char_pos.x = x;
    char_pos.y = y;

    const char *p = text;
    while (*p) {
        uint32_t letter = *p;
        uint32_t letter_next = *(p + 1);
        lv_draw_character(layer, label_dsc, &char_pos, letter);
        char_pos.x += lv_font_get_glyph_width(label_dsc->font, letter, letter_next);
        p++;
    }
}

static void draw_detect_results_on_canvas_impl(lv_obj_t *canvas,
                                               const std::list<dl::detect::result_t> &detect_res,
                                               const std::vector<lv_color_t> &palette,
                                               const std::vector<std::string> *category_labels
#if defined(CONFIG_HAND_GESTURE_CLS_MODEL_LOCATION)
                                               ,
                                               const std::vector<std::string> *recognition_results
#endif
)
{
    if (palette.empty()) {
        return;
    }
    lv_draw_rect_dsc_t rect_dsc;
    lv_draw_rect_dsc_init(&rect_dsc);
    rect_dsc.bg_opa = LV_OPA_TRANSP;
    rect_dsc.border_width = 2;

    lv_draw_arc_dsc_t arc_dsc;
    lv_draw_arc_dsc_init(&arc_dsc);
    arc_dsc.width = 3;
    arc_dsc.radius = 3;
    arc_dsc.start_angle = 0;
    arc_dsc.end_angle = 360;

    lv_draw_label_dsc_t label_dsc;
    lv_draw_label_dsc_init(&label_dsc);

    label_dsc.color = lv_color_hex(0xFF0000);
    label_dsc.font = &montserrat_bold_20;

    lv_area_t coords_rect;

    uint8_t count = 0;
    (void)count;
#if defined(CONFIG_HAND_GESTURE_CLS_MODEL_LOCATION)
    size_t recognition_idx = 0;
#endif

    for (const auto &res : detect_res) {
        if (res.box.size() < 4) {
            continue;
        }
#if defined(CONFIG_COCO_DETECT_MODEL_LOCATION)
        if (res.category < 0 || static_cast<size_t>(res.category) >= sizeof(COCO_CLASS_NAMES) / sizeof(COCO_CLASS_NAMES[0])) {
            continue;
        }
#endif
        CanvasLayerScope canvas_layer(canvas);
        lv_layer_t &layer = canvas_layer.get();
        size_t color_idx = res.category >= 0 && res.category < palette.size() ? res.category : 0;
#if defined(CONFIG_COCO_POSE_MODEL_LOCATION)
        (void)color_idx;
#endif
#if defined(CONFIG_COLOR_DETECT_MODEL)        
        if (res.keypoint.size() == 4) {
            lv_draw_line_dsc_t line_dsc;
            lv_draw_line_dsc_init(&line_dsc);
            line_dsc.color = palette[color_idx];
            line_dsc.width = 4;
            line_dsc.round_start = 1;
            line_dsc.round_end = 1;
            line_dsc.p1.x = res.keypoint[0];
            line_dsc.p1.y = res.keypoint[1];
            line_dsc.p2.x = res.keypoint[2];
            line_dsc.p2.y = res.keypoint[3];
            lv_draw_line(&layer, &line_dsc);
            continue;
        }
#endif
        coords_rect = {res.box[0], res.box[1], res.box[2], res.box[3]};

        rect_dsc.border_color = palette[color_idx];

        lv_draw_rect(&layer, &rect_dsc, &coords_rect);

#if defined(CONFIG_HAND_GESTURE_CLS_MODEL_LOCATION)
        if (recognition_results && recognition_idx < recognition_results->size()) {
            const auto &recognition_text = (*recognition_results)[recognition_idx];
            if (!recognition_text.empty()) {
                draw_text(&layer, &label_dsc, res.box[0] + 5, res.box[1] + 5, recognition_text.c_str());
            }
        }
        recognition_idx++;
#endif

#if defined(CONFIG_COLOR_DETECT_MODEL)
        char category_name[16];
        const char *category_text = category_name;
        if (category_labels && res.category >= 0 && res.category < category_labels->size()) {
            category_text = (*category_labels)[res.category].c_str();
        } else {
            snprintf(category_name, sizeof(category_name), "%d", res.category);
        }
        label_dsc.color = palette[color_idx];
        draw_text(&layer, &label_dsc, res.box[0] + 5, res.box[1] + 5, category_text);

#elif defined(CONFIG_HUMAN_FACE_DETECT_MODEL_LOCATION)
        lv_point_t char_pos;
        if (!res.keypoint.empty()) {
            arc_dsc.color = lv_color_hex(0xFE0000);
            assert(res.keypoint.size() == 10);
            for (int i = 0; i < 5; i++) {
                arc_dsc.center.x = res.keypoint[2 * i];
                arc_dsc.center.y = res.keypoint[2 * i + 1];
                lv_draw_arc(&layer, &arc_dsc);
            }
        }
        
        char score_text[16];
        snprintf(score_text, sizeof(score_text), "%.2f", res.score);
        char_pos.x = res.box[0] + 5;
        char_pos.y = res.box[1] + 5;
        const char* p = score_text;
        while (*p) {
            uint32_t letter = *p;
            uint32_t letter_next = *(p + 1);
            lv_draw_character(&layer, &label_dsc, &char_pos, letter);
            char_pos.x += lv_font_get_glyph_width(label_dsc.font, letter, letter_next);
            p++;
        }

#elif defined(CONFIG_CAT_DETECT_MODEL_LOCATION)
        lv_point_t char_pos;
        char score_text[16];
        snprintf(score_text, sizeof(score_text), "%.2f", res.score);
        char_pos.x = res.box[0] + 5;
        char_pos.y = res.box[1] + 5;
        const char* p = score_text;
        while (*p) {
            uint32_t letter = *p;
            uint32_t letter_next = *(p + 1);
            lv_draw_character(&layer, &label_dsc, &char_pos, letter);
            char_pos.x += lv_font_get_glyph_width(label_dsc.font, letter, letter_next);
            p++;
        }
#elif defined(CONFIG_DOG_DETECT_MODEL_LOCATION)
        lv_point_t char_pos;
        char score_text[16];
        snprintf(score_text, sizeof(score_text), "%.2f", res.score);
        char_pos.x = res.box[0] + 5;
        char_pos.y = res.box[1] + 5;
        const char* p = score_text;
        while (*p) {
            uint32_t letter = *p;
            uint32_t letter_next = *(p + 1);
            lv_draw_character(&layer, &label_dsc, &char_pos, letter);
            char_pos.x += lv_font_get_glyph_width(label_dsc.font, letter, letter_next);
            p++;
        }


#elif defined(CONFIG_PEDESTRIAN_DETECT_MODEL_LOCATION)
        lv_point_t char_pos;
        const char* category_name;
        category_name = "pedestrian";

        char_pos.x = res.box[0] + 5;
        char_pos.y = res.box[1] + 5;
        const char* p = category_name;
        while (*p) {
            uint32_t letter = *p;
            uint32_t letter_next = *(p + 1);
            lv_draw_character(&layer, &label_dsc, &char_pos, letter);
            char_pos.x += lv_font_get_glyph_width(label_dsc.font, letter, letter_next);
            p++;
        }

        char score_text[16];
        snprintf(score_text, sizeof(score_text), "%.2f", res.score);
        char_pos.x = res.box[0] + 5;
        char_pos.y = res.box[1] + 25;
        p = score_text;
        while (*p) {
            uint32_t letter = *p;
            uint32_t letter_next = *(p + 1);
            lv_draw_character(&layer, &label_dsc, &char_pos, letter);
            char_pos.x += lv_font_get_glyph_width(label_dsc.font, letter, letter_next);
            p++;
        }
        count++;
        if(count >= 2) {
            break;
        }

#elif defined(CONFIG_COCO_DETECT_MODEL_LOCATION)
        lv_point_t char_pos;
        const char* category_name;
        category_name = COCO_CLASS_NAMES[res.category];

        char_pos.x = res.box[0] + 5;
        char_pos.y = res.box[1] + 5;
        const char* p = category_name;
        while (*p) {
            uint32_t letter = *p;
            uint32_t letter_next = *(p + 1);
            lv_draw_character(&layer, &label_dsc, &char_pos, letter);
            char_pos.x += lv_font_get_glyph_width(label_dsc.font, letter, letter_next);
            p++;
        }
        char score_text[16];
        snprintf(score_text, sizeof(score_text), "%.2f", res.score);
        char_pos.x = res.box[0] + 5;
        char_pos.y = res.box[1] + 25;
        p = score_text;
        while (*p) {
            uint32_t letter = *p;
            uint32_t letter_next = *(p + 1);
            lv_draw_character(&layer, &label_dsc, &char_pos, letter);
            char_pos.x += lv_font_get_glyph_width(label_dsc.font, letter, letter_next);
            p++;
        }
        
        count++;
        if(count >= 2) {
            break;
        }
#elif defined(CONFIG_COCO_POSE_MODEL_LOCATION)
        lv_point_t char_pos;
        if (!res.keypoint.empty()) {
            arc_dsc.color = lv_color_hex(0xFE0000);
            if(res.keypoint.size() == 34) {
                // 绘制关键点，过滤掉原点(0,0)的点
                for (int i = 0; i < 17; i++) {
                    int x = res.keypoint[2 * i];
                    int y = res.keypoint[2 * i + 1];
                    // 过滤原点(0,0)的关键点
                    if (x != 0 || y != 0) {
                        arc_dsc.center.x = x;
                        arc_dsc.center.y = y;
                        lv_draw_arc(&layer, &arc_dsc);
                    }
                }

                lv_draw_line_dsc_t line_dsc;
                lv_draw_line_dsc_init(&line_dsc);
                line_dsc.color = lv_color_hex(0x0000FF);
                line_dsc.width = 2;
                line_dsc.round_start = 1;
                line_dsc.round_end = 1;

                // COCO 17关键点连线关系
                int connections[17][2] = {
                    {0, 1}, {0, 2},     // 鼻尖连接左右眼
                    {1, 3}, {2, 4},     // 眼睛连接耳朵
                    {1, 2},             // 双眼之间
                    {5, 6},             // 肩膀之间
                    {5, 7}, {7, 9},     // 左臂
                    {6, 8}, {8, 10},    // 右臂
                    {5, 11}, {6, 12},   // 肩膀连接髋部
                    {11, 12},           // 髋部之间
                    {11, 13}, {13, 15}, // 左腿
                    {12, 14}, {14, 16}, // 右腿
                };

                // 绘制连线，只有两个端点都不是原点时才绘制
                for (int i = 0; i < 17; i++) {
                    int idx1 = connections[i][0];
                    int idx2 = connections[i][1];
                    int x1 = res.keypoint[2 * idx1];
                    int y1 = res.keypoint[2 * idx1 + 1];
                    int x2 = res.keypoint[2 * idx2];
                    int y2 = res.keypoint[2 * idx2 + 1];
                    
                    // 只有两个点都不是原点(0,0)时才绘制连线
                    if ((x1 != 0 || y1 != 0) && (x2 != 0 || y2 != 0)) {
                        line_dsc.p1.x = x1;
                        line_dsc.p1.y = y1;
                        line_dsc.p2.x = x2;
                        line_dsc.p2.y = y2;
                        lv_draw_line(&layer, &line_dsc);
                    }
                }
            }

        }
        char score_text[16];
        snprintf(score_text, sizeof(score_text), "%.2f", res.score);
        char_pos.x = res.box[0] + 5;
        char_pos.y = res.box[1] + 5;
        const char* p = score_text;
        while (*p) {
            uint32_t letter = *p;
            uint32_t letter_next = *(p + 1);
            lv_draw_character(&layer, &label_dsc, &char_pos, letter);
            char_pos.x += lv_font_get_glyph_width(label_dsc.font, letter, letter_next);
            p++;
        }
#endif


    }
}

void draw_detect_results_on_canvas(lv_obj_t *canvas,
                                   const std::list<dl::detect::result_t> &detect_res,
                                   const std::vector<lv_color_t> &palette,
                                   const std::vector<std::string> &category_labels)
{
    draw_detect_results_on_canvas_impl(canvas, detect_res, palette, &category_labels
#if defined(CONFIG_HAND_GESTURE_CLS_MODEL_LOCATION)
                                       ,
                                       nullptr
#endif
    );
}

#if defined(CONFIG_HAND_GESTURE_CLS_MODEL_LOCATION)
void draw_recognition_results_on_canvas(lv_obj_t *canvas,
                                        const std::list<dl::detect::result_t> &detect_res,
                                        const std::vector<lv_color_t> &palette,
                                        const std::vector<std::string> &recognition_results)
{
    draw_detect_results_on_canvas_impl(canvas, detect_res, palette, nullptr, &recognition_results);
}
#endif
#endif

void print_detect_results(const std::list<dl::detect::result_t> &detect_res)
{
    (void)detect_res;
}
} // namespace detect

namespace lcd_disp {
#if !BSP_CONFIG_NO_GRAPHIC_LIB
WhoDetectResultLCDDisp::WhoDetectResultLCDDisp(task::WhoTask *task,
                                               lv_obj_t *canvas,
                                               const std::vector<std::vector<uint8_t>> &palette) :
    m_task(task), m_res_mutex(xSemaphoreCreateMutex()), m_result(), m_canvas(canvas)
{
    m_palette = cvt_to_lv_palette(palette);
}
#else
WhoDetectResultLCDDisp::WhoDetectResultLCDDisp(task::WhoTask *task, const std::vector<std::vector<uint8_t>> &palette) :
    m_task(task),
    m_res_mutex(xSemaphoreCreateMutex()),
    m_result(),
    m_rgb888_palette(palette),
    m_rgb565_palette(palette.size(), std::vector<uint8_t>(2))
{
    uint32_t caps = 0;
    for (int i = 0; i < m_rgb888_palette.size(); i++) {
        dl::image::cvt_pix(m_rgb888_palette[i].data(),
                           m_rgb565_palette[i].data(),
                           dl::image::DL_IMAGE_PIX_TYPE_RGB888,
                           dl::image::DL_IMAGE_PIX_TYPE_RGB565LE,
                           caps);
    }
}
#endif

WhoDetectResultLCDDisp::~WhoDetectResultLCDDisp()
{
    vSemaphoreDelete(m_res_mutex);
}

void WhoDetectResultLCDDisp::set_palette(const std::vector<std::vector<uint8_t>> &palette)
{
    xSemaphoreTake(m_res_mutex, portMAX_DELAY);
#if BSP_CONFIG_NO_GRAPHIC_LIB
    m_rgb888_palette = palette;
    m_rgb565_palette.assign(palette.size(), std::vector<uint8_t>(2));
    uint32_t caps = 0;
    for (int i = 0; i < m_rgb888_palette.size(); i++) {
        dl::image::cvt_pix(m_rgb888_palette[i].data(),
                           m_rgb565_palette[i].data(),
                           dl::image::DL_IMAGE_PIX_TYPE_RGB888,
                           dl::image::DL_IMAGE_PIX_TYPE_RGB565LE,
                           caps);
    }
#else
    m_palette = cvt_to_lv_palette(palette);
#endif
    xSemaphoreGive(m_res_mutex);
}

void WhoDetectResultLCDDisp::set_category_labels(const std::vector<std::string> &category_labels)
{
    xSemaphoreTake(m_res_mutex, portMAX_DELAY);
    m_category_labels = category_labels;
    xSemaphoreGive(m_res_mutex);
}

void WhoDetectResultLCDDisp::save_detect_result(const detect::WhoDetect::result_t &result)
{
    xSemaphoreTake(m_res_mutex, portMAX_DELAY);
    while (m_results.size() >= MAX_PENDING_RESULTS) {
        m_results.pop();
    }
    m_results.push(result);
    xSemaphoreGive(m_res_mutex);
}

#if defined(CONFIG_HAND_GESTURE_CLS_MODEL_LOCATION)
void WhoDetectResultLCDDisp::save_recognition_result(const detect::WhoDetect::result_t &detect_result,
                                                     const std::vector<std::string> &recognition_results)
{
    xSemaphoreTake(m_res_mutex, portMAX_DELAY);
    while (m_recognition_results.size() >= MAX_PENDING_RESULTS) {
        m_recognition_results.pop();
    }
    m_recognition_results.push({detect_result, recognition_results});
    xSemaphoreGive(m_res_mutex);
}
#endif

void WhoDetectResultLCDDisp::lcd_disp_cb(who::cam::cam_fb_t *fb)
{
    if (!m_task->is_active()) {

        return;
    }
    xSemaphoreTake(m_res_mutex, portMAX_DELAY);
    // Try to sync camera frame and result, skip the future result.
    auto compare_timestamp = [](const struct timeval &t1, const struct timeval &t2) -> bool {
        if (t1.tv_sec == t2.tv_sec) {
            return t1.tv_usec < t2.tv_usec;
        }
        return t1.tv_sec < t2.tv_sec;
    };
    struct timeval t1 = fb->timestamp;
    detect::WhoDetect::result_t display_result;
#if BSP_CONFIG_NO_GRAPHIC_LIB
    std::vector<std::vector<uint8_t>> rgb888_palette;
    std::vector<std::vector<uint8_t>> rgb565_palette;
#else
    std::vector<lv_color_t> palette;
    std::vector<std::string> category_labels;
#if defined(CONFIG_HAND_GESTURE_CLS_MODEL_LOCATION)
    std::vector<std::string> recognition_result;
#endif
#endif
    // If detect fps higher than display fps, the result queue may be more than 1. May happen when using lvgl.
    while (!m_results.empty()) {
        auto &result = m_results.front();
        if (!compare_timestamp(t1, result.timestamp)) {
            m_result = std::move(result);
            m_results.pop();
        } else {
            break;
        }
    }
#if defined(CONFIG_HAND_GESTURE_CLS_MODEL_LOCATION)
    while (!m_recognition_results.empty()) {
        auto &result = m_recognition_results.front();
        if (!compare_timestamp(t1, result.detect_result.timestamp)) {
            m_result = std::move(result.detect_result);
            m_recognition_result = std::move(result.recognition_results);
            m_recognition_results.pop();
        } else {
            break;
        }
    }
#endif
#if BSP_CONFIG_NO_GRAPHIC_LIB
    rgb888_palette = m_rgb888_palette;
    rgb565_palette = m_rgb565_palette;
#else
    palette = m_palette;
    category_labels = m_category_labels;
#if defined(CONFIG_HAND_GESTURE_CLS_MODEL_LOCATION)
    recognition_result = m_recognition_result;
#endif
#endif
    // Cleanup may run when only the detection core is paused by Yield2Idle.
    // Keep a local snapshot alive until drawing finishes after releasing the lock.
    display_result = m_result;
    xSemaphoreGive(m_res_mutex);
#if BSP_CONFIG_NO_GRAPHIC_LIB
    if (fb->format == cam::cam_fb_fmt_t::CAM_FB_FMT_RGB565) {
        detect::draw_detect_results_on_img(*fb, display_result.det_res, rgb565_palette);
    } else if (fb->format == cam::cam_fb_fmt_t::CAM_FB_FMT_RGB888) {
        detect::draw_detect_results_on_img(*fb, display_result.det_res, rgb888_palette);
    }
#else
#if defined(CONFIG_HAND_GESTURE_CLS_MODEL_LOCATION)
    detect::draw_recognition_results_on_canvas(m_canvas, display_result.det_res, palette, recognition_result);
#else
    detect::draw_detect_results_on_canvas(m_canvas, display_result.det_res, palette, category_labels);
#endif
#endif
}

void WhoDetectResultLCDDisp::cleanup()
{
    xSemaphoreTake(m_res_mutex, portMAX_DELAY);
    std::queue<detect::WhoDetect::result_t>().swap(m_results);
#if defined(CONFIG_HAND_GESTURE_CLS_MODEL_LOCATION)
    std::queue<recognition_result_t>().swap(m_recognition_results);
#endif
    m_result = {};
#if defined(CONFIG_HAND_GESTURE_CLS_MODEL_LOCATION)
    m_recognition_result = {};
#endif
    xSemaphoreGive(m_res_mutex);
}
} // namespace lcd_disp
} // namespace who
