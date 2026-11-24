#include "color_detect.hpp"

#include <algorithm>
#include <cstring>

static const char *TAG = "ColorDetect";

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
} // namespace

ColorDetect::ColorDetect(uint16_t width, uint16_t height) :
    m_src_img(),
    m_width(width),
    m_height(height),
    m_hsv_mask(heap_caps_malloc(width * height, MALLOC_CAP_DEFAULT)),
    m_mask_label(heap_caps_malloc(width * height * 2, MALLOC_CAP_DEFAULT)),
    m_inv_scale_x(0),
    m_inv_scale_y(0),
    m_x(nullptr),
    m_y(nullptr),
    m_gen_xy_map(false),
    m_morphology(false),
    m_morph_kernel_size(0),
    m_morph_buffer(nullptr),
    m_cc_queue(static_cast<uint32_t *>(
        heap_caps_malloc(static_cast<size_t>(width) * height * sizeof(uint32_t),
                         MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT))),
    m_mutex(xSemaphoreCreateMutex())
{
    assert(m_mutex);
    if (!m_cc_queue) {
        m_cc_queue = static_cast<uint32_t *>(
            heap_caps_malloc(static_cast<size_t>(width) * height * sizeof(uint32_t), MALLOC_CAP_DEFAULT));
    }
    assert(m_cc_queue);
}

ColorDetect::~ColorDetect()
{
    auto check_and_free = [](void *ptr) {
        if (ptr) {
            heap_caps_free(ptr);
        }
    };
    check_and_free(m_x);
    check_and_free(m_y);
    check_and_free(m_morph_buffer);
    check_and_free(m_cc_queue);
    heap_caps_free(m_hsv_mask);
    heap_caps_free(m_mask_label);
    vSemaphoreDelete(m_mutex);
}

void ColorDetect::register_color(const std::vector<uint8_t> &hsv_min,
                                 const std::vector<uint8_t> &hsv_max,
                                 const std::string &name,
                                 int area_thr,
                                 const std::vector<uint8_t> &box_color)
{
    MutexGuard lock(m_mutex);
    assert(hsv_min.size() == 3 && hsv_max.size() == 3 && hsv_min[0] <= 180 && hsv_max[0] <= 180 &&
           hsv_min[1] < hsv_max[1] && hsv_min[2] < hsv_max[2] && area_thr < m_width * m_height);
    assert(box_color.empty() || box_color.size() == 3);
    if (std::find(m_color_names.begin(), m_color_names.end(), name) != m_color_names.end()) {
        ESP_LOGE(TAG, "Color %s is already registered.", name.c_str());
        return;
    }
    m_hsv_min.emplace_back(hsv_min);
    m_hsv_max.emplace_back(hsv_max);
    m_box_colors.emplace_back(box_color.empty() ? std::vector<uint8_t>{255, 165, 0} : box_color);
    m_area_thr.emplace_back(area_thr);
    m_color_names.emplace_back(name);
}

void ColorDetect::clear_colors()
{
    MutexGuard lock(m_mutex);
    m_hsv_min.clear();
    m_hsv_max.clear();
    m_box_colors.clear();
    m_area_thr.clear();
    m_color_names.clear();
}

void ColorDetect::delete_color(int idx)
{
    MutexGuard lock(m_mutex);
    assert(idx >= 0 && idx < m_hsv_min.size());
    m_hsv_min.erase(m_hsv_min.begin() + idx);
    m_hsv_max.erase(m_hsv_max.begin() + idx);
    m_box_colors.erase(m_box_colors.begin() + idx);
    m_area_thr.erase(m_area_thr.begin() + idx);
    m_color_names.erase(m_color_names.begin() + idx);
}

void ColorDetect::delete_color(const std::string &name)
{
    MutexGuard lock(m_mutex);
    auto it = std::find(m_color_names.begin(), m_color_names.end(), name);
    if (it == m_color_names.end()) {
        ESP_LOGE(TAG, "Failed to delete Color %s, it is not registered.", name.c_str());
        return;
    }
    int idx = std::distance(m_color_names.begin(), it);
    m_hsv_min.erase(m_hsv_min.begin() + idx);
    m_hsv_max.erase(m_hsv_max.begin() + idx);
    m_box_colors.erase(m_box_colors.begin() + idx);
    m_area_thr.erase(m_area_thr.begin() + idx);
    m_color_names.erase(m_color_names.begin() + idx);
}

void ColorDetect::enable_morphology(int kernel_size)
{
    MutexGuard lock(m_mutex);
    if (kernel_size < 1) {
        kernel_size = 1;
    }
    if ((kernel_size & 1) == 0) {
        kernel_size++;
    }
    kernel_size = std::min(kernel_size, 15);
    if (!m_morph_buffer) {
        m_morph_buffer = static_cast<uint8_t *>(
            heap_caps_malloc(static_cast<size_t>(m_width) * m_height, MALLOC_CAP_DEFAULT));
    }
    if (!m_morph_buffer) {
        ESP_LOGE(TAG, "No memory for the morphology buffer");
        m_morphology = false;
        return;
    }
    m_morph_kernel_size = kernel_size;
    m_morphology = true;
}

void ColorDetect::morph(bool erode)
{
    if (!m_morph_buffer || m_morph_kernel_size <= 0) {
        return;
    }
    const uint8_t *src = static_cast<const uint8_t *>(m_hsv_mask);
    uint8_t *dst = m_morph_buffer;
    const int radius = m_morph_kernel_size / 2;
    for (int y = 0; y < m_height; y++) {
        for (int x = 0; x < m_width; x++) {
            bool hit = erode;
            for (int dy = -radius; dy <= radius && hit == erode; dy++) {
                for (int dx = -radius; dx <= radius; dx++) {
                    const int nx = x + dx;
                    const int ny = y + dy;
                    const bool inside = nx >= 0 && nx < m_width && ny >= 0 && ny < m_height;
                    const bool set = inside && src[ny * m_width + nx] != 0;
                    if (erode && !set) {
                        hit = false;
                        break;
                    }
                    if (!erode && set) {
                        hit = true;
                        break;
                    }
                }
            }
            dst[y * m_width + x] = hit ? 0xFF : 0;
        }
    }
    memcpy(m_hsv_mask, m_morph_buffer, static_cast<size_t>(m_width) * m_height);
}

void ColorDetect::collect_components(int category, int area_threshold)
{
    const uint8_t *mask = static_cast<const uint8_t *>(m_hsv_mask);
    uint8_t *visited = static_cast<uint8_t *>(m_mask_label);
    const size_t pixels = static_cast<size_t>(m_width) * m_height;
    memset(visited, 0, pixels);

    for (int start_y = 0; start_y < m_height; start_y++) {
        for (int start_x = 0; start_x < m_width; start_x++) {
            const int start = start_y * m_width + start_x;
            if (mask[start] == 0 || visited[start]) {
                continue;
            }

            int head = 0;
            int tail = 0;
            m_cc_queue[tail++] = static_cast<uint32_t>(start);
            visited[start] = 1;
            int min_x = start_x;
            int max_x = start_x;
            int min_y = start_y;
            int max_y = start_y;
            int area = 0;

            while (head < tail) {
                const uint32_t idx = m_cc_queue[head++];
                const int x = static_cast<int>(idx % m_width);
                const int y = static_cast<int>(idx / m_width);
                area++;
                min_x = std::min(min_x, x);
                max_x = std::max(max_x, x);
                min_y = std::min(min_y, y);
                max_y = std::max(max_y, y);

                for (int dy = -1; dy <= 1; dy++) {
                    for (int dx = -1; dx <= 1; dx++) {
                        if (dx == 0 && dy == 0) {
                            continue;
                        }
                        const int nx = x + dx;
                        const int ny = y + dy;
                        if (nx < 0 || nx >= m_width || ny < 0 || ny >= m_height) {
                            continue;
                        }
                        const int nidx = ny * m_width + nx;
                        if (mask[nidx] != 0 && !visited[nidx]) {
                            visited[nidx] = 1;
                            m_cc_queue[tail++] = static_cast<uint32_t>(nidx);
                        }
                    }
                }
            }

            if (area > area_threshold) {
                const int x1 = static_cast<int>(min_x * m_inv_scale_x);
                const int y1 = static_cast<int>(min_y * m_inv_scale_y);
                const int x2 = static_cast<int>((max_x + 1) * m_inv_scale_x);
                const int y2 = static_cast<int>((max_y + 1) * m_inv_scale_y);
                m_result.push_back({category, 1.f, {x1, y1, x2, y2}, {}});
            }
        }
    }
}

std::string ColorDetect::get_color_name(int idx)
{
    MutexGuard lock(m_mutex);
    assert(idx >= 0 && idx < m_hsv_min.size());
    return m_color_names[idx];
}

void ColorDetect::gen_xy_map()
{
    const int col_step = (m_src_img.pix_type == dl::image::DL_IMAGE_PIX_TYPE_RGB888) ? 3 : 2;
    const int row_step = m_src_img.width * col_step;

    auto check_and_free = [](void *ptr) {
        if (ptr) {
            heap_caps_free(ptr);
        }
    };
    check_and_free(m_x);
    check_and_free(m_y);

    m_x = (int *)heap_caps_malloc(m_width * sizeof(int), MALLOC_CAP_DEFAULT);
    m_y = (int *)heap_caps_malloc(m_height * sizeof(int), MALLOC_CAP_DEFAULT);
    int src_width = m_src_img.width;
    int src_height = m_src_img.height;
    m_inv_scale_x = static_cast<float>(src_width) / m_width;
    m_inv_scale_y = static_cast<float>(src_height) / m_height;
    for (int i = 0; i < m_width; i++) {
        m_x[i] = std::min(static_cast<int>(i * m_inv_scale_x), src_width - 1) * col_step;
    }
    for (int i = 0; i < m_height; i++) {
        m_y[i] = std::min(static_cast<int>(i * m_inv_scale_y), src_height - 1) * row_step;
    }
}

void ColorDetect::make_hsv_mask(size_t color_index)
{
    const auto &hsv_min = m_hsv_min[color_index];
    const auto &hsv_max = m_hsv_max[color_index];
    const bool h_across_zero = hsv_min[0] > hsv_max[0];
    const bool rgb888 = m_src_img.pix_type == dl::image::DL_IMAGE_PIX_TYPE_RGB888;
    const bool rgb565_be = m_src_img.pix_type == dl::image::DL_IMAGE_PIX_TYPE_RGB565BE;
    const uint8_t *src = static_cast<const uint8_t *>(m_src_img.data);
    uint8_t *mask = static_cast<uint8_t *>(m_hsv_mask);

    for (int y = 0; y < m_height; y++) {
        const uint8_t *row = src + m_y[y];
        for (int x = 0; x < m_width; x++) {
            uint8_t r, g, b;
            if (rgb888) {
                const uint8_t *pixel = row + m_x[x];
                r = pixel[0];
                g = pixel[1];
                b = pixel[2];
            } else {
                uint16_t value;
                memcpy(&value, row + m_x[x], sizeof(value));
                if (rgb565_be) {
                    value = static_cast<uint16_t>((value >> 8) | (value << 8));
                }
                r = static_cast<uint8_t>(((value >> 11) & 0x1F) * 255 / 31);
                g = static_cast<uint8_t>(((value >> 5) & 0x3F) * 255 / 63);
                b = static_cast<uint8_t>((value & 0x1F) * 255 / 31);
            }

            const uint8_t max_value = std::max({r, g, b});
            const uint8_t min_value = std::min({r, g, b});
            const uint8_t delta = max_value - min_value;
            const uint8_t value = max_value;
            const uint8_t saturation = max_value ? static_cast<uint8_t>(delta * 255u / max_value) : 0;

            int hue360 = 0;
            if (delta != 0) {
                if (max_value == r) {
                    hue360 = 60 * (static_cast<int>(g) - b) / delta;
                } else if (max_value == g) {
                    hue360 = 120 + 60 * (static_cast<int>(b) - r) / delta;
                } else {
                    hue360 = 240 + 60 * (static_cast<int>(r) - g) / delta;
                }
                if (hue360 < 0) {
                    hue360 += 360;
                }
            }
            const uint8_t hue = static_cast<uint8_t>(hue360 / 2);

            const bool hue_match = h_across_zero ? (hue >= hsv_min[0] || hue <= hsv_max[0])
                                                 : (hue >= hsv_min[0] && hue <= hsv_max[0]);
            mask[y * m_width + x] =
                (hue_match && saturation >= hsv_min[1] && saturation <= hsv_max[1] && value >= hsv_min[2] &&
                 value <= hsv_max[2])
                    ? 0xFF
                    : 0;
        }
    }
}

std::list<dl::detect::result_t> &ColorDetect::run(const dl::image::img_t &img)
{
    MutexGuard lock(m_mutex);
    m_result.clear();
    if (m_hsv_min.empty() || m_hsv_max.empty()) {
        ESP_LOGE(TAG, "No color is registered. Please call register_color() first.");
        return m_result;
    }
    const bool supported = img.pix_type == dl::image::DL_IMAGE_PIX_TYPE_RGB888 ||
                           img.pix_type == dl::image::DL_IMAGE_PIX_TYPE_RGB565LE ||
                           img.pix_type == dl::image::DL_IMAGE_PIX_TYPE_RGB565BE;
    if (!img.data || !img.height || !img.width || !supported) {
        ESP_LOGE(TAG, "Invalid src img.");
        return m_result;
    }
    if (img.width != m_src_img.width || img.height != m_src_img.height || img.pix_type != m_src_img.pix_type) {
        m_gen_xy_map = true;
    }
    m_src_img = img;
    if (m_gen_xy_map) {
        gen_xy_map();
        m_gen_xy_map = false;
    }

    for (size_t i = 0; i < m_hsv_min.size(); i++) {
        make_hsv_mask(i);
        if (m_morphology) {
            morph(true);
            morph(false);
            morph(false);
            morph(true);
        }
        collect_components(static_cast<int>(i), m_area_thr[i]);
    }
    for (auto &res : m_result) {
        res.limit_box(img.width, img.height);
    }
    return m_result;
}
