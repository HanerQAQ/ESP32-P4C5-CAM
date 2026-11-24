#pragma once

#include "driver/usb_serial_jtag.h"
#include "esp_err.h"
#include "who_frame_cap.hpp"
#include "who_protocol.hpp"
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>

#ifndef WHO_COLOR_ADJUST_HOST_SERIAL_DEFAULT_ENABLED
#define WHO_COLOR_ADJUST_HOST_SERIAL_DEFAULT_ENABLED 1
#endif

struct cJSON;

namespace who {
namespace communicate {

struct WhoColorAdjustConfig {
    int baud_rate = 921600;
    bool host_serial_enabled = WHO_COLOR_ADJUST_HOST_SERIAL_DEFAULT_ENABLED;
};

class WhoColorAdjustTask : public task::WhoTask {
public:
    using recv_cb_t = std::function<void(const uint8_t *data, size_t len)>;
    using settings_getter_t = std::function<std::string(void)>;
    using settings_saver_t = std::function<bool(const std::string &json)>;

    WhoColorAdjustTask(const std::string &name, const WhoColorAdjustConfig &config);
    ~WhoColorAdjustTask();

    bool send(const uint8_t *data, size_t len, TickType_t timeout = 0);
    bool send(const char *data, TickType_t timeout = 0);
    void set_recv_cb(const recv_cb_t &recv_cb);
    void set_settings_cbs(const settings_getter_t &getter, const settings_saver_t &saver);
    void set_frame_cap_node(frame_cap::WhoFrameCapNode *frame_cap_node);
    who::cam::cam_fb_t *get_frame(int index = -1);

private:
    static constexpr size_t TX_CHUNK_SIZE = 256;
    static constexpr size_t RX_BUFFER_SIZE = 1024;
    static constexpr size_t CDC_RX_BUFFER_SIZE = 1024;
    static constexpr size_t CDC_TX_BUFFER_SIZE = 4096;
    static constexpr size_t TX_QUEUE_SIZE = 8;
    static constexpr TickType_t IDLE_DELAY = pdMS_TO_TICKS(1);
    static constexpr TickType_t PROTOCOL_FRAME_DELAY = pdMS_TO_TICKS(3);

    struct tx_msg_t {
        size_t len;
        uint8_t data[TX_CHUNK_SIZE];
    };

    void task() override;
    void cleanup() override;
    void handle_pause();
    esp_err_t init_usb_cdc();
    void drain_tx_queue();
    void handle_rx_data(const uint8_t *data, size_t len);
    bool extract_json_command(std::string &json);
    bool handle_protocol_message(const protocol::Message &message);
    bool handle_protocol_command(const std::string &json);
    bool forward_play_voice_prompt_frame(const uint8_t *data, size_t len);
    bool send_frame_jpeg();
    bool send_protocol_payload(uint8_t func, const uint8_t *payload, size_t payload_len);
    bool send_protocol_json(uint8_t func, const char *json);
    bool crop_rgb565_center_resize_640x480_to_rgb888(const who::cam::cam_fb_t *fb, uint8_t *dst, size_t dst_len);
    bool encode_rgb888_to_jpeg(const uint8_t *rgb888, size_t rgb888_len, uint8_t *jpeg_buf, size_t jpeg_buf_size, int *jpeg_len);
    void write_usb_data(const uint8_t *data, size_t len);
    void write_usb_text(const char *text);

    static constexpr int JPEG_WIDTH = 640;
    static constexpr int JPEG_HEIGHT = 480;
    static constexpr size_t JPEG_RGB888_LEN = JPEG_WIDTH * JPEG_HEIGHT * 3;
    static constexpr size_t JPEG_OUTPUT_BUF_LEN = JPEG_RGB888_LEN + 4096;
    alignas(16) uint8_t m_rgb888_buf[JPEG_RGB888_LEN];
    alignas(16) uint8_t m_jpeg_buf[JPEG_OUTPUT_BUF_LEN];

    WhoColorAdjustConfig m_config;
    QueueHandle_t m_tx_queue;
    SemaphoreHandle_t m_cb_mutex;
    recv_cb_t m_recv_cb;
    settings_getter_t m_settings_getter;
    settings_saver_t m_settings_saver;
    frame_cap::WhoFrameCapNode *m_frame_cap_node;
    bool m_usb_cdc_inited;
    bool m_usb_cdc_driver_installed;
    protocol::PayloadUnpacker m_payload_unpacker;
    std::string m_rx_accum;
};

} // namespace communicate
} // namespace who
