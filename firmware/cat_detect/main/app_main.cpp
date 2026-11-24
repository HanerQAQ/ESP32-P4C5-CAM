#include "frame_cap_pipeline.hpp"
#include "cat_detect.hpp"
#include "object_detect_i2c_protocol.hpp"
#include "who_i2c_slave_task.hpp"
#include "who_ci1302_task.hpp"
#include "who_detect_app_lcd.hpp"
#include "who_frame_lcd_disp.hpp"
#include "who_web.hpp"
#include "who_web_detect_json.hpp"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_check.h"
#include "p4c5_cam_board.h"
#include <cassert>
#include <vector>

static const char *TAG = "app_main";

using namespace who::frame_cap;
using namespace who::app;
using namespace who::communicate;
using namespace who::lcd_disp;

namespace {
class CachedDetectAppLCD : public WhoDetectAppLCD {
public:
    CachedDetectAppLCD(const std::vector<std::vector<uint8_t>> &palette,
                       WhoFrameCap *frame_cap,
                       WhoFrameCapNode *lcd_disp_frame_cap_node = nullptr) :
        WhoDetectAppLCD(palette, frame_cap, lcd_disp_frame_cap_node),
        m_result_mutex(xSemaphoreCreateMutex())
    {
        assert(m_result_mutex);
    }

    ~CachedDetectAppLCD()
    {
        vSemaphoreDelete(m_result_mutex);
    }

    who::detect::WhoDetect::result_t get_result()
    {
        xSemaphoreTake(m_result_mutex, portMAX_DELAY);
        auto result = m_last_result;
        xSemaphoreGive(m_result_mutex);
        return result;
    }

protected:
    void detect_result_cb(const who::detect::WhoDetect::result_t &result) override
    {
        WhoDetectAppLCD::detect_result_cb(result);
        xSemaphoreTake(m_result_mutex, portMAX_DELAY);
        m_last_result = result;
        xSemaphoreGive(m_result_mutex);
        who::web::WhoWeb::get_instance().set_objects_json(
            who::web::json::detect_objects(result, {"cat"}));
    }

private:
    SemaphoreHandle_t m_result_mutex;
    who::detect::WhoDetect::result_t m_last_result;
};
} // namespace

dl::detect::Detect *get_detect_model()
{
#if defined(CONFIG_CAT_DETECT_MODEL_LOCATION)
    return new CatDetect(static_cast<CatDetect::model_type_t>(CONFIG_DEFAULT_CAT_DETECT_MODEL),
                               false);
#else
    ESP_LOGE("MAIN", "No detect model component in idf_component.yml");
    return nullptr;
#endif
}

extern "C" void app_main(void)
{
    WhoFrameCapNode *lcd_disp_frame_cap_node = nullptr;
    vTaskPrioritySet(xTaskGetCurrentTaskHandle(), 5);

    auto frame_cap = get_lcd_mipi_csi_ppa_frame_cap_pipeline(&lcd_disp_frame_cap_node);

    auto detect_app = new CachedDetectAppLCD({{0, 255, 0}}, frame_cap, lcd_disp_frame_cap_node);
    detect_app->set_model(get_detect_model());
    detect_app->set_task_priorities(4, 3);

    auto i2c_slave = new WhoI2CSlaveTask("I2CSlave");
    auto i2c_protocol = new ObjectDetectI2CProtocol(
        i2c_slave,
        "cat",
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
    if (!detect_app->run()) {
        ESP_LOGE(TAG, "Failed to start one or more detection tasks");
        all_tasks_started = false;
    }
    if (!all_tasks_started) {
        ESP_LOGE(TAG, "One or more tasks failed to start; system will continue running");
    }

    esp_err_t web_err = who::web::WhoWeb::get_instance().start(
        "Cat Detection", "cat", lcd_disp_frame_cap_node);
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
