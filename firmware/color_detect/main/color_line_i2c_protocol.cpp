#include "color_line_i2c_protocol.hpp"
#include "who_ci1302_task.hpp"
#include "who_speech_recognition_store.hpp"
#include "cJSON.h"
#include "esp_log.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

namespace {
const char *TAG = "ColorLineI2CProtocol";
constexpr size_t MAX_COLOR_BLOCKS = UINT8_MAX;
constexpr float PI_F = 3.14159265358979323846f;

void append_u16_le(std::vector<uint8_t> &data, uint16_t value)
{
    data.push_back(value & 0xff);
    data.push_back((value >> 8) & 0xff);
}

void append_float_le(std::vector<uint8_t> &data, float value)
{
    uint8_t bytes[sizeof(value)] = {};
    memcpy(bytes, &value, sizeof(bytes));
    data.insert(data.end(), bytes, bytes + sizeof(bytes));
}

uint16_t clamp_u16(int value)
{
    return static_cast<uint16_t>(std::clamp(value, 0, static_cast<int>(std::numeric_limits<uint16_t>::max())));
}
} // namespace

ColorLineI2CProtocol::ColorLineI2CProtocol(who::communicate::WhoI2CSlaveTask *i2c_task,
                                           uint16_t result_width,
                                           const config_getter_t &config_getter,
                                           const setting_handler_t &setting_handler,
                                           const result_getter_t &result_getter) :
    m_i2c_task(i2c_task),
    m_result_width(result_width),
    m_config_getter(config_getter),
    m_setting_handler(setting_handler),
    m_result_getter(result_getter)
{
    if (m_i2c_task) {
        m_i2c_task->set_receive_parser(
            [this](const uint8_t *data, size_t len) { return handle_received_data(data, len); });
    }
}

bool ColorLineI2CProtocol::handle_received_data(const uint8_t *data, size_t len)
{
    if (!data || len == 0) {
        return false;
    }

    // ESP_LOGI(TAG, "I2C RX length: %u bytes", static_cast<unsigned>(len));

    cJSON *root = cJSON_ParseWithLength(reinterpret_cast<const char *>(data), len);
    if (!root) {
        return false;
    }

    bool handled = false;
    const char *command = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(root, "command"));
    const cJSON *params = cJSON_GetObjectItemCaseSensitive(root, "params");
    if (command && strcmp(command, "get_info") == 0) {
        handled = handle_get_info(cJSON_GetStringValue(params));
    } else if (command && strcmp(command, "vision_setting") == 0) {
        handled = handle_vision_setting(params);
    } else if (command && strcmp(command, "speech_recognition") == 0) {
        handled = send_speech_recognition();
    } else {
        handled = handle_play_voice_prompt(root);
    }

    cJSON_Delete(root);
    return handled;
}

bool ColorLineI2CProtocol::handle_get_info(const char *params)
{
    if (!params) {
        return false;
    }
    if (strcmp(params, "color_config") == 0) {
        return send_config_info();
    }
    if (strcmp(params, "color_result") == 0) {
        return send_detection_result();
    }
    return false;
}

bool ColorLineI2CProtocol::handle_vision_setting(const cJSON *params)
{
    if (!cJSON_IsObject(params)) {
        return send_setting_result(false);
    }

    const cJSON *mode_item = cJSON_GetObjectItemCaseSensitive(params, "mode");
    const cJSON *targets_item = cJSON_GetObjectItemCaseSensitive(params, "target");
    bool valid = cJSON_IsNumber(mode_item) && mode_item->valuedouble == mode_item->valueint &&
                 mode_item->valueint >= 0 && mode_item->valueint <= 1 && cJSON_IsArray(targets_item);

    std::vector<uint8_t> targets;
    if (valid) {
        const int target_count = cJSON_GetArraySize(targets_item);
        const ConfigInfo config = m_config_getter ? m_config_getter() : ConfigInfo{};
        valid = target_count >= 1 && target_count <= config.color_count;
        if (valid && mode_item->valueint == 1) {
            valid = target_count == 1;
        }
        targets.reserve(target_count);
        for (int i = 0; valid && i < target_count; i++) {
            const cJSON *target = cJSON_GetArrayItem(targets_item, i);
            valid = cJSON_IsNumber(target) && target->valuedouble == target->valueint &&
                    target->valueint >= 0 && target->valueint <= UINT8_MAX;
            if (valid) {
                const uint8_t value = static_cast<uint8_t>(target->valueint);
                valid = std::find(targets.begin(), targets.end(), value) == targets.end();
                targets.push_back(value);
            }
        }
    }

    const bool success = valid && m_setting_handler &&
                         m_setting_handler(static_cast<uint8_t>(mode_item->valueint), targets);
    return send_setting_result(success);
}

bool ColorLineI2CProtocol::handle_play_voice_prompt(const cJSON *root)
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

bool ColorLineI2CProtocol::send_config_info()
{
    if (!m_config_getter) {
        return false;
    }
    const ConfigInfo config = m_config_getter();
    std::vector<uint8_t> data;
    data.reserve(1 + config.tracking_color_indices.size());
    data.push_back(config.mode);
    data.insert(data.end(), config.tracking_color_indices.begin(), config.tracking_color_indices.end());
    return send_data(data);
}

bool ColorLineI2CProtocol::send_detection_result()
{
    if (!m_config_getter || !m_result_getter) {
        return false;
    }

    std::vector<uint8_t> data;
    const ConfigInfo config = m_config_getter();
    const auto result = m_result_getter();
    const bool encoded = config.mode == 1 ? encode_line_result(result, data) : encode_color_result(result, data);
    return encoded && send_data(data);
}

bool ColorLineI2CProtocol::send_setting_result(bool success)
{
    return send_data({static_cast<uint8_t>(success ? 1 : 0)});
}

bool ColorLineI2CProtocol::send_speech_recognition()
{
    uint8_t data0 = 0;
    uint8_t data1 = 0;
    who::communicate::consume_speech_recognition_result(&data0, &data1);
    return send_data({data0, data1});
}

bool ColorLineI2CProtocol::send_data(const std::vector<uint8_t> &data)
{
    return m_i2c_task && m_i2c_task->set_tx_data(data.data(), data.size());
}

bool ColorLineI2CProtocol::forward_play_voice_prompt(uint16_t voice_data)
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

bool ColorLineI2CProtocol::encode_color_result(const who::detect::WhoDetect::result_t &result,
                                               std::vector<uint8_t> &data) const
{
    const size_t count = std::min(result.det_res.size(), MAX_COLOR_BLOCKS);
    if (!m_config_getter) {
        return false;
    }
    const ConfigInfo config = m_config_getter();
    data.clear();
    data.reserve(1 + count * 9);
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
        if (item.category < 0 || static_cast<size_t>(item.category) >= config.tracking_color_indices.size()) {
            return false;
        }
        data.push_back(config.tracking_color_indices[item.category]);
        append_u16_le(data, clamp_u16(x));
        append_u16_le(data, clamp_u16(y));
        append_u16_le(data, clamp_u16(std::max(0, x2 - x)));
        append_u16_le(data, clamp_u16(std::max(0, y2 - y)));
        encoded++;
    }
    return true;
}

bool ColorLineI2CProtocol::encode_line_result(const who::detect::WhoDetect::result_t &result,
                                              std::vector<uint8_t> &data) const
{
    data.clear();
    if (result.det_res.empty() || result.det_res.front().keypoint.size() < 4) {
        data.push_back(0);
        return true;
    }

    const auto &item = result.det_res.front();
    const int x0 = item.keypoint[0];
    const int y0 = item.keypoint[1];
    const int x1 = item.keypoint[2];
    const int y1 = item.keypoint[3];
    const float dx = static_cast<float>(x1 - x0);
    const float dy = static_cast<float>(y1 - y0);
    const float angle = std::atan2(dx, dy) * 180.0f / PI_F;
    const float width = m_result_width > 0 ? static_cast<float>(m_result_width) : 1.0f;
    const float center_x = (static_cast<float>(x0) + static_cast<float>(x1)) * 0.5f;
    const float offset = std::clamp((center_x - width * 0.5f) / (width * 0.5f), -1.0f, 1.0f);

    data.reserve(17);
    data.push_back(1);
    append_u16_le(data, clamp_u16(x0));
    append_u16_le(data, clamp_u16(y0));
    append_u16_le(data, clamp_u16(x1));
    append_u16_le(data, clamp_u16(y1));
    append_float_le(data, angle);
    append_float_le(data, offset);
    return true;
}
