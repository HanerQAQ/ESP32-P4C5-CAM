#pragma once

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <initializer_list>
#include <string>
#include <vector>

#include "cJSON.h"
#include "who_detect.hpp"
#include "who_web_json.hpp"

namespace who {
namespace web {
namespace json {

inline void add_int_array(cJSON *obj, const char *name, std::initializer_list<int> values)
{
    cJSON *array = cJSON_AddArrayToObject(obj, name);
    for (int value : values) {
        cJSON_AddItemToArray(array, cJSON_CreateNumber(value));
    }
}

inline std::string detect_objects(const who::detect::WhoDetect::result_t &result,
                                  const std::vector<std::string> &labels,
                                  const std::vector<std::string> &recognition = {})
{
    cJSON *array = cJSON_CreateArray();
    size_t index = 0;
    for (const auto &item : result.det_res) {
        if (index >= 64) {
            break;
        }
        cJSON *obj = cJSON_CreateObject();

        std::string label;
        if (index < recognition.size() && !recognition[index].empty()) {
            label = recognition[index];
        } else if (item.category >= 0 && static_cast<size_t>(item.category) < labels.size()) {
            label = labels[item.category];
        } else {
            char fallback[24];
            snprintf(fallback, sizeof(fallback), "class %d", item.category);
            label = fallback;
        }
        cJSON_AddStringToObject(obj, "label", label.c_str());
        cJSON_AddNumberToObject(obj, "category", item.category);
        cJSON_AddNumberToObject(obj, "score", item.score);

        const char *color_str = nullptr;
        if (label == "Red" || label == "red") color_str = "#ff0000";
        else if (label == "Yellow" || label == "yellow") color_str = "#ffff00";
        else if (label == "Green" || label == "green") color_str = "#00ff00";
        else if (label == "Blue" || label == "blue") color_str = "#0000ff";
        else if (label == "Purple" || label == "purple") color_str = "#ff00ff";
        if (color_str) {
            cJSON_AddStringToObject(obj, "color", color_str);
        }

        int x = 0, y = 0, x2 = 0, y2 = 0;
        if (item.box.size() >= 4) {
            x = item.box[0];
            y = item.box[1];
            x2 = item.box[2];
            y2 = item.box[3];
        }
        add_int_array(obj, "box", {x, y, std::max(0, x2 - x), std::max(0, y2 - y)});

        if (item.keypoint.size() >= 4 && item.keypoint.size() % 2 == 0) {
            cJSON *keypoints = cJSON_AddArrayToObject(obj, "keypoints");
            for (size_t i = 0; i + 1 < item.keypoint.size(); i += 2) {
                cJSON *point = cJSON_CreateArray();
                cJSON_AddItemToArray(point, cJSON_CreateNumber(item.keypoint[i]));
                cJSON_AddItemToArray(point, cJSON_CreateNumber(item.keypoint[i + 1]));
                cJSON_AddItemToArray(keypoints, point);
            }
        }

        cJSON_AddItemToArray(array, obj);
        index++;
    }
    return print_json(array);
}

inline std::string line_objects(const who::detect::WhoDetect::result_t &result,
                                const std::string &label,
                                int image_width = 0)
{
    cJSON *array = cJSON_CreateArray();
    if (!result.det_res.empty()) {
        const auto &item = result.det_res.front();
        if (item.keypoint.size() >= 4) {
            const int x0 = item.keypoint[0];
            const int y0 = item.keypoint[1];
            const int x1 = item.keypoint[2];
            const int y1 = item.keypoint[3];
            cJSON *obj = cJSON_CreateObject();
            cJSON_AddStringToObject(obj, "label", label.c_str());
            cJSON_AddNumberToObject(obj, "score", item.score);
            cJSON_AddItemToObject(obj, "line",
                                  cJSON_CreateIntArray(&item.keypoint[0], 4));
            if (image_width > 0) {
                constexpr float rad_to_deg = 57.2957795f;
                const float dx = static_cast<float>(x1 - x0);
                const float dy = static_cast<float>(y1 - y0);
                const float angle = std::atan2(dx, dy) * rad_to_deg;
                const float center_x = (static_cast<float>(x0) + static_cast<float>(x1)) * 0.5f;
                float offset = (center_x - image_width * 0.5f) / (image_width * 0.5f);
                offset = std::max(-1.0f, std::min(1.0f, offset));
                cJSON_AddNumberToObject(obj, "angle", angle);
                cJSON_AddNumberToObject(obj, "offset", offset);
            }
            cJSON_AddItemToArray(array, obj);
        }
    }
    return print_json(array);
}

inline const std::vector<std::string> &coco_labels()
{
    static const std::vector<std::string> labels = {
        "person", "bicycle", "car", "motorcycle", "airplane", "bus", "train", "truck", "boat",
        "traffic light", "fire hydrant", "stop sign", "parking meter", "bench", "bird", "cat",
        "dog", "horse", "sheep", "cow", "elephant", "bear", "zebra", "giraffe", "backpack",
        "umbrella", "handbag", "tie", "suitcase", "frisbee", "skis", "snowboard", "sports ball",
        "kite", "baseball bat", "baseball glove", "skateboard", "surfboard", "tennis racket",
        "bottle", "wine glass", "cup", "fork", "knife", "spoon", "bowl", "banana", "apple",
        "sandwich", "orange", "broccoli", "carrot", "hot dog", "pizza", "donut", "cake", "chair",
        "couch", "potted plant", "bed", "dining table", "toilet", "tv", "laptop", "mouse",
        "remote", "keyboard", "cell phone", "microwave", "oven", "toaster", "sink",
        "refrigerator", "book", "clock", "vase", "scissors", "teddy bear", "hair drier",
        "toothbrush"};
    return labels;
}

} // namespace json
} // namespace web
} // namespace who
