#include "who_speech_recognition_store.hpp"

#include <atomic>

namespace {
std::atomic<uint32_t> s_speech_recognition_result{0};

constexpr uint32_t VALID_BIT = 1UL << 16;
}

namespace who {
namespace communicate {

void store_speech_recognition_result(uint8_t data0, uint8_t data1)
{
    const uint32_t value = VALID_BIT |
                           (static_cast<uint32_t>(data0) << 8) |
                           static_cast<uint32_t>(data1);
    s_speech_recognition_result.store(value, std::memory_order_release);
}

bool consume_speech_recognition_result(uint8_t *data0, uint8_t *data1)
{
    const uint32_t value = s_speech_recognition_result.exchange(0, std::memory_order_acq_rel);
    if ((value & VALID_BIT) == 0) {
        return false;
    }
    if (data0) {
        *data0 = static_cast<uint8_t>((value >> 8) & 0xff);
    }
    if (data1) {
        *data1 = static_cast<uint8_t>(value & 0xff);
    }
    return true;
}

} // namespace communicate
} // namespace who
