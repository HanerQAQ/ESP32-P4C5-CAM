#pragma once

#include "who_detect.hpp"
#include "who_i2c_slave_task.hpp"
#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

struct cJSON;

class ColorLineI2CProtocol {
public:
    struct ConfigInfo {
        uint8_t mode;
        uint8_t color_count;
        std::vector<uint8_t> tracking_color_indices;
    };

    using config_getter_t = std::function<ConfigInfo(void)>;
    using setting_handler_t = std::function<bool(uint8_t mode, const std::vector<uint8_t> &targets)>;
    using result_getter_t = std::function<who::detect::WhoDetect::result_t(void)>;

    ColorLineI2CProtocol(who::communicate::WhoI2CSlaveTask *i2c_task,
                        uint16_t result_width,
                        const config_getter_t &config_getter,
                        const setting_handler_t &setting_handler,
                        const result_getter_t &result_getter);

private:
    bool handle_received_data(const uint8_t *data, size_t len);
    bool handle_get_info(const char *params);
    bool handle_vision_setting(const cJSON *params);
    bool handle_play_voice_prompt(const cJSON *root);
    bool send_config_info();
    bool send_detection_result();
    bool send_setting_result(bool success);
    bool send_speech_recognition();
    bool send_data(const std::vector<uint8_t> &data);
    bool forward_play_voice_prompt(uint16_t voice_data);
    bool encode_color_result(const who::detect::WhoDetect::result_t &result, std::vector<uint8_t> &data) const;
    bool encode_line_result(const who::detect::WhoDetect::result_t &result, std::vector<uint8_t> &data) const;

    who::communicate::WhoI2CSlaveTask *m_i2c_task;
    uint16_t m_result_width;
    config_getter_t m_config_getter;
    setting_handler_t m_setting_handler;
    result_getter_t m_result_getter;
};
