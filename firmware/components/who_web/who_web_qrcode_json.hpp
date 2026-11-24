#pragma once

#include <string>

#include "cJSON.h"
#include "who_qrcode.hpp"
#include "who_web_json.hpp"

namespace who {
namespace web {
namespace json {

inline std::string qrcode_objects(const who::qrcode::WhoQRCode::result_t &result)
{
    cJSON *array = cJSON_CreateArray();
    size_t index = 0;
    for (const auto &code : result.codes) {
        if (index++ >= 16) {
            break;
        }
        cJSON *obj = cJSON_CreateObject();

        std::string payload = code.payload;
        if (payload.size() > 1024) {
            payload.resize(1024);
            payload += "...";
        }
        const char *display_text = payload.empty() ? "QR" : payload.c_str();

        cJSON_AddStringToObject(obj, "label", display_text);
        cJSON_AddNumberToObject(obj, "index", static_cast<double>(index));

        cJSON *box = cJSON_CreateArray();
        cJSON_AddItemToArray(box, cJSON_CreateNumber(code.x));
        cJSON_AddItemToArray(box, cJSON_CreateNumber(code.y));
        cJSON_AddItemToArray(box, cJSON_CreateNumber(code.w));
        cJSON_AddItemToArray(box, cJSON_CreateNumber(code.h));
        cJSON_AddItemToObject(obj, "box", box);

        cJSON_AddStringToObject(obj, "text", display_text);
        cJSON_AddItemToArray(array, obj);
    }
    return print_json(array);
}

} // namespace json
} // namespace web
} // namespace who
