#include "who_recognition.hpp"
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace {
int hand_gesture_id_from_name(const char *name)
{
    const char *gesture_names[] = {
        "one", "two", "three", "four", "five", "like", "ok", "no_gesture", "call", "dislike"};

    if (!name) {
        return -1;
    }

    for (size_t i = 0; i < sizeof(gesture_names) / sizeof(gesture_names[0]); i++) {
        if (strcmp(name, gesture_names[i]) == 0) {
            if (strcmp(name, "no_gesture") == 0) {
                return -1;
            }
            return static_cast<int>(i);
        }
    }
    return -1;
}
} // namespace

namespace who {
namespace recognition {
WhoRecognitionCore::WhoRecognitionCore(const std::string &name, detect::WhoDetect *detect) :
    task::WhoTask(name), m_detect(detect)
{
}

WhoRecognitionCore::~WhoRecognitionCore()
{
    delete m_recognizer;
}

void WhoRecognitionCore::set_recognizer(HandGestureRecognizer *recognizer)
{
    m_recognizer = recognizer;
}

void WhoRecognitionCore::set_recognition_result_cb(const std::function<void(const std::string &)> &result_cb)
{
    m_recognition_result_cb = result_cb;
}

void WhoRecognitionCore::set_recognition_result_cb(const std::function<void(const result_t &)> &result_cb)
{
    m_recognition_detail_result_cb = result_cb;
}

void WhoRecognitionCore::set_detect_result_cb(const std::function<void(const detect::WhoDetect::result_t &)> &result_cb)
{
    m_detect_result_cb = result_cb;
}

void WhoRecognitionCore::set_cleanup_func(const std::function<void()> &cleanup_func)
{
    m_cleanup = cleanup_func;
}

bool WhoRecognitionCore::run(const configSTACK_DEPTH_TYPE uxStackDepth,
                             UBaseType_t uxPriority,
                             const BaseType_t xCoreID)
{
    if (!m_recognizer) {
        ESP_LOGE("WhoRecognitionCore", "recognizer is nullptr, please call set_recognizer() first.");
        return false;
    }
    return task::WhoTask::run(uxStackDepth, uxPriority, xCoreID);
}

void WhoRecognitionCore::task()
{
    while (true) {
        auto new_detect_result_cb = [this](const detect::WhoDetect::result_t &result) {
            if (m_detect_result_cb) {
                m_detect_result_cb(result);
            }

            if (result.det_res.empty()) {
                if (m_recognition_detail_result_cb) {
                    m_recognition_detail_result_cb({result, {}});
                }
                return;
            }

            auto ret = m_recognizer->recognize(result.img, result.det_res);
            std::vector<std::string> recognition_results;
            recognition_results.reserve(ret.size());
            for (const auto &recognition_result : ret) {
                const int gesture_id = hand_gesture_id_from_name(recognition_result.cat_name);
                if (gesture_id < 0) {
                    recognition_results.emplace_back();
                    continue;
                }
                recognition_results.emplace_back(std::format("{}({})", recognition_result.cat_name, gesture_id));
            }

            if (m_recognition_result_cb && !ret.empty()) {
                m_recognition_result_cb(recognition_results[0]);
            }
            if (m_recognition_detail_result_cb) {
                m_recognition_detail_result_cb({result, recognition_results});
            }
        };
        m_detect->set_detect_result_cb(new_detect_result_cb);

        EventBits_t event_bits = xEventGroupWaitBits(
            m_event_group, TASK_PAUSE | TASK_STOP, pdTRUE, pdFALSE, portMAX_DELAY);
        if (event_bits & TASK_STOP) {
            break;
        } else if (event_bits & TASK_PAUSE) {
            m_detect->set_detect_result_cb(m_detect_result_cb);
            xEventGroupSetBits(m_event_group, TASK_PAUSED);
            EventBits_t pause_event_bits =
                xEventGroupWaitBits(m_event_group, TASK_RESUME | TASK_STOP, pdTRUE, pdFALSE, portMAX_DELAY);
            if (pause_event_bits & TASK_STOP) {
                break;
            }
        }
    }
    m_detect->set_detect_result_cb(m_detect_result_cb);
    xEventGroupSetBits(m_event_group, TASK_STOPPED);
    vTaskDelete(NULL);
}

void WhoRecognitionCore::cleanup()
{
    if (m_cleanup) {
        m_cleanup();
    }
}

WhoRecognition::WhoRecognition(frame_cap::WhoFrameCapNode *frame_cap_node) :
    m_detect(new detect::WhoDetect("Detect", frame_cap_node)),
    m_recognition(new WhoRecognitionCore("Recognition", m_detect))
{
    WhoTaskGroup::register_task(m_detect);
    WhoTaskGroup::register_task(m_recognition);
}

WhoRecognition::~WhoRecognition()
{
    WhoTaskGroup::destroy();
}

void WhoRecognition::set_detect_model(dl::detect::Detect *model)
{
    m_detect->set_model(model);
}

void WhoRecognition::set_recognizer(HandGestureRecognizer *recognizer)
{
    m_recognition->set_recognizer(recognizer);
}

detect::WhoDetect *WhoRecognition::get_detect_task()
{
    return m_detect;
}

WhoRecognitionCore *WhoRecognition::get_recognition_task()
{
    return m_recognition;
}
} // namespace recognition
} // namespace who
