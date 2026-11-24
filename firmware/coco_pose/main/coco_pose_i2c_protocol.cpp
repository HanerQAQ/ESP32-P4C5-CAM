#include "coco_pose_i2c_protocol.hpp"
#include "who_ci1302_task.hpp"
#include "who_speech_recognition_store.hpp"
#include "cJSON.h"
#include "esp_log.h"
#include <algorithm>
#include <cstring>
#include <limits>

namespace {
const char *TAG = "CocoPoseI2CProtocol";
constexpr size_t COCO_KEYPOINT_COUNT = 17;
constexpr size_t MAX_OBJECTS = UINT8_MAX;
constexpr size_t POSE_RECORD_HEADER_SIZE = 10;
constexpr size_t KEYPOINT_RECORD_SIZE = 5;

void append_u16_le(std::vector<uint8_t> &data, uint16_t value)
{
    data.push_back(value & 0xff);
    data.push_back((value >> 8) & 0xff);
}

uint16_t clamp_u16(int value)
{
    return static_cast<uint16_t>(std::clamp(value, 0, static_cast<int>(std::numeric_limits<uint16_t>::max())));
}
} // namespace

CocoPoseI2CProtocol::CocoPoseI2CProtocol(who::communicate::WhoI2CSlaveTask *i2c_task,
                                         const std::string &result_param,
                                         const result_getter_t &result_getter) :
    m_i2c_task(i2c_task),
    m_result_param(result_param),
    m_result_getter(result_getter)
{
    if (m_i2c_task) {
        m_i2c_task->set_receive_parser(
            [this](const uint8_t *data, size_t len) { return handle_received_data(data, len); });
    }
}

bool CocoPoseI2CProtocol::handle_received_data(const uint8_t *data, size_t len)
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
    } else if (command && strcmp(command, "play_voice_prompt") == 0) {
        handled = handle_play_voice_prompt(root);
    }

    cJSON_Delete(root);
    return handled;
}

bool CocoPoseI2CProtocol::handle_get_info(const char *params)
{
    if (!params || m_result_param != params) {
        return false;
    }
    return send_pose_result();
}

bool CocoPoseI2CProtocol::handle_play_voice_prompt(const cJSON *root)
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

    const uint16_t voice_data = static_cast<uint16_t>(params->valueint);
    // ESP_LOGI(TAG, "Forwarding voice prompt data: %u", static_cast<unsigned>(voice_data));
    return forward_play_voice_prompt(voice_data);
}

bool CocoPoseI2CProtocol::send_pose_result()
{
    if (!m_result_getter) {
        return false;
    }

    std::vector<uint8_t> data;
    return encode_pose_result(m_result_getter(), data) && send_data(data);
}

bool CocoPoseI2CProtocol::send_speech_recognition()
{
    uint8_t data0 = 0;
    uint8_t data1 = 0;
    who::communicate::consume_speech_recognition_result(&data0, &data1);
    return send_data({data0, data1});
}

bool CocoPoseI2CProtocol::send_data(const std::vector<uint8_t> &data)
{
    // ESP_LOGI(TAG, "I2C TX DATA (%u bytes)", static_cast<unsigned>(data.size()));
    // ESP_LOG_BUFFER_HEX(TAG, data.data(), data.size());
    return m_i2c_task && m_i2c_task->set_tx_data(data.data(), data.size());
}

bool CocoPoseI2CProtocol::forward_play_voice_prompt(uint16_t voice_data)
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

bool CocoPoseI2CProtocol::encode_pose_result(const who::detect::WhoDetect::result_t &result,
                                             std::vector<uint8_t> &data) const
{
    const size_t count = std::min(result.det_res.size(), MAX_OBJECTS);
    data.clear();
    data.reserve(1 + count * (POSE_RECORD_HEADER_SIZE + COCO_KEYPOINT_COUNT * KEYPOINT_RECORD_SIZE));
    data.push_back(static_cast<uint8_t>(count));

    size_t encoded = 0;
    for (const auto &item : result.det_res) {
        if (encoded >= count) {
            break;
        }

        const int x = item.box.size() > 0 ? item.box[0] : 0;
        const int y = item.box.size() > 1 ? item.box[1] : 0;
        const int x2 = item.box.size() > 2 ? item.box[2] : x;
        const int y2 = item.box.size() > 3 ? item.box[3] : y;

        data.push_back(static_cast<uint8_t>(encoded + 1));
        append_u16_le(data, clamp_u16(x));
        append_u16_le(data, clamp_u16(y));
        append_u16_le(data, clamp_u16(std::max(0, x2 - x)));
        append_u16_le(data, clamp_u16(std::max(0, y2 - y)));

        const size_t keypoint_count_offset = data.size();
        data.push_back(0);
        uint8_t keypoint_count = 0;
        for (size_t i = 0; i < COCO_KEYPOINT_COUNT; ++i) {
            const size_t keypoint_index = i * 2;
            const int keypoint_x = item.keypoint.size() > keypoint_index ? item.keypoint[keypoint_index] : 0;
            const int keypoint_y = item.keypoint.size() > keypoint_index + 1 ? item.keypoint[keypoint_index + 1] : 0;
            if (keypoint_x == 0 && keypoint_y == 0) {
                continue;
            }
            data.push_back(static_cast<uint8_t>(i));
            append_u16_le(data, clamp_u16(keypoint_x));
            append_u16_le(data, clamp_u16(keypoint_y));
            keypoint_count++;
        }
        data[keypoint_count_offset] = keypoint_count;

        encoded++;
    }
    return true;
}
