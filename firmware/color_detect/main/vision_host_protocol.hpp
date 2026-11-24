#pragma once

#include <cstdint>
#include <functional>
#include <string>

struct ColorConfig {
    uint8_t name;
    uint8_t hsv_min[3];
    uint8_t hsv_max[3];
    uint8_t box_color[3];
    int32_t area_thr;
};

struct VisionConfig {
    int32_t mode;
    uint8_t target_count;
    uint8_t targets[5];
    uint8_t color_count;
    ColorConfig colors[5];
};

class VisionHostProtocol {
public:
    using config_getter_t = std::function<VisionConfig(void)>;
    using config_saver_t = std::function<bool(const VisionConfig &config)>;

    VisionHostProtocol(const config_getter_t &getter, const config_saver_t &saver);

    std::string get_settings_json() const;
    bool save_settings_json(const std::string &json) const;

private:
    config_getter_t m_getter;
    config_saver_t m_saver;
};
