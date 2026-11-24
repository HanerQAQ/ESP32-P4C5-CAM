#pragma once

#include "who_i2c_slave_task.hpp"
#include "who_recognition.hpp"
#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

struct cJSON;

class HandGestureI2CProtocol {
public:
    using result_getter_t = std::function<who::recognition::WhoRecognitionCore::result_t(void)>;

    HandGestureI2CProtocol(who::communicate::WhoI2CSlaveTask *i2c_task, const result_getter_t &result_getter);

private:
    bool handle_received_data(const uint8_t *data, size_t len);
    bool handle_get_info(const char *params);
    bool handle_play_voice_prompt(const cJSON *root);
    bool send_detection_result();
    bool send_speech_recognition();
    bool send_data(const std::vector<uint8_t> &data);
    bool forward_play_voice_prompt(uint16_t voice_data);
    bool encode_result(const who::recognition::WhoRecognitionCore::result_t &result, std::vector<uint8_t> &data) const;

    who::communicate::WhoI2CSlaveTask *m_i2c_task;
    result_getter_t m_result_getter;
};
