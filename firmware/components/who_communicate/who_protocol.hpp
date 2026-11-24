#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace who {
namespace communicate {
namespace protocol {

static constexpr uint8_t HEADER_0 = 0xAA;
static constexpr uint8_t HEADER_1 = 0x55;
static constexpr size_t MAX_FRAME_SIZE = 1024;
static constexpr size_t FRAME_OVERHEAD = 10;
static constexpr size_t DATA_FRAME_OVERHEAD = 9;
static constexpr size_t MAX_PAYLOAD_SIZE = MAX_FRAME_SIZE - FRAME_OVERHEAD;

struct Frame {
    uint8_t func = 0;
    uint16_t fragment_id = 1;
    uint16_t total_fragments = 1;
    std::vector<uint8_t> payload;
};

struct Message {
    uint8_t func = 0;
    std::vector<uint8_t> payload;
};

uint8_t checksum(const uint8_t *data, size_t len);
bool pack_frame(const Frame &frame, std::vector<uint8_t> &out);
bool pack_payload(uint8_t func, const uint8_t *payload, size_t payload_len, std::vector<std::vector<uint8_t>> &out);
bool pack_data_frame(const Frame &frame, size_t max_frame_size, std::vector<uint8_t> &out);
bool pack_data_payload(const uint8_t *payload,
                       size_t payload_len,
                       size_t max_frame_size,
                       uint16_t max_fragments,
                       std::vector<uint8_t> &out);
bool unpack_frame(const uint8_t *data, size_t len, Frame &frame);
bool assemble_payload(const std::vector<Frame> &frames, Message &message);

class StreamUnpacker {
public:
    void reset();
    void feed(const uint8_t *data, size_t len, std::vector<Frame> &frames);
    size_t pending_size() const;

private:
    std::vector<uint8_t> m_buffer;
};

class PayloadUnpacker {
public:
    void reset();
    void feed(const uint8_t *data, size_t len, std::vector<Message> &messages);
    bool has_pending_data() const;

private:
    void handle_frame(const Frame &frame, std::vector<Message> &messages);

    StreamUnpacker m_stream_unpacker;
    std::vector<Frame> m_fragments;
};

} // namespace protocol
} // namespace communicate
} // namespace who
