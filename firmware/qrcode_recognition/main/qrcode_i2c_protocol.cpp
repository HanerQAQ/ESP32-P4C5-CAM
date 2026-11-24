#include "qrcode_i2c_protocol.hpp"
#include "who_ci1302_task.hpp"
#include "who_speech_recognition_store.hpp"
#include "cJSON.h"
#include "esp_log.h"
#include <algorithm>
#include <cstring>
#include <limits>

namespace {
const char *TAG = "QRCodeI2CProtocol";
constexpr size_t MAX_QRCODES = UINT8_MAX;
constexpr size_t MAX_STRING_LEN = UINT16_MAX;

void append_u16_le(std::vector<uint8_t> &data, uint16_t value)
{
    data.push_back(value & 0xff);
    data.push_back((value >> 8) & 0xff);
}
} // namespace

QRCodeI2CProtocol::QRCodeI2CProtocol(who::communicate::WhoI2CSlaveTask *i2c_task,
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

bool QRCodeI2CProtocol::handle_received_data(const uint8_t *data, size_t len)
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

bool QRCodeI2CProtocol::handle_get_info(const char *params)
{
    if (!params || m_result_param != params) {
        return false;
    }
    return send_qrcode_result();
}

bool QRCodeI2CProtocol::handle_play_voice_prompt(const cJSON *root)
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

bool QRCodeI2CProtocol::send_qrcode_result()
{
    if (!m_result_getter) {
        return false;
    }

    std::vector<uint8_t> data;
    return encode_qrcode_result(m_result_getter(), data) && send_data(data);
}

bool QRCodeI2CProtocol::send_speech_recognition()
{
    uint8_t data0 = 0;
    uint8_t data1 = 0;
    who::communicate::consume_speech_recognition_result(&data0, &data1);
    return send_data({data0, data1});
}

bool QRCodeI2CProtocol::send_data(const std::vector<uint8_t> &data)
{
    return m_i2c_task && m_i2c_task->set_tx_data(data.data(), data.size());
}

bool QRCodeI2CProtocol::forward_play_voice_prompt(uint16_t voice_data)
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

bool QRCodeI2CProtocol::encode_qrcode_result(const who::qrcode::WhoQRCode::result_t &result,
                                             std::vector<uint8_t> &data) const
{
    const size_t count = std::min(result.codes.size(), MAX_QRCODES);
    data.clear();
    data.push_back(static_cast<uint8_t>(count));

    size_t encoded = 0;
    for (const auto &code : result.codes) {
        if (encoded >= count) {
            break;
        }
        const size_t string_len = std::min(code.payload.size(), MAX_STRING_LEN);
        data.push_back(static_cast<uint8_t>(encoded + 1));
        append_u16_le(data, code.x);
        append_u16_le(data, code.y);
        append_u16_le(data, code.w);
        append_u16_le(data, code.h);
        append_u16_le(data, static_cast<uint16_t>(string_len));
        data.insert(data.end(), code.payload.begin(), code.payload.begin() + string_len);
        encoded++;
    }
    return true;
}
