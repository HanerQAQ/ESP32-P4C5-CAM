#include "frame_cap_pipeline.hpp"
#include "color_line_i2c_protocol.hpp"
#include "color_detect.hpp"
#include "line_regression_detect.hpp"
#include "vision_host_protocol.hpp"
#include "who_i2c_slave_task.hpp"
#include "who_ci1302_task.hpp"
#include "who_detect_app_lcd.hpp"
#include "who_communicate_app_term.hpp"
#include "who_frame_lcd_disp.hpp"
#include "who_web.hpp"
#include "who_web_detect_json.hpp"
#include "nvs.h"
#include "nvs_flash.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_check.h"
#include <algorithm>
#include <cassert>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

static const char *TAG = "app_main";

static const char *NVS_NAMESPACE = "vision_cfg";
static const char *NVS_KEY = "settings";

using namespace who::frame_cap;
using namespace who::app;
using namespace who::communicate;
using namespace who::lcd_disp;

namespace {
constexpr int MAX_COLORS = 5;
constexpr int MAX_TARGETS = 5;

class VisionModeDetect : public dl::detect::Detect {
public:
    VisionModeDetect(ColorDetect *color_model, LineRegressionDetect *line_model) :
        m_color_model(color_model),
        m_line_model(line_model),
        m_mode(0),
        m_mutex(xSemaphoreCreateMutex())
    {
        assert(m_color_model);
        assert(m_line_model);
        assert(m_mutex);
    }

    ~VisionModeDetect()
    {
        delete m_color_model;
        delete m_line_model;
        vSemaphoreDelete(m_mutex);
    }

    void update(int mode, const std::function<void(ColorDetect *, LineRegressionDetect *)> &updater)
    {
        xSemaphoreTake(m_mutex, portMAX_DELAY);
        updater(m_color_model, m_line_model);
        m_mode = mode;
        xSemaphoreGive(m_mutex);
    }

    std::list<dl::detect::result_t> &run(const dl::image::img_t &img) override
    {
        xSemaphoreTake(m_mutex, portMAX_DELAY);
        std::list<dl::detect::result_t> &result =
            m_mode == 1 ? m_line_model->run(img) : m_color_model->run(img);
        xSemaphoreGive(m_mutex);
        return result;
    }

    Detect &set_score_thr(float score_thr, int idx) override { return *this; }
    Detect &set_nms_thr(float nms_thr, int idx) override { return *this; }
    dl::Model *get_raw_model(int idx) override { return nullptr; }

private:
    ColorDetect *m_color_model;
    LineRegressionDetect *m_line_model;
    int m_mode;
    SemaphoreHandle_t m_mutex;
};

SemaphoreHandle_t g_config_mutex;
VisionConfig g_config;

static const char *color_name_str(uint8_t id)
{
    switch (id) {
        case 1: return "Red";
        case 2: return "Blue";
        case 3: return "Green";
        case 4: return "Yellow";
        case 5: return "Purple";
        default: return "Color";
    }
}

int selected_color_index(const VisionConfig &cfg);

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

class VisionDetectAppLCD : public WhoDetectAppLCD {
public:
    VisionDetectAppLCD(const std::vector<std::vector<uint8_t>> &palette,
                       WhoFrameCap *frame_cap,
                       WhoFrameCapNode *lcd_disp_frame_cap_node = nullptr) :
        WhoDetectAppLCD(palette, frame_cap, lcd_disp_frame_cap_node),
        m_result_mutex(xSemaphoreCreateMutex())
    {
        assert(m_result_mutex);
    }

    ~VisionDetectAppLCD()
    {
        vSemaphoreDelete(m_result_mutex);
    }

    who::detect::WhoDetect::result_t get_result()
    {
        MutexGuard lock(m_result_mutex);
        return m_last_result;
    }

protected:
    void detect_result_cb(const who::detect::WhoDetect::result_t &result) override
    {
        WhoDetectAppLCD::detect_result_cb(result);
        {
            MutexGuard lock(m_result_mutex);
            m_last_result = result;
        }

        bool line_mode = false;
        std::string line_label = "line";
        std::vector<std::string> labels;
        {
            MutexGuard lock(g_config_mutex);
            line_mode = g_config.mode == 1;
            if (line_mode) {
                const int idx = selected_color_index(g_config);
                if (idx >= 0) {
                    line_label = "line " + std::string(color_name_str(g_config.colors[idx].name));
                }
            } else {
                labels.reserve(g_config.target_count);
                for (int i = 0; i < g_config.target_count; i++) {
                    labels.push_back(color_name_str(g_config.targets[i]));
                }
            }
        }

        std::string objects = line_mode
                                  ? who::web::json::line_objects(result, line_label, BSP_LCD_H_RES)
                                  : who::web::json::detect_objects(result, labels);
        who::web::WhoWeb::get_instance().set_objects_json(std::move(objects));
    }

private:
    SemaphoreHandle_t m_result_mutex;
    who::detect::WhoDetect::result_t m_last_result;
};

VisionConfig default_config()
{
    VisionConfig cfg = {};
    cfg.mode = 0;
    cfg.target_count = 4;
    cfg.targets[0] = 1; // Red
    cfg.targets[1] = 2; // Blue
    cfg.targets[2] = 3; // Green
    cfg.targets[3] = 4; // Yellow
    cfg.color_count = 5;
    // 1: Red: Hue across 0: 160..10, Sat: 60..255, Val: 45..255, box: [255, 0, 0]
    cfg.colors[0] = {1, {160, 60, 45}, {10, 255, 255}, {255, 0, 0}, 150};
    // 2: Blue: Hue: 90..130, Sat: 50..255, Val: 40..255, box: [0, 0, 255]
    cfg.colors[1] = {2, {90, 50, 40}, {130, 255, 255}, {0, 0, 255}, 150};
    // 3: Green: Hue: 35..85, Sat: 50..255, Val: 40..255, box: [0, 255, 0]
    cfg.colors[2] = {3, {35, 50, 40}, {85, 255, 255}, {0, 255, 0}, 150};
    // 4: Yellow: Hue: 15..35, Sat: 60..255, Val: 60..255, box: [255, 255, 0]
    cfg.colors[3] = {4, {15, 60, 60}, {35, 255, 255}, {255, 255, 0}, 150};
    // 5: Purple: Hue: 135..160, Sat: 50..255, Val: 40..255, box: [255, 0, 255]
    cfg.colors[4] = {5, {135, 50, 40}, {160, 255, 255}, {255, 0, 255}, 150};
    return cfg;
}

bool init_nvs()
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    if (err != ESP_OK) {
        return false;
    }
    return true;
}

bool load_config_from_nvs(VisionConfig *cfg)
{
    if (!cfg) {
        return false;
    }
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        return false;
    }
    size_t len = sizeof(VisionConfig);
    err = nvs_get_blob(handle, NVS_KEY, cfg, &len);
    nvs_close(handle);
    return err == ESP_OK && len == sizeof(VisionConfig);
}

bool save_config_to_nvs(const VisionConfig &cfg)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return false;
    }
    err = nvs_set_blob(handle, NVS_KEY, &cfg, sizeof(cfg));
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err == ESP_OK;
}

int find_color_index_by_name(const VisionConfig &cfg, uint8_t name);

int find_color_index_by_name(const VisionConfig &cfg, uint8_t name)
{
    for (int i = 0; i < cfg.color_count; i++) {
        if (cfg.colors[i].name == name) {
            return i;
        }
    }
    return -1;
}

bool is_config_valid(const VisionConfig &cfg)
{
    if (cfg.mode < 0 || cfg.mode > 1 || cfg.color_count == 0 || cfg.color_count > MAX_COLORS ||
        cfg.target_count == 0 || cfg.target_count > MAX_TARGETS ||
        (cfg.mode == 1 && cfg.target_count != 1)) {
        return false;
    }

    bool color_names[MAX_COLORS + 1] = {};
    for (int i = 0; i < cfg.color_count; i++) {
        const auto &color = cfg.colors[i];
        if (color.name < 1 || color.name > MAX_COLORS || color_names[color.name] ||
            color.hsv_min[0] > 180 || color.hsv_max[0] > 180 ||
            color.hsv_min[1] >= color.hsv_max[1] || color.hsv_min[2] >= color.hsv_max[2] ||
            color.area_thr <= 0 || color.area_thr >= MODEL_INPUT_W * MODEL_INPUT_H) {
            return false;
        }
        color_names[color.name] = true;
    }

    bool targets[MAX_COLORS + 1] = {};
    for (int i = 0; i < cfg.target_count; i++) {
        const uint8_t target = cfg.targets[i];
        if (target < 1 || target > MAX_COLORS || targets[target] || find_color_index_by_name(cfg, target) < 0) {
            return false;
        }
        targets[target] = true;
    }
    return true;
}

void log_current_config(const VisionConfig &cfg)
{
    std::string targets;
    for (int i = 0; i < cfg.target_count; i++) {
        if (!targets.empty()) {
            targets += ',';
        }
        targets += std::to_string(cfg.targets[i]);
    }

    const char *mode_name = cfg.mode == 1 ? "line_tracking" : "color_detect";
    ESP_LOGI(TAG,
             "Current vision config: mode=%ld (%s), tracking_color_indices=[%s]",
             static_cast<long>(cfg.mode),
             mode_name,
             targets.c_str());
}

int selected_color_index(const VisionConfig &cfg)
{
    uint8_t name = cfg.target_count > 0 ? cfg.targets[0] : 1;
    int idx = find_color_index_by_name(cfg, name);
    return idx >= 0 ? idx : (cfg.color_count > 0 ? 0 : -1);
}

bool append_registered_color(ColorDetect *color_model, const VisionConfig &cfg, uint8_t name)
{
    int idx = find_color_index_by_name(cfg, name);
    if (idx < 0) {
        return false;
    }

    const auto &color = cfg.colors[idx];
    color_model->register_color({color.hsv_min[0], color.hsv_min[1], color.hsv_min[2]},
                                {color.hsv_max[0], color.hsv_max[1], color.hsv_max[2]},
                                color_name_str(color.name),
                                color.area_thr,
                                {color.box_color[0], color.box_color[1], color.box_color[2]});
    return true;
}

void apply_config_to_model(VisionModeDetect *model, const VisionConfig &cfg)
{
    if (!model || !is_config_valid(cfg)) {
        ESP_LOGE(TAG, "Refusing to apply invalid vision configuration");
        return;
    }

    model->update(cfg.mode, [&cfg](ColorDetect *color_model, LineRegressionDetect *line_model) {
        color_model->clear_colors();
        for (int i = 0; i < cfg.target_count; i++) {
            bool registered = append_registered_color(color_model, cfg, cfg.targets[i]);
            assert(registered);
        }

        int line_color_idx = selected_color_index(cfg);
        if (line_color_idx >= 0) {
            const auto &line = cfg.colors[line_color_idx];
            line_model->set_hsv_threshold({line.hsv_min[0], line.hsv_min[1], line.hsv_min[2]},
                                          {line.hsv_max[0], line.hsv_max[1], line.hsv_max[2]});
            line_model->set_min_pixels(line.area_thr);
            line_model->set_roi(0.45f, 1.0f);
            line_model->set_stride(2, 2);
        }
    });
}

std::vector<std::vector<uint8_t>> palette_from_config(const VisionConfig &cfg)
{
    if (cfg.mode == 1) {
        int line_color_idx = selected_color_index(cfg);
        if (line_color_idx >= 0) {
            const auto &color = cfg.colors[line_color_idx];
            return {{color.box_color[0], color.box_color[1], color.box_color[2]}};
        }
    }

    std::vector<std::vector<uint8_t>> palette;
    palette.reserve(cfg.target_count);
    for (int i = 0; i < cfg.target_count; i++) {
        int idx = find_color_index_by_name(cfg, cfg.targets[i]);
        if (idx >= 0) {
            const auto &color = cfg.colors[idx];
            palette.push_back({color.box_color[0], color.box_color[1], color.box_color[2]});
        }
    }
    return palette.empty() ? std::vector<std::vector<uint8_t>>{{255, 165, 0}} : palette;
}

std::vector<std::string> category_labels_from_config(const VisionConfig &cfg)
{
    std::vector<std::string> labels;
    if (cfg.mode == 1) {
        if (cfg.target_count > 0) {
            labels.push_back(color_name_str(cfg.targets[0]));
        }
        return labels;
    }

    labels.reserve(cfg.target_count);
    for (int i = 0; i < cfg.target_count; i++) {
        labels.push_back(color_name_str(cfg.targets[i]));
    }
    return labels;
}

bool save_host_config(VisionModeDetect *model, WhoDetectAppLCD *detect_app, const VisionConfig &config)
{
    if (!is_config_valid(config) || !save_config_to_nvs(config)) {
        return false;
    }

    VisionConfig loaded = {};
    if (!load_config_from_nvs(&loaded) || memcmp(&config, &loaded, sizeof(config)) != 0) {
        return false;
    }

    xSemaphoreTake(g_config_mutex, portMAX_DELAY);
    g_config = loaded;
    xSemaphoreGive(g_config_mutex);
    apply_config_to_model(model, loaded);
    if (detect_app) {
        detect_app->set_palette(palette_from_config(loaded));
        detect_app->set_category_labels(category_labels_from_config(loaded));
    }
    return true;
}

bool apply_vision_setting(VisionModeDetect *model,
                          WhoDetectAppLCD *detect_app,
                          uint8_t mode,
                          const std::vector<uint8_t> &targets)
{
    if (targets.empty() || targets.size() > MAX_TARGETS || (mode == 1 && targets.size() != 1)) {
        return false;
    }

    VisionConfig parsed = {};
    {
        MutexGuard lock(g_config_mutex);
        parsed = g_config;
    }

    parsed.mode = mode;
    parsed.target_count = static_cast<uint8_t>(targets.size());
    for (size_t i = 0; i < targets.size(); i++) {
        if (find_color_index_by_name(parsed, targets[i]) < 0) {
            return false;
        }
        parsed.targets[i] = targets[i];
    }

    if (!is_config_valid(parsed) || !save_config_to_nvs(parsed)) {
        return false;
    }
    {
        MutexGuard lock(g_config_mutex);
        g_config = parsed;
    }
    apply_config_to_model(model, parsed);
    if (detect_app) {
        detect_app->set_palette(palette_from_config(parsed));
        detect_app->set_category_labels(category_labels_from_config(parsed));
    }
    return true;
}
} // namespace

extern "C" void app_main(void)
{
    WhoFrameCapNode *lcd_disp_frame_cap_node = nullptr;
    vTaskPrioritySet(xTaskGetCurrentTaskHandle(), 5);
    g_config_mutex = xSemaphoreCreateMutex();
    assert(g_config_mutex);

    init_nvs();
    g_config = default_config();
    VisionConfig stored_config = {};
    if (load_config_from_nvs(&stored_config)) {
        bool matches_standard = is_config_valid(stored_config) &&
                                stored_config.target_count >= 4 &&
                                stored_config.colors[1].name == 2 &&
                                stored_config.colors[1].box_color[2] > 200 && // Blue box has B>200
                                stored_config.colors[3].name == 4 &&
                                stored_config.colors[3].box_color[0] > 200 && // Yellow box has R>200
                                stored_config.colors[3].box_color[1] > 200;  // and G>200
        if (matches_standard) {
            g_config = stored_config;
        } else {
            ESP_LOGI(TAG, "Upgrading vision configuration in NVS to standard ColorTool order (1=Red, 2=Blue, 3=Green, 4=Yellow)");
            g_config = default_config();
            save_config_to_nvs(g_config);
        }
    } else {
        save_config_to_nvs(g_config);
    }
    log_current_config(g_config);

    auto frame_cap = get_lcd_mipi_csi_ppa_frame_cap_pipeline(&lcd_disp_frame_cap_node);

    auto communicate_app = new WhoCommunicateAppTerm({}, frame_cap->get_node("FrameCapFetch"));
    auto detect_app = new VisionDetectAppLCD(palette_from_config(g_config), frame_cap, lcd_disp_frame_cap_node);
    detect_app->set_category_labels(category_labels_from_config(g_config));
    auto *color_model = new ColorDetect(MODEL_INPUT_W, MODEL_INPUT_H);
    color_model->enable_morphology(3);
    auto *model = new VisionModeDetect(color_model,
                                       new LineRegressionDetect(MODEL_INPUT_W, MODEL_INPUT_H));
    apply_config_to_model(model, g_config);
    detect_app->set_model(model);
    detect_app->set_task_priorities(4, 3);
    auto host_protocol = new VisionHostProtocol(
        []() {
            MutexGuard lock(g_config_mutex);
            return g_config;
        },
        [model, detect_app](const VisionConfig &config) { return save_host_config(model, detect_app, config); });
    communicate_app->set_settings_cbs(
        [host_protocol]() { return host_protocol->get_settings_json(); },
        [host_protocol](const std::string &json) { return host_protocol->save_settings_json(json); });

    auto i2c_slave = new WhoI2CSlaveTask("I2CSlave");
    auto i2c_protocol = new ColorLineI2CProtocol(
        i2c_slave,
        lcd_disp_frame_cap_node->get_fb_width(),
        []() {
            MutexGuard lock(g_config_mutex);
            ColorLineI2CProtocol::ConfigInfo info = {
                static_cast<uint8_t>(g_config.mode),
                g_config.color_count,
                {},
            };
            info.tracking_color_indices.assign(g_config.targets, g_config.targets + g_config.target_count);
            return info;
        },
        [model, detect_app](uint8_t mode, const std::vector<uint8_t> &targets) {
            return apply_vision_setting(model, detect_app, mode, targets);
        },
        [detect_app]() { return detect_app->get_result(); });
    (void)i2c_protocol;

    bool all_tasks_started = true;
    auto ci1302 = WhoCi1302Task::get_instance();
    if (!ci1302->run(3072, 7, 0)) {
        ESP_LOGE(TAG, "Failed to start task: %s", ci1302->get_name().c_str());
        all_tasks_started = false;
    }
    if (!i2c_slave->run(4096, 8, 0)) {
        ESP_LOGE(TAG, "Failed to start task: %s", i2c_slave->get_name().c_str());
        all_tasks_started = false;
    }
    if (!communicate_app->run()) {
        ESP_LOGE(TAG, "Failed to start communication task");
        all_tasks_started = false;
    }
    if (!detect_app->run()) {
        ESP_LOGE(TAG, "Failed to start one or more detection tasks");
        all_tasks_started = false;
    }
    if (!all_tasks_started) {
        ESP_LOGE(TAG, "One or more tasks failed to start; system will continue running");
    }

    esp_err_t web_err = who::web::WhoWeb::get_instance().start(
        "Color / Line Detection", "color", lcd_disp_frame_cap_node);
    if (web_err != ESP_OK) {
        ESP_LOGE(TAG, "Web backhaul failed to start: %s", esp_err_to_name(web_err));
    }

    size_t internal_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    size_t internal_total = heap_caps_get_total_size(MALLOC_CAP_INTERNAL);
    size_t internal_used = internal_total - internal_free;
    ESP_LOGI(TAG,
             "Internal RAM: used %u bytes, free %u bytes, total %u bytes (%.2f%% used)",
             static_cast<unsigned>(internal_used),
             static_cast<unsigned>(internal_free),
             static_cast<unsigned>(internal_total),
             internal_total ? static_cast<double>(internal_used) * 100.0 / internal_total : 0.0);

    float psram_free = (float)heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    float psram_total = (float)heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
    ESP_LOGI(TAG, "PSRAM USE %.2f%% ", (psram_free / psram_total) * 100.0f);
}
