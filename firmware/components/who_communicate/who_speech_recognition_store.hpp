#pragma once

#include <cstdint>

namespace who {
namespace communicate {

void store_speech_recognition_result(uint8_t data0, uint8_t data1);
bool consume_speech_recognition_result(uint8_t *data0, uint8_t *data1);

} // namespace communicate
} // namespace who
