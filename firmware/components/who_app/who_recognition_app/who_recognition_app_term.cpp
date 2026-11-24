#include "who_recognition_app_term.hpp"
#include "hand_detect.hpp"
#include "hand_gesture_recognition.hpp"
#include "who_yield2idle.hpp"

namespace who {
namespace app {
WhoRecognitionAppTerm::WhoRecognitionAppTerm(frame_cap::WhoFrameCap *frame_cap) : WhoRecognitionAppBase(frame_cap)
{
    auto recognition_task = m_recognition->get_recognition_task();
    recognition_task->set_recognition_result_cb(
        std::bind(&WhoRecognitionAppTerm::recognition_result_cb, this, std::placeholders::_1));

    m_recognition->set_recognizer(new HandGestureRecognizer());
    m_recognition->set_detect_model(new HandDetect());
}



WhoRecognitionAppTerm::~WhoRecognitionAppTerm()
{
}

bool WhoRecognitionAppTerm::run()
{
    bool ret = WhoYield2Idle::get_instance()->run();
    for (const auto &frame_cap_node : m_frame_cap->get_all_nodes()) {
        ret &= frame_cap_node->run(4096, 2, 0);
    }
    ret &= m_recognition->get_detect_task()->run(3584, 2, 1);
    ret &= m_recognition->get_recognition_task()->run(3584, 2, 1);
    return ret;
}

void WhoRecognitionAppTerm::recognition_result_cb(const std::string &result)
{
    ESP_LOGI("WhoRecognition", "%s", result.c_str());
}
} // namespace app
} // namespace who
