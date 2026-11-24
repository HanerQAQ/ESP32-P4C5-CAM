#include "who_protocol.hpp"
#include <algorithm>
#include <cstring>
#include <utility>

namespace who {
namespace communicate {
namespace protocol {

namespace {
uint16_t read_le16(const uint8_t *data)
{
    return static_cast<uint16_t>(data[0]) | (static_cast<uint16_t>(data[1]) << 8);
}

void write_le16(uint8_t *data, uint16_t value)
{
    data[0] = value & 0xff;
    data[1] = (value >> 8) & 0xff;
}
} // namespace

uint8_t checksum(const uint8_t *data, size_t len)
{
    uint8_t value = 0;
    if (!data) {
        return value;
    }
    for (size_t i = 0; i < len; i++) {
        value ^= data[i];
    }
    return value;
}

bool pack_frame(const Frame &frame, std::vector<uint8_t> &out)
{
    if (frame.payload.size() > MAX_PAYLOAD_SIZE ||
        frame.fragment_id == 0 ||
        frame.total_fragments == 0 ||
        frame.fragment_id > frame.total_fragments) {
        return false;
    }

    uint16_t data_len = static_cast<uint16_t>(1 + frame.payload.size());
    out.assign(FRAME_OVERHEAD + frame.payload.size(), 0);
    out[0] = HEADER_0;
    out[1] = HEADER_1;
    write_le16(out.data() + 2, data_len);
    write_le16(out.data() + 4, frame.fragment_id);
    write_le16(out.data() + 6, frame.total_fragments);
    out[8] = frame.func;
    if (!frame.payload.empty()) {
        memcpy(out.data() + 9, frame.payload.data(), frame.payload.size());
    }
    out[9 + frame.payload.size()] = checksum(out.data() + 8, data_len);
    return true;
}

bool pack_payload(uint8_t func, const uint8_t *payload, size_t payload_len, std::vector<std::vector<uint8_t>> &out)
{
    if (!payload && payload_len > 0) {
        return false;
    }

    size_t total_fragments = payload_len == 0 ? 1 : (payload_len + MAX_PAYLOAD_SIZE - 1) / MAX_PAYLOAD_SIZE;
    if (total_fragments > UINT16_MAX) {
        return false;
    }

    out.clear();
    out.reserve(total_fragments);
    size_t offset = 0;
    for (uint16_t frag = 1; frag <= static_cast<uint16_t>(total_fragments); frag++) {
        size_t chunk_len = std::min(MAX_PAYLOAD_SIZE, payload_len - offset);
        Frame frame = {};
        frame.func = func;
        frame.fragment_id = frag;
        frame.total_fragments = static_cast<uint16_t>(total_fragments);
        if (chunk_len > 0) {
            frame.payload.assign(payload + offset, payload + offset + chunk_len);
        }

        std::vector<uint8_t> packed;
        if (!pack_frame(frame, packed)) {
            out.clear();
            return false;
        }
        out.push_back(std::move(packed));
        offset += chunk_len;
    }
    return true;
}

bool pack_data_frame(const Frame &frame, size_t max_frame_size, std::vector<uint8_t> &out)
{
    if (max_frame_size <= DATA_FRAME_OVERHEAD ||
        frame.payload.size() > max_frame_size - DATA_FRAME_OVERHEAD ||
        frame.fragment_id == 0 ||
        frame.total_fragments == 0 ||
        frame.fragment_id > frame.total_fragments) {
        return false;
    }

    uint16_t data_len = static_cast<uint16_t>(frame.payload.size());
    out.assign(DATA_FRAME_OVERHEAD + frame.payload.size(), 0);
    out[0] = HEADER_0;
    out[1] = HEADER_1;
    write_le16(out.data() + 2, data_len);
    write_le16(out.data() + 4, frame.fragment_id);
    write_le16(out.data() + 6, frame.total_fragments);
    if (!frame.payload.empty()) {
        memcpy(out.data() + 8, frame.payload.data(), frame.payload.size());
    }
    out[8 + frame.payload.size()] = checksum(out.data() + 8, data_len);
    return true;
}

bool pack_data_payload(const uint8_t *payload,
                       size_t payload_len,
                       size_t max_frame_size,
                       uint16_t max_fragments,
                       std::vector<uint8_t> &out)
{
    if ((!payload && payload_len > 0) || max_frame_size <= DATA_FRAME_OVERHEAD || max_fragments == 0) {
        return false;
    }

    const size_t max_payload_size = max_frame_size - DATA_FRAME_OVERHEAD;
    size_t total_fragments = payload_len == 0 ? 1 : (payload_len + max_payload_size - 1) / max_payload_size;
    if (total_fragments > max_fragments || total_fragments > UINT16_MAX) {
        return false;
    }

    out.clear();
    out.reserve(payload_len + total_fragments * DATA_FRAME_OVERHEAD);
    size_t offset = 0;
    for (uint16_t frag = 1; frag <= static_cast<uint16_t>(total_fragments); frag++) {
        size_t chunk_len = std::min(max_payload_size, payload_len - offset);
        Frame frame = {};
        frame.fragment_id = frag;
        frame.total_fragments = static_cast<uint16_t>(total_fragments);
        if (chunk_len > 0) {
            frame.payload.assign(payload + offset, payload + offset + chunk_len);
        }

        std::vector<uint8_t> packed;
        if (!pack_data_frame(frame, max_frame_size, packed)) {
            out.clear();
            return false;
        }
        out.insert(out.end(), packed.begin(), packed.end());
        offset += chunk_len;
    }
    return true;
}

bool unpack_frame(const uint8_t *data, size_t len, Frame &frame)
{
    if (!data || len < FRAME_OVERHEAD || data[0] != HEADER_0 || data[1] != HEADER_1) {
        return false;
    }

    uint16_t data_len = read_le16(data + 2);
    if (data_len == 0 || data_len > 1 + MAX_PAYLOAD_SIZE || len != static_cast<size_t>(data_len) + 9) {
        return false;
    }

    uint16_t fragment_id = read_le16(data + 4);
    uint16_t total_fragments = read_le16(data + 6);
    if (fragment_id == 0 || total_fragments == 0 || fragment_id > total_fragments) {
        return false;
    }

    uint8_t expected = checksum(data + 8, data_len);
    if (expected != data[8 + data_len]) {
        return false;
    }

    frame.func = data[8];
    frame.fragment_id = fragment_id;
    frame.total_fragments = total_fragments;
    frame.payload.assign(data + 9, data + 8 + data_len);
    return true;
}

bool assemble_payload(const std::vector<Frame> &frames, Message &message)
{
    if (frames.empty()) {
        return false;
    }

    const uint8_t func = frames.front().func;
    const uint16_t total_fragments = frames.front().total_fragments;
    if (total_fragments == 0 || frames.size() != total_fragments) {
        return false;
    }

    size_t payload_len = 0;
    for (size_t i = 0; i < frames.size(); i++) {
        const Frame &frame = frames[i];
        if (frame.func != func ||
            frame.total_fragments != total_fragments ||
            frame.fragment_id != static_cast<uint16_t>(i + 1)) {
            return false;
        }
        payload_len += frame.payload.size();
    }

    message.func = func;
    message.payload.clear();
    message.payload.reserve(payload_len);
    for (const auto &frame : frames) {
        message.payload.insert(message.payload.end(), frame.payload.begin(), frame.payload.end());
    }
    return true;
}

void StreamUnpacker::reset()
{
    m_buffer.clear();
}

size_t StreamUnpacker::pending_size() const
{
    return m_buffer.size();
}

void StreamUnpacker::feed(const uint8_t *data, size_t len, std::vector<Frame> &frames)
{
    frames.clear();
    if (!data || len == 0) {
        return;
    }

    m_buffer.insert(m_buffer.end(), data, data + len);
    while (true) {
        auto header = std::find(m_buffer.begin(), m_buffer.end(), HEADER_0);
        if (header == m_buffer.end()) {
            m_buffer.clear();
            return;
        }
        if (header != m_buffer.begin()) {
            m_buffer.erase(m_buffer.begin(), header);
        }
        if (m_buffer.size() < FRAME_OVERHEAD) {
            return;
        }
        if (m_buffer[1] != HEADER_1) {
            m_buffer.erase(m_buffer.begin());
            continue;
        }

        uint16_t data_len = read_le16(m_buffer.data() + 2);
        size_t frame_len = static_cast<size_t>(data_len) + 9;
        if (data_len == 0 || data_len > 1 + MAX_PAYLOAD_SIZE) {
            m_buffer.erase(m_buffer.begin());
            continue;
        }
        if (m_buffer.size() < frame_len) {
            return;
        }

        Frame frame;
        if (unpack_frame(m_buffer.data(), frame_len, frame)) {
            frames.push_back(std::move(frame));
            m_buffer.erase(m_buffer.begin(), m_buffer.begin() + frame_len);
        } else {
            m_buffer.erase(m_buffer.begin());
        }
    }
}

void PayloadUnpacker::reset()
{
    m_stream_unpacker.reset();
    m_fragments.clear();
}

bool PayloadUnpacker::has_pending_data() const
{
    return m_stream_unpacker.pending_size() > 0 || !m_fragments.empty();
}

void PayloadUnpacker::feed(const uint8_t *data, size_t len, std::vector<Message> &messages)
{
    messages.clear();
    std::vector<Frame> frames;
    m_stream_unpacker.feed(data, len, frames);
    for (const auto &frame : frames) {
        handle_frame(frame, messages);
    }
}

void PayloadUnpacker::handle_frame(const Frame &frame, std::vector<Message> &messages)
{
    if (frame.total_fragments == 1) {
        Message message;
        message.func = frame.func;
        message.payload = frame.payload;
        messages.push_back(std::move(message));
        return;
    }

    if (frame.fragment_id == 1) {
        m_fragments.clear();
    } else if (m_fragments.empty() ||
               m_fragments.front().func != frame.func ||
               m_fragments.front().total_fragments != frame.total_fragments ||
               frame.fragment_id != static_cast<uint16_t>(m_fragments.size() + 1)) {
        m_fragments.clear();
        return;
    }

    m_fragments.push_back(frame);
    if (m_fragments.size() == frame.total_fragments) {
        Message message;
        if (assemble_payload(m_fragments, message)) {
            messages.push_back(std::move(message));
        }
        m_fragments.clear();
    }
}

} // namespace protocol
} // namespace communicate
} // namespace who
