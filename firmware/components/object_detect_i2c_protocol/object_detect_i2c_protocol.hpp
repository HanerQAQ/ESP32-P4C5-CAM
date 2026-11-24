#pragma once

#include "who_detect.hpp"
#include "who_i2c_slave_task.hpp"
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

struct cJSON;

class ObjectDetectI2CProtocol {
public:
    enum class ObjectIdMode {
        SEQUENCE,
        CATEGORY,
    };

    using result_getter_t = std::function<who::detect::WhoDetect::result_t(void)>;

    ObjectDetectI2CProtocol(who::communicate::WhoI2CSlaveTask *i2c_task,
                            const std::string &result_param,
                            const result_getter_t &result_getter,
                            ObjectIdMode object_id_mode = ObjectIdMode::SEQUENCE);

private:
    bool handle_received_data(const uint8_t *data, size_t len);
    bool handle_get_info(const char *params);
    bool handle_play_voice_prompt(const cJSON *root);
    bool send_detection_result();
    bool send_speech_recognition();
    bool send_data(const std::vector<uint8_t> &data);
    bool forward_play_voice_prompt(uint16_t voice_data);
    bool encode_detection_result(const who::detect::WhoDetect::result_t &result, std::vector<uint8_t> &data) const;

    who::communicate::WhoI2CSlaveTask *m_i2c_task;
    std::string m_result_param;
    result_getter_t m_result_getter;
    ObjectIdMode m_object_id_mode;
};
