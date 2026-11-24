#pragma once

#include <string>

#include "cJSON.h"

namespace who {
namespace web {
namespace json {

inline std::string print_json(cJSON *item)
{
    if (!item) {
        return "[]";
    }
    char *printed = cJSON_PrintUnformatted(item);
    std::string out = printed ? printed : "[]";
    cJSON_free(printed);
    cJSON_Delete(item);
    return out;
}

} // namespace json
} // namespace web
} // namespace who
