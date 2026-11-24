#include "vision_host_protocol.hpp"
#include "frame_cap_pipeline.hpp"
#include "cJSON.h"
#include <algorithm>
#include <cstdlib>

namespace {
constexpr int MAX_COLORS = 5;
constexpr int MAX_TARGETS = 5;

bool get_int(const cJSON *obj, const char *key, int min, int max, int *out)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (!cJSON_IsNumber(item) || item->valuedouble != item->valueint ||
        item->valueint < min || item->valueint > max) {
        return false;
    }
    *out = item->valueint;
    return true;
}

bool parse_u8_array3(const cJSON *obj, const char *key, int max, uint8_t out[3])
{
    const cJSON *array = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (!cJSON_IsArray(array) || cJSON_GetArraySize(array) != 3) {
        return false;
    }
    for (int i = 0; i < 3; i++) {
        const cJSON *item = cJSON_GetArrayItem(array, i);
        if (!cJSON_IsNumber(item) || item->valuedouble != item->valueint ||
            item->valueint < 0 || item->valueint > max) {
            return false;
        }
        out[i] = static_cast<uint8_t>(item->valueint);
    }
    return true;
}

bool parse_optional_int(const cJSON *obj, const char *key, int min, int max, int *out)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (!item) {
        return true;
    }
    return get_int(obj, key, min, max, out);
}

bool parse_color_config(const cJSON *obj, ColorConfig *config, bool strict_color_detect)
{
    if (!cJSON_IsObject(obj) || !config) {
        return false;
    }

    const cJSON *name_item = cJSON_GetObjectItemCaseSensitive(obj, "index");
    int name = -1;
    if (cJSON_IsNumber(name_item) && name_item->valuedouble == name_item->valueint) {
        name = name_item->valueint;
    } else {
        const char *name_text = cJSON_GetStringValue(name_item);
        if (name_text) {
            name = atoi(name_text);
        }
    }

    int area_threshold = config->area_thr > 0 ? config->area_thr : 256;
    if (name < 1 || name > MAX_COLORS ||
        !parse_u8_array3(obj, "hsv_min", 255, config->hsv_min) ||
        !parse_u8_array3(obj, "hsv_max", 255, config->hsv_max) ||
        !parse_u8_array3(obj, "box_color", 255, config->box_color) ||
        !parse_optional_int(obj, "area_thr", 1, MODEL_INPUT_W * MODEL_INPUT_H - 1, &area_threshold)) {
        return false;
    }
    if (strict_color_detect &&
        (config->hsv_min[0] > 180 || config->hsv_max[0] > 180 ||
         config->hsv_min[1] >= config->hsv_max[1] || config->hsv_min[2] >= config->hsv_max[2])) {
        return false;
    }
    if (!strict_color_detect &&
        (config->hsv_min[1] > config->hsv_max[1] || config->hsv_min[2] > config->hsv_max[2])) {
        return false;
    }

    config->hsv_min[0] = std::min<uint8_t>(config->hsv_min[0], 180);
    config->hsv_max[0] = std::min<uint8_t>(config->hsv_max[0], 180);
    config->name = static_cast<uint8_t>(name);
    config->area_thr = area_threshold;
    return true;
}

int find_color_index_by_name(const VisionConfig &config, uint8_t name)
{
    for (int i = 0; i < config.color_count; i++) {
        if (config.colors[i].name == name) {
            return i;
        }
    }
    return -1;
}

bool validate_targets(const VisionConfig &config)
{
    if (config.target_count == 0 || config.target_count > MAX_TARGETS ||
        (config.mode == 1 && config.target_count != 1)) {
        return false;
    }
    for (int i = 0; i < config.target_count; i++) {
        if (config.targets[i] < 1 || config.targets[i] > MAX_COLORS ||
            find_color_index_by_name(config, config.targets[i]) < 0) {
            return false;
        }
    }
    return true;
}

bool parse_settings_json(const std::string &json, const VisionConfig &base_config, VisionConfig *config)
{
    cJSON *root = cJSON_ParseWithLength(json.data(), json.size());
    if (!root) {
        return false;
    }

    bool valid = false;
    const cJSON *params = cJSON_GetObjectItemCaseSensitive(root, "params");
    const cJSON *targets = cJSON_GetObjectItemCaseSensitive(params, "target");
    const cJSON *colors = cJSON_GetObjectItemCaseSensitive(params, "color_config");
    if (cJSON_IsObject(params) && config) {
        VisionConfig parsed = base_config;
        int mode = parsed.mode;
        valid = parse_optional_int(params, "mode", 0, 1, &mode);
        parsed.mode = mode;

        if (valid && targets) {
            const int target_count = cJSON_GetArraySize(targets);
            valid = cJSON_IsArray(targets) && target_count > 0 && target_count <= MAX_TARGETS;
            parsed.target_count = static_cast<uint8_t>(target_count);
            for (int i = 0; valid && i < target_count; i++) {
                const cJSON *target = cJSON_GetArrayItem(targets, i);
                valid = cJSON_IsNumber(target) && target->valuedouble == target->valueint &&
                        target->valueint >= 1 && target->valueint <= MAX_COLORS;
                if (valid) {
                    parsed.targets[i] = static_cast<uint8_t>(target->valueint);
                }
            }
        }

        if (valid && colors) {
            const int color_count = cJSON_GetArraySize(colors);
            valid = cJSON_IsArray(colors) && color_count > 0 && color_count <= MAX_COLORS;
            VisionConfig updated = parsed;
            updated.color_count = static_cast<uint8_t>(color_count);
            for (int i = 0; valid && i < color_count; i++) {
                const cJSON *item = cJSON_GetArrayItem(colors, i);
                ColorConfig color = {};
                const cJSON *name_item = cJSON_GetObjectItemCaseSensitive(item, "index");
                int name = -1;
                if (cJSON_IsNumber(name_item) && name_item->valuedouble == name_item->valueint) {
                    name = name_item->valueint;
                } else {
                    const char *name_text = cJSON_GetStringValue(name_item);
                    if (name_text) {
                        name = atoi(name_text);
                    }
                }
                int old_idx = name >= 0 ? find_color_index_by_name(parsed, static_cast<uint8_t>(name)) : -1;
                if (old_idx >= 0) {
                    color = parsed.colors[old_idx];
                }
                valid = parse_color_config(item, &color, parsed.mode == 0);
                if (valid) {
                    updated.colors[i] = color;
                }
            }
            if (valid) {
                parsed = updated;
            }
        }

        valid = valid && validate_targets(parsed);
        if (valid && colors) {
            *config = parsed;
        }
    }

    cJSON_Delete(root);
    return valid;
}
} // namespace

VisionHostProtocol::VisionHostProtocol(const config_getter_t &getter, const config_saver_t &saver) :
    m_getter(getter),
    m_saver(saver)
{
}

std::string VisionHostProtocol::get_settings_json() const
{
    if (!m_getter) {
        return {};
    }
    const VisionConfig config = m_getter();

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "command", "vision_setting");
    cJSON *params = cJSON_AddArrayToObject(root, "params");
    for (int i = 0; i < config.color_count; i++) {
        const auto &color = config.colors[i];
        cJSON *item = cJSON_CreateObject();
        cJSON_AddNumberToObject(item, "index", color.name > 0 ? color.name : (i + 1));
        cJSON *hsv_min = cJSON_AddArrayToObject(item, "hsv_min");
        cJSON *hsv_max = cJSON_AddArrayToObject(item, "hsv_max");
        cJSON *box_color = cJSON_AddArrayToObject(item, "box_color");
        for (int j = 0; j < 3; j++) {
            cJSON_AddItemToArray(hsv_min, cJSON_CreateNumber(color.hsv_min[j]));
            cJSON_AddItemToArray(hsv_max, cJSON_CreateNumber(color.hsv_max[j]));
            cJSON_AddItemToArray(box_color, cJSON_CreateNumber(color.box_color[j]));
        }
        cJSON_AddNumberToObject(item, "area_thr", color.area_thr);
        cJSON_AddItemToArray(params, item);
    }

    char *text = cJSON_PrintUnformatted(root);
    std::string json = text ? text : "";
    cJSON_free(text);
    cJSON_Delete(root);
    return json;
}

bool VisionHostProtocol::save_settings_json(const std::string &json) const
{
    VisionConfig config = m_getter ? m_getter() : VisionConfig{};
    return m_saver && parse_settings_json(json, config, &config) && m_saver(config);
}
