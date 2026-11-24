#include "apriltag_i2c_protocol.hpp"
#include "who_ci1302_task.hpp"
#include "who_speech_recognition_store.hpp"
#include "cJSON.h"
#include "esp_log.h"
#include <algorithm>
#include <cstring>
#include <limits>

namespace {
const char *TAG = "AprilTagI2CProtocol";
constexpr size_t APRILTAG_RECORD_SIZE = 38;
constexpr size_t MAX_TAGS = UINT8_MAX;
static_assert(APRILTAG_RECORD_SIZE == 2 + 4 * sizeof(uint16_t) + 7 * sizeof(float));

void append_u16_le(std::vector<uint8_t> &data, uint16_t value)
{
    data.push_back(value & 0xff);
    data.push_back((value >> 8) & 0xff);
}

void append_float_le(std::vector<uint8_t> &data, float value)
{
    uint8_t bytes[sizeof(float)];
    memcpy(bytes, &value, sizeof(bytes));
    data.insert(data.end(), bytes, bytes + sizeof(bytes));
}

uint16_t clamp_u16(int value)
{
    return static_cast<uint16_t>(std::clamp(value, 0, static_cast<int>(std::numeric_limits<uint16_t>::max())));
}
} // namespace

AprilTagI2CProtocol::AprilTagI2CProtocol(who::communicate::WhoI2CSlaveTask *i2c_task,
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

bool AprilTagI2CProtocol::handle_received_data(const uint8_t *data, size_t len)
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

bool AprilTagI2CProtocol::handle_get_info(const char *params)
{
    if (!params || m_result_param != params) {
        return false;
    }
    return send_apriltag_result();
}

bool AprilTagI2CProtocol::handle_play_voice_prompt(const cJSON *root)
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

bool AprilTagI2CProtocol::send_apriltag_result()
{
    if (!m_result_getter) {
        return false;
    }

    std::vector<uint8_t> data;
    return encode_apriltag_result(m_result_getter(), data) && send_data(data);
}

bool AprilTagI2CProtocol::send_speech_recognition()
{
    uint8_t data0 = 0;
    uint8_t data1 = 0;
    who::communicate::consume_speech_recognition_result(&data0, &data1);
    return send_data({data0, data1});
}

bool AprilTagI2CProtocol::send_data(const std::vector<uint8_t> &data)
{
    return m_i2c_task && m_i2c_task->set_tx_data(data.data(), data.size());
}

bool AprilTagI2CProtocol::forward_play_voice_prompt(uint16_t voice_data)
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

bool AprilTagI2CProtocol::encode_apriltag_result(const who::apriltag::WhoApriltag::result_t &result,
                                                 std::vector<uint8_t> &data) const
{
    const size_t count = std::min(result.tags.size(), MAX_TAGS);
    data.clear();
    data.reserve(1 + count * APRILTAG_RECORD_SIZE);
    data.push_back(static_cast<uint8_t>(count));

    size_t encoded = 0;
    for (const auto &tag : result.tags) {
        if (encoded >= count) {
            break;
        }

        append_u16_le(data, clamp_u16(tag.id));
        append_u16_le(data, clamp_u16(tag.x1));
        append_u16_le(data, clamp_u16(tag.y1));
        append_u16_le(data, clamp_u16(std::max(0, tag.x2 - tag.x1)));
        append_u16_le(data, clamp_u16(std::max(0, tag.y2 - tag.y1)));
        append_float_le(data, tag.rotation);
        append_float_le(data, tag.x_translation);
        append_float_le(data, tag.y_translation);
        append_float_le(data, tag.z_translation);
        append_float_le(data, tag.x_rotation);
        append_float_le(data, tag.y_rotation);
        append_float_le(data, tag.z_rotation);
        encoded++;
    }

    return data.size() == 1 + encoded * APRILTAG_RECORD_SIZE;
}
