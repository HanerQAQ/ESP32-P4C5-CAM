#include "who_color_adjust_task.hpp"
#include "who_ci1302_task.hpp"
#include "who_protocol.hpp"
#include "cJSON.h"
#include "esp_check.h"
#include "esp_jpeg_enc.h"
#include "esp_log.h"
#include <algorithm>
#include <cassert>
#include <cstring>

static const char *TAG = "WhoColorAdjustTask";

namespace who {
namespace communicate {

WhoColorAdjustTask::WhoColorAdjustTask(const std::string &name, const WhoColorAdjustConfig &config) :
    task::WhoTask(name),
    m_config(config),
    m_tx_queue(xQueueCreate(TX_QUEUE_SIZE, sizeof(tx_msg_t))),
    m_cb_mutex(xSemaphoreCreateMutex()),
    m_frame_cap_node(nullptr),
    m_usb_cdc_inited(false),
    m_usb_cdc_driver_installed(false),
    m_payload_unpacker(),
    m_rx_accum()
{
    assert(m_tx_queue);
    assert(m_cb_mutex);
}

WhoColorAdjustTask::~WhoColorAdjustTask()
{
    vQueueDelete(m_tx_queue);
    vSemaphoreDelete(m_cb_mutex);
}

bool WhoColorAdjustTask::send(const uint8_t *data, size_t len, TickType_t timeout)
{
    if (!m_config.host_serial_enabled) {
        return false;
    }
    if (!data || len == 0) {
        return false;
    }

    size_t offset = 0;
    while (offset < len) {
        tx_msg_t msg = {};
        msg.len = std::min(TX_CHUNK_SIZE, len - offset);
        memcpy(msg.data, data + offset, msg.len);
        if (xQueueSend(m_tx_queue, &msg, timeout) != pdTRUE) {
            return false;
        }
        offset += msg.len;
    }

    return true;
}

bool WhoColorAdjustTask::send(const char *data, TickType_t timeout)
{
    if (!data) {
        return false;
    }

    return send(reinterpret_cast<const uint8_t *>(data), strlen(data), timeout);
}

void WhoColorAdjustTask::set_recv_cb(const recv_cb_t &recv_cb)
{
    xSemaphoreTake(m_cb_mutex, portMAX_DELAY);
    m_recv_cb = recv_cb;
    xSemaphoreGive(m_cb_mutex);
}

void WhoColorAdjustTask::set_settings_cbs(const settings_getter_t &getter, const settings_saver_t &saver)
{
    xSemaphoreTake(m_cb_mutex, portMAX_DELAY);
    m_settings_getter = getter;
    m_settings_saver = saver;
    xSemaphoreGive(m_cb_mutex);
}

void WhoColorAdjustTask::set_frame_cap_node(frame_cap::WhoFrameCapNode *frame_cap_node)
{
    xSemaphoreTake(m_cb_mutex, portMAX_DELAY);
    m_frame_cap_node = frame_cap_node;
    xSemaphoreGive(m_cb_mutex);
}

who::cam::cam_fb_t *WhoColorAdjustTask::get_frame(int index)
{
    xSemaphoreTake(m_cb_mutex, portMAX_DELAY);
    auto *frame_cap_node = m_frame_cap_node;
    xSemaphoreGive(m_cb_mutex);

    if (!frame_cap_node) {
        return nullptr;
    }

    return frame_cap_node->cam_fb_peek(index);
}

void WhoColorAdjustTask::drain_tx_queue()
{
    if (!m_config.host_serial_enabled) {
        xQueueReset(m_tx_queue);
        return;
    }

    tx_msg_t msg = {};
    while (xQueueReceive(m_tx_queue, &msg, 0) == pdTRUE) {
        write_usb_data(msg.data, msg.len);
    }
}

void WhoColorAdjustTask::handle_pause()
{
    xEventGroupSetBits(m_event_group, TASK_PAUSED);
    EventBits_t pause_event_bits =
        xEventGroupWaitBits(m_event_group, TASK_RESUME | TASK_STOP, pdTRUE, pdFALSE, portMAX_DELAY);
    if (pause_event_bits & TASK_STOP) {
        xEventGroupSetBits(m_event_group, TASK_STOP);
    }
}

esp_err_t WhoColorAdjustTask::init_usb_cdc()
{
    if (!m_config.host_serial_enabled) {
        return ESP_OK;
    }
    if (m_usb_cdc_inited) {
        return ESP_OK;
    }

    usb_serial_jtag_driver_config_t usb_cdc_config = {
        .tx_buffer_size = CDC_TX_BUFFER_SIZE,
        .rx_buffer_size = CDC_RX_BUFFER_SIZE,
    };

    if (!usb_serial_jtag_is_driver_installed()) {
        ESP_RETURN_ON_ERROR(usb_serial_jtag_driver_install(&usb_cdc_config), TAG, "install USB CDC driver failed");
        m_usb_cdc_driver_installed = true;
    }

    (void)m_config;

    m_usb_cdc_inited = true;
    return ESP_OK;
}


void WhoColorAdjustTask::write_usb_data(const uint8_t *data, size_t len)
{
    if (!m_config.host_serial_enabled) {
        return;
    }
    if (!data || len == 0) {
        return;
    }

    size_t offset = 0;
    while (offset < len) {
        size_t chunk_len = std::min(TX_CHUNK_SIZE, len - offset);
        int written = usb_serial_jtag_write_bytes(data + offset, chunk_len, pdMS_TO_TICKS(100));
        if (written <= 0) {
            vTaskDelay(IDLE_DELAY);
            continue;
        }
        offset += static_cast<size_t>(written);
    }
}

void WhoColorAdjustTask::write_usb_text(const char *text)
{
    if (!text) {
        return;
    }
    write_usb_data(reinterpret_cast<const uint8_t *>(text), strlen(text));
}

void WhoColorAdjustTask::handle_rx_data(const uint8_t *data, size_t len)
{
    if (forward_play_voice_prompt_frame(data, len)) {
        return;
    }

    std::vector<protocol::Message> messages;
    m_payload_unpacker.feed(data, len, messages);
    if (!messages.empty()) {
        xSemaphoreTake(m_cb_mutex, portMAX_DELAY);
        auto recv_cb = m_recv_cb;
        xSemaphoreGive(m_cb_mutex);

        for (const auto &message : messages) {
            if (!handle_protocol_message(message) && recv_cb) {
                recv_cb(message.payload.data(), message.payload.size());
            }
        }
        return;
    }
    if (m_payload_unpacker.has_pending_data()) {
        return;
    }

    m_rx_accum.append(reinterpret_cast<const char *>(data), len);
    if (m_rx_accum.size() > 4096) {
        m_rx_accum.erase(0, m_rx_accum.size() - 4096);
    }

    std::string json;
    bool handled = false;
    while (extract_json_command(json)) {
        handled |= handle_protocol_command(json);
    }

    if (!handled) {
        xSemaphoreTake(m_cb_mutex, portMAX_DELAY);
        auto recv_cb = m_recv_cb;
        xSemaphoreGive(m_cb_mutex);
        if (recv_cb) {
            recv_cb(data, len);
        }
        return;
    }
}

bool WhoColorAdjustTask::handle_protocol_message(const protocol::Message &message)
{
    if (message.payload.empty()) {
        return false;
    }

    std::string json(reinterpret_cast<const char *>(message.payload.data()), message.payload.size());
    return handle_protocol_command(json);
}

bool WhoColorAdjustTask::extract_json_command(std::string &json)
{
    size_t start = m_rx_accum.find('{');
    if (start == std::string::npos) {
        m_rx_accum.clear();
        return false;
    }
    if (start > 0) {
        m_rx_accum.erase(0, start);
    }

    int depth = 0;
    bool in_string = false;
    bool escape = false;
    for (size_t i = 0; i < m_rx_accum.size(); i++) {
        char ch = m_rx_accum[i];
        if (escape) {
            escape = false;
            continue;
        }
        if (in_string && ch == '\\') {
            escape = true;
            continue;
        }
        if (ch == '"') {
            in_string = !in_string;
            continue;
        }
        if (in_string) {
            continue;
        }
        if (ch == '{') {
            depth++;
        } else if (ch == '}') {
            depth--;
            if (depth == 0) {
                json = m_rx_accum.substr(0, i + 1);
                m_rx_accum.erase(0, i + 1);
                return true;
            }
        }
    }
    return false;
}

bool WhoColorAdjustTask::handle_protocol_command(const std::string &json)
{
    cJSON *root = cJSON_ParseWithLength(json.data(), json.size());
    if (!root) {
        return false;
    }

    bool handled = false;
    const cJSON *command = cJSON_GetObjectItemCaseSensitive(root, "command");
    const cJSON *params = cJSON_GetObjectItemCaseSensitive(root, "params");
    const char *command_text = cJSON_GetStringValue(command);
    if (command_text && strcmp(command_text, "vision_setting") == 0 && params) {
        if (cJSON_IsString(params)) {
            const char *param_text = cJSON_GetStringValue(params);
            if (param_text && strcmp(param_text, "get_img_frame") == 0) {
                handled = true;
                if (!send_frame_jpeg()) {
                    ESP_LOGW(TAG, "get_img_frame failed");
                }
            } else if (param_text && (strcmp(param_text, "get_color_config") == 0 ||
                                      strcmp(param_text, "get_setting") == 0 ||
                                      strcmp(param_text, "get_settings") == 0 ||
                                      strcmp(param_text, "read_setting") == 0 ||
                                      strcmp(param_text, "read_settings") == 0)) {
                xSemaphoreTake(m_cb_mutex, portMAX_DELAY);
                auto getter = m_settings_getter;
                xSemaphoreGive(m_cb_mutex);
                if (getter) {
                    std::string response = getter();
                    handled = send_protocol_json(0x01, response.c_str());
                }
            }
        } else if (cJSON_IsObject(params)) {
            xSemaphoreTake(m_cb_mutex, portMAX_DELAY);
            auto saver = m_settings_saver;
            xSemaphoreGive(m_cb_mutex);
            bool is_save = saver ? saver(json) : false;
            const char *response = is_save ?
                "{\"command\":\"vision_setting\",\"params\":{\"is_save\":true}}" :
                "{\"command\":\"vision_setting\",\"params\":{\"is_save\":false}}";
            handled = send_protocol_json(0x02, response);
        }
    }

    cJSON_Delete(root);
    return handled;
}

bool WhoColorAdjustTask::forward_play_voice_prompt_frame(const uint8_t *data, size_t len)
{
    if (!data || len != 5 ||
        data[0] != 0xAA || data[1] != 0x55 || data[2] != 0xFF || data[4] != 0xFB) {
        return false;
    }

    return WhoCi1302Task::get_instance()->send(data, len);
}

bool WhoColorAdjustTask::send_frame_jpeg()
{
    if (!m_config.host_serial_enabled) {
        return false;
    }
    auto *fb = get_frame();
    if (!fb) {
        ESP_LOGW(TAG, "No frame available");
        return false;
    }
    if (fb->format != who::cam::cam_fb_fmt_t::CAM_FB_FMT_RGB565) {
        ESP_LOGW(TAG, "Unsupported frame format: %d", static_cast<int>(fb->format));
        return false;
    }

    uint8_t *rgb888 = m_rgb888_buf;
    uint8_t *jpeg_buf = m_jpeg_buf;
    int jpeg_len = 0;
    bool ok = crop_rgb565_center_resize_640x480_to_rgb888(fb, rgb888, JPEG_RGB888_LEN) &&
              encode_rgb888_to_jpeg(rgb888, JPEG_RGB888_LEN, jpeg_buf, JPEG_OUTPUT_BUF_LEN, &jpeg_len);

    if (!ok || jpeg_len <= 0) {
        return false;
    }

    return send_protocol_payload(0x00, jpeg_buf, static_cast<size_t>(jpeg_len));
}

bool WhoColorAdjustTask::send_protocol_json(uint8_t func, const char *json)
{
    if (!json) {
        return false;
    }
    return send_protocol_payload(func, reinterpret_cast<const uint8_t *>(json), strlen(json));
}

bool WhoColorAdjustTask::send_protocol_payload(uint8_t func, const uint8_t *payload, size_t payload_len)
{
    std::vector<std::vector<uint8_t>> frames;
    if (!protocol::pack_payload(func, payload, payload_len, frames)) {
        ESP_LOGE(TAG, "Protocol payload too large: %u", static_cast<unsigned>(payload_len));
        return false;
    }

    for (const auto &frame : frames) {
        write_usb_data(frame.data(), frame.size());
        vTaskDelay(PROTOCOL_FRAME_DELAY);
    }
    return true;
}

bool WhoColorAdjustTask::crop_rgb565_center_resize_640x480_to_rgb888(const who::cam::cam_fb_t *fb, uint8_t *dst, size_t dst_len)
{
    constexpr int DST_W = 640;
    constexpr int DST_H = 480;
    constexpr int SRC_BYTES_PER_PIXEL = 2;
    constexpr int DST_BYTES_PER_PIXEL = 3;
    if (!fb || !fb->buf || !dst || dst_len < DST_W * DST_H * DST_BYTES_PER_PIXEL ||
        fb->width < DST_W || fb->height < DST_H) {
        ESP_LOGW(TAG,
                 "UART frame must be at least %dx%d RGB565, got %dx%d",
                 DST_W,
                 DST_H,
                 fb ? fb->width : 0,
                 fb ? fb->height : 0);
        return false;
    }

    int crop_w = fb->width;
    int crop_h = crop_w * DST_H / DST_W;
    if (crop_h > fb->height) {
        crop_h = fb->height;
        crop_w = crop_h * DST_W / DST_H;
    }
    const int src_x = (fb->width - crop_w) / 2;
    const int src_y = (fb->height - crop_h) / 2;
    const uint8_t *src = static_cast<const uint8_t *>(fb->buf);
    const size_t src_stride = fb->width * SRC_BYTES_PER_PIXEL;
    const size_t dst_stride = DST_W * DST_BYTES_PER_PIXEL;

    for (int y = 0; y < DST_H; y++) {
        const int sample_y = src_y + (DST_H - 1 - y) * crop_h / DST_H;
        const uint8_t *src_row = src + sample_y * src_stride + src_x * SRC_BYTES_PER_PIXEL;
        uint8_t *dst_row = dst + y * dst_stride;
        for (int x = 0; x < DST_W; x++) {
            const int sample_x = (DST_W - 1 - x) * crop_w / DST_W;
            const uint8_t *src_pixel = src_row + sample_x * SRC_BYTES_PER_PIXEL;
            uint16_t pixel = src_pixel[0] | (static_cast<uint16_t>(src_pixel[1]) << 8);
            uint8_t r5 = (pixel >> 11) & 0x1f;
            uint8_t g6 = (pixel >> 5) & 0x3f;
            uint8_t b5 = pixel & 0x1f;
            dst_row[x * DST_BYTES_PER_PIXEL] = (r5 << 3) | (r5 >> 2);
            dst_row[x * DST_BYTES_PER_PIXEL + 1] = (g6 << 2) | (g6 >> 4);
            dst_row[x * DST_BYTES_PER_PIXEL + 2] = (b5 << 3) | (b5 >> 2);
        }
    }
    return true;
}

bool WhoColorAdjustTask::encode_rgb888_to_jpeg(const uint8_t *rgb888, size_t rgb888_len, uint8_t *jpeg_buf, size_t jpeg_buf_size, int *jpeg_len)
{
    if (!rgb888 || rgb888_len != JPEG_RGB888_LEN || !jpeg_buf || !jpeg_len) {
        return false;
    }

    jpeg_enc_config_t cfg = DEFAULT_JPEG_ENC_CONFIG();
    cfg.width = JPEG_WIDTH;
    cfg.height = JPEG_HEIGHT;
        cfg.src_type = JPEG_PIXEL_FORMAT_RGB888;
    cfg.subsampling = JPEG_SUBSAMPLE_420;
        cfg.quality = 80;
    cfg.task_enable = false;

    jpeg_enc_handle_t jpeg_enc = nullptr;
    if (jpeg_enc_open(&cfg, &jpeg_enc) != JPEG_ERR_OK) {
        ESP_LOGE(TAG, "Failed to open JPEG encoder");
        return false;
    }

    int out_len = 0;
    jpeg_error_t ret = jpeg_enc_process(jpeg_enc, rgb888, static_cast<int>(rgb888_len), jpeg_buf, static_cast<int>(jpeg_buf_size), &out_len);
    jpeg_enc_close(jpeg_enc);
    if (ret != JPEG_ERR_OK || out_len <= 0) {
        ESP_LOGE(TAG, "JPEG encode failed: %d", static_cast<int>(ret));
        return false;
    }

    *jpeg_len = out_len;
    return true;
}

void WhoColorAdjustTask::task()
{
    if (init_usb_cdc() != ESP_OK) {
        xEventGroupSetBits(m_event_group, TASK_STOPPED);
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "WhoColorAdjustTask started, host_serial=%s", m_config.host_serial_enabled ? "enabled" : "disabled");

    uint8_t *rx_buf = m_config.host_serial_enabled ? new uint8_t[RX_BUFFER_SIZE] : nullptr;

    while (true) {
        EventBits_t event_bits = xEventGroupWaitBits(m_event_group, TASK_PAUSE | TASK_STOP, pdTRUE, pdFALSE, 0);
        if (event_bits & TASK_STOP) {
            break;
        } else if (event_bits & TASK_PAUSE) {
            handle_pause();
            continue;
        }

        drain_tx_queue();
        if (!m_config.host_serial_enabled) {
            vTaskDelay(IDLE_DELAY);
            continue;
        }

        int len = usb_serial_jtag_read_bytes(rx_buf, RX_BUFFER_SIZE, 0);
        if (len <= 0) {
            vTaskDelay(IDLE_DELAY);
            continue;
        }

        handle_rx_data(rx_buf, static_cast<size_t>(len));
    }

    delete[] rx_buf;
    xEventGroupSetBits(m_event_group, TASK_STOPPED);
    vTaskDelete(NULL);
}

void WhoColorAdjustTask::cleanup()
{
    xQueueReset(m_tx_queue);
}

} // namespace communicate
} // namespace who
