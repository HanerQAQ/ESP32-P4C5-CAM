#include "hand_gesture_i2c_protocol.hpp"
#include "who_ci1302_task.hpp"
#include "who_speech_recognition_store.hpp"
#include "cJSON.h"
#include "esp_log.h"
#include <algorithm>
#include <cstring>
#include <limits>

namespace {
const char *TAG = "HandGestureI2C";
constexpr size_t MAX_GESTURES = UINT8_MAX;
constexpr uint8_t NO_GESTURE_ID = 7;

void append_u16_le(std::vector<uint8_t> &data, uint16_t value)
{
    data.push_back(value & 0xff);
    data.push_back((value >> 8) & 0xff);
}

uint16_t clamp_u16(int value)
{
    return static_cast<uint16_t>(std::clamp(value, 0, static_cast<int>(std::numeric_limits<uint16_t>::max())));
}

int gesture_id_from_name(const std::string &name)
{
    const char *gesture_names[] = {
        "one", "two", "three", "four", "five", "like", "ok", "no_gesture", "call", "dislike"};

    const size_t left = name.find('(');
    const std::string base_name = left == std::string::npos ? name : name.substr(0, left);
    if (base_name.empty()) {
        return NO_GESTURE_ID;
    }

    for (size_t i = 0; i < sizeof(gesture_names) / sizeof(gesture_names[0]); i++) {
        if (base_name == gesture_names[i]) {
            return static_cast<int>(i);
        }
    }
    return NO_GESTURE_ID;
}
} // namespace

HandGestureI2CProtocol::HandGestureI2CProtocol(who::communicate::WhoI2CSlaveTask *i2c_task,
                                               const result_getter_t &result_getter) :
    m_i2c_task(i2c_task),
    m_result_getter(result_getter)
{
    if (m_i2c_task) {
        m_i2c_task->set_receive_parser(
            [this](const uint8_t *data, size_t len) { return handle_received_data(data, len); });
    }
}

bool HandGestureI2CProtocol::handle_received_data(const uint8_t *data, size_t len)
{
    if (!data || len == 0) {
        return false;
    }

    // ESP_LOGI(TAG, "I2C RX (%u bytes): %.*s", static_cast<unsigned>(len), static_cast<int>(len), data);

    cJSON *root = cJSON_ParseWithLength(reinterpret_cast<const char *>(data), len);
    if (!root) {
        return false;
    }

    bool handled = false;
    const char *command = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(root, "command"));
    const cJSON *params = cJSON_GetObjectItemCaseSensitive(root, "params");
    if (command && strcmp(command, "get_info") == 0) {
        handled = handle_get_info(cJSON_GetStringValue(params));
    } else if (command && strcmp(command, "speech_recognition") == 0) {
        handled = send_speech_recognition();
    } else {
        handled = handle_play_voice_prompt(root);
    }

    cJSON_Delete(root);
    return handled;
}

bool HandGestureI2CProtocol::handle_get_info(const char *params)
{
    if (!params) {
        return false;
    }
    if (strcmp(params, "hand_gesture") == 0) {
        return send_detection_result();
    }
    return false;
}

bool HandGestureI2CProtocol::handle_play_voice_prompt(const cJSON *root)
{
    if (!cJSON_IsObject(root)) {
        return false;
    }

    const char *command = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(root, "command"));
    const cJSON *params = cJSON_GetObjectItemCaseSensitive(root, "params");
    if (!command || strcmp(command, "play_voice_prompt") != 0) {
        return false;
    }
    if (!cJSON_IsNumber(params) || params->valuedouble != params->valueint ||
        params->valueint < 0 || params->valueint > UINT16_MAX) {
        return false;
    }

    return forward_play_voice_prompt(static_cast<uint16_t>(params->valueint));
}

bool HandGestureI2CProtocol::send_detection_result()
{
    if (!m_result_getter) {
        return false;
    }

    std::vector<uint8_t> data;
    return encode_result(m_result_getter(), data) && send_data(data);
}

bool HandGestureI2CProtocol::send_speech_recognition()
{
    uint8_t data0 = 0;
    uint8_t data1 = 0;
    who::communicate::consume_speech_recognition_result(&data0, &data1);
    return send_data({data0, data1});
}

bool HandGestureI2CProtocol::send_data(const std::vector<uint8_t> &data)
{
    return m_i2c_task && m_i2c_task->set_tx_data(data.data(), data.size());
}

bool HandGestureI2CProtocol::forward_play_voice_prompt(uint16_t voice_data)
{
    const uint8_t frame[] = {
        0xAA,
        0x55,
        static_cast<uint8_t>(voice_data >> 8),
        static_cast<uint8_t>(voice_data & 0xFF),
        0xFB,
    };
    return who::communicate::WhoCi1302Task::get_instance()->send(frame, sizeof(frame));
}

bool HandGestureI2CProtocol::encode_result(const who::recognition::WhoRecognitionCore::result_t &result,
                                           std::vector<uint8_t> &data) const
{
    data.clear();
    data.reserve(1 + std::min(result.detect_result.det_res.size(), MAX_GESTURES) * 9);
    data.push_back(0);

    size_t encoded = 0;
    for (const auto &item : result.detect_result.det_res) {
        if (data[0] >= MAX_GESTURES) {
            break;
        }
        const int gesture_id = encoded < result.recognition_results.size() ?
            gesture_id_from_name(result.recognition_results[encoded]) :
            NO_GESTURE_ID;
        encoded++;
        const int x = item.box.size() > 0 ? item.box[0] : 0;
        const int y = item.box.size() > 1 ? item.box[1] : 0;
        const int x2 = item.box.size() > 2 ? item.box[2] : x;
        const int y2 = item.box.size() > 3 ? item.box[3] : y;
        data.push_back(static_cast<uint8_t>(gesture_id));
        append_u16_le(data, clamp_u16(x));
        append_u16_le(data, clamp_u16(y));
        append_u16_le(data, clamp_u16(std::max(0, x2 - x)));
        append_u16_le(data, clamp_u16(std::max(0, y2 - y)));
        data[0]++;
    }
    return true;
}
