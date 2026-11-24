#pragma once

#include <cstdio>
#include <string>

#include "cJSON.h"
#include "who_apriltag.hpp"
#include "who_web_json.hpp"

namespace who {
namespace web {
namespace json {

inline std::string apriltag_objects(const who::apriltag::WhoApriltag::result_t &result)
{
    cJSON *array = cJSON_CreateArray();
    size_t index = 0;
    for (const auto &tag : result.tags) {
        if (index++ >= 64) {
            break;
        }
        cJSON *obj = cJSON_CreateObject();
        char label[24];
        snprintf(label, sizeof(label), "Tag %d", tag.id);
        cJSON_AddStringToObject(obj, "label", label);
        cJSON_AddNumberToObject(obj, "id", tag.id);

        cJSON *box = cJSON_CreateArray();
        cJSON_AddItemToArray(box, cJSON_CreateNumber(tag.x1));
        cJSON_AddItemToArray(box, cJSON_CreateNumber(tag.y1));
        cJSON_AddItemToArray(box, cJSON_CreateNumber(tag.x2 - tag.x1));
        cJSON_AddItemToArray(box, cJSON_CreateNumber(tag.y2 - tag.y1));
        cJSON_AddItemToObject(obj, "box", box);

        cJSON_AddNumberToObject(obj, "rotation", tag.rotation);
        cJSON *pose = cJSON_AddObjectToObject(obj, "pose");
        cJSON_AddNumberToObject(pose, "x", tag.x_translation);
        cJSON_AddNumberToObject(pose, "y", tag.y_translation);
        cJSON_AddNumberToObject(pose, "z", tag.z_translation);
        cJSON_AddNumberToObject(pose, "roll", tag.x_rotation);
        cJSON_AddNumberToObject(pose, "pitch", tag.y_rotation);
        cJSON_AddNumberToObject(pose, "yaw", tag.z_rotation);
        cJSON_AddItemToArray(array, obj);
    }
    return print_json(array);
}

} // namespace json
} // namespace web
} // namespace who
