#include "esp_http_server.h"
#include "who_web.hpp"

#include <algorithm>
#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <string>
#include <utility>

#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>

#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "driver/jpeg_encode.h"
#include "driver/ppa.h"
#include "who_frame_cap.hpp"

extern "C" esp_err_t who_web_network_start(void);

static const char *TAG = "who_web";
static const char *STREAM_BOUNDARY = "123456789000000000000987654321";
static const char *STREAM_CONTENT_TYPE = "multipart/x-mixed-replace;boundary=123456789000000000000987654321";

/* 引入内嵌的压缩前端静态资源，与 ESP32P4C5Mini_Factory 保持一致 */
extern const uint8_t index_html_gz_start[] asm("_binary_index_html_gz_start");
extern const uint8_t index_html_gz_end[] asm("_binary_index_html_gz_end");
extern const uint8_t loading_jpg_gz_start[] asm("_binary_loading_jpg_gz_start");
extern const uint8_t loading_jpg_gz_end[] asm("_binary_loading_jpg_gz_end");
extern const uint8_t favicon_ico_gz_start[] asm("_binary_favicon_ico_gz_start");
extern const uint8_t favicon_ico_gz_end[] asm("_binary_favicon_ico_gz_end");
extern const uint8_t assets_index_js_gz_start[] asm("_binary_index_js_gz_start");
extern const uint8_t assets_index_js_gz_end[] asm("_binary_index_js_gz_end");
extern const uint8_t assets_index_css_gz_start[] asm("_binary_index_css_gz_start");
extern const uint8_t assets_index_css_gz_end[] asm("_binary_index_css_gz_end");

namespace who {
namespace web {

static void upscale_rgb565_2x(const uint16_t *src, uint16_t *dst, int src_w, int src_h)
{
    int dst_w = src_w * 2;
    for (int y = 0; y < src_h; ++y) {
        const uint16_t *s_row = src + y * src_w;
        uint32_t *d_row0 = reinterpret_cast<uint32_t *>(dst + (y * 2) * dst_w);
        uint32_t *d_row1 = reinterpret_cast<uint32_t *>(dst + (y * 2 + 1) * dst_w);
        for (int x = 0; x < src_w; ++x) {
            uint32_t p = s_row[x];
            d_row0[x] = p | (p << 16);
        }
        memcpy(d_row1, d_row0, dst_w * sizeof(uint16_t));
    }
}

WhoWeb &WhoWeb::get_instance()
{
    static WhoWeb instance;
    return instance;
}

WhoWeb::WhoWeb()
{
    m_jpeg_lock = xSemaphoreCreateMutex();
    m_objects_lock = xSemaphoreCreateMutex();
    m_frame_sem = xSemaphoreCreateBinary();
    configASSERT(m_jpeg_lock && m_objects_lock && m_frame_sem);
}

WhoWeb::~WhoWeb()
{
    m_encoder_running.store(false);
    m_started.store(false);
    stop_http_servers();
    release_resources();
    if (m_jpeg_lock) {
        vSemaphoreDelete(m_jpeg_lock);
    }
    if (m_objects_lock) {
        vSemaphoreDelete(m_objects_lock);
    }
    if (m_frame_sem) {
        vSemaphoreDelete(m_frame_sem);
    }
}

void WhoWeb::release_resources()
{
    if (m_ppa_handle) {
        ppa_unregister_client(static_cast<ppa_client_handle_t>(m_ppa_handle));
        m_ppa_handle = nullptr;
    }
    if (m_jpeg_encoder) {
        jpeg_del_encoder_engine(static_cast<jpeg_encoder_handle_t>(m_jpeg_encoder));
        m_jpeg_encoder = nullptr;
    }
    if (m_rgb_buffer_320) {
        free(m_rgb_buffer_320);
        m_rgb_buffer_320 = nullptr;
    }
    if (m_rgb_buffer_640) {
        free(m_rgb_buffer_640);
        m_rgb_buffer_640 = nullptr;
    }
    for (auto &buffer : m_jpeg_buffers) {
        if (buffer) {
            free(buffer);
            buffer = nullptr;
        }
    }
    for (auto &len : m_jpeg_lengths) {
        len = 0;
    }
    m_ready_index = -1;
    m_sending_index = -1;
    m_ready_seq = 0;
    m_encode_index = 0;
}

esp_err_t WhoWeb::start(const char *title, const char *type, frame_cap::WhoFrameCapNode *frame_node)
{
    if (m_started.load()) {
        return ESP_OK;
    }
    if (!title || !type || !frame_node) {
        return ESP_ERR_INVALID_ARG;
    }

    m_title = title;
    m_type = type;
    m_frame_node = frame_node;
    m_width = frame_node->get_fb_width();
    m_height = frame_node->get_fb_height();
    if (m_width == 0 || m_height == 0) {
        m_width = 320;
        m_height = 240;
    }

    m_rgb_buffer_320_size = 320 * 240 * 2;
    m_rgb_buffer_640_size = 640 * 480 * 2;
    m_jpeg_buffer_size = 256 * 1024;

    jpeg_encode_memory_alloc_cfg_t tx_mem_cfg = {
        .buffer_direction = JPEG_ENC_ALLOC_INPUT_BUFFER,
    };
    jpeg_encode_memory_alloc_cfg_t rx_mem_cfg = {
        .buffer_direction = JPEG_ENC_ALLOC_OUTPUT_BUFFER,
    };

    size_t alloc_sz = 0;
    m_rgb_buffer_320 = static_cast<uint8_t *>(jpeg_alloc_encoder_mem(m_rgb_buffer_320_size, &tx_mem_cfg, &alloc_sz));
    m_rgb_buffer_640 = static_cast<uint8_t *>(jpeg_alloc_encoder_mem(m_rgb_buffer_640_size, &tx_mem_cfg, &alloc_sz));
    for (size_t i = 0; i < JPEG_BUFFER_COUNT; ++i) {
        m_jpeg_buffers[i] = static_cast<uint8_t *>(jpeg_alloc_encoder_mem(m_jpeg_buffer_size, &rx_mem_cfg, &alloc_sz));
        m_jpeg_lengths[i] = 0;
    }
    if (!m_rgb_buffer_320 || !m_rgb_buffer_640 || !m_jpeg_buffers[0] || !m_jpeg_buffers[1] || !m_jpeg_buffers[2]) {
        ESP_LOGE(TAG, "Failed to allocate web streaming buffers from PSRAM");
        release_resources();
        return ESP_ERR_NO_MEM;
    }

    jpeg_encode_engine_cfg_t enc_eng_cfg = {
        .intr_priority = 0,
        .timeout_ms = 40,
    };
    jpeg_encoder_handle_t hw_enc = nullptr;
    esp_err_t err = jpeg_new_encoder_engine(&enc_eng_cfg, &hw_enc);
    if (err != ESP_OK || !hw_enc) {
        ESP_LOGE(TAG, "Hardware JPEG encoder creation failed: %s", esp_err_to_name(err));
        release_resources();
        return err != ESP_OK ? err : ESP_FAIL;
    }
    m_jpeg_encoder = hw_enc;
    ESP_LOGI(TAG, "ESP32-P4 Hardware JPEG Encoder Engine initialized successfully");

    ppa_client_config_t ppa_client_config = {};
    ppa_client_config.oper_type = PPA_OPERATION_SRM;
    ppa_client_handle_t ppa_handle = nullptr;
    err = ppa_register_client(&ppa_client_config, &ppa_handle);
    if (err == ESP_OK) {
        m_ppa_handle = ppa_handle;
        ESP_LOGI(TAG, "Hardware PPA SRM Client registered for native 640x480 resolution");
    } else {
        ESP_LOGW(TAG, "Hardware PPA register failed: %s, fallback to upscale", esp_err_to_name(err));
    }

    err = who_web_network_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Wi-Fi SoftAP start failed: %s", esp_err_to_name(err));
        release_resources();
        return err;
    }

    err = start_http_servers();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HTTP server startup failed: %s", esp_err_to_name(err));
        stop_http_servers();
        release_resources();
        return err;
    }

    m_encoder_running.store(true);
    BaseType_t task_ret = pdFAIL;
#if CONFIG_SPIRAM_ALLOW_STACK_EXTERNAL_MEMORY
    task_ret = xTaskCreatePinnedToCoreWithCaps(encoder_task_entry, "web_jpeg", 8192, this, 4, &m_encoder_task, 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (task_ret != pdPASS) {
        task_ret = xTaskCreatePinnedToCore(encoder_task_entry, "web_jpeg", 8192, this, 4, &m_encoder_task, 1);
    }
#else
    task_ret = xTaskCreatePinnedToCore(encoder_task_entry, "web_jpeg", 8192, this, 4, &m_encoder_task, 1);
#endif
    if (task_ret != pdPASS) {
        m_encoder_running.store(false);
        stop_http_servers();
        release_resources();
        ESP_LOGE(TAG, "Failed to create web encoder task");
        return ESP_ERR_NO_MEM;
    }

    m_started.store(true);
    ESP_LOGI(TAG, "Web backhaul ready: %s (%s), 320x240 / 640x480, http=%d stream=%d",
             m_title.c_str(), m_type.c_str(),
             CONFIG_WHO_WEB_HTTP_PORT, CONFIG_WHO_WEB_STREAM_PORT);
    return ESP_OK;
}

esp_err_t WhoWeb::reinit_encoder(int format_id, int quality)
{
    if (format_id != 0 && format_id != 1) return ESP_ERR_INVALID_ARG;
    if (quality < 1 || quality > 100) return ESP_ERR_INVALID_ARG;

    xSemaphoreTake(m_jpeg_lock, portMAX_DELAY);
    m_current_format.store(format_id);
    m_jpeg_quality.store(static_cast<uint8_t>(quality));
    m_ready_index = -1;
    m_sending_index = -1;
    for (size_t i = 0; i < JPEG_BUFFER_COUNT; ++i) {
        m_jpeg_lengths[i] = 0;
    }
    m_ready_seq++;
    xSemaphoreGive(m_jpeg_lock);
    ESP_LOGI(TAG, "Configured HW JPEG encoder: format=%d (%s), quality=%d",
             format_id, (format_id == 1) ? "640x480" : "320x240", quality);
    return ESP_OK;
}

esp_err_t WhoWeb::set_format(int format_id)
{
    if (format_id != 0 && format_id != 1) {
        return ESP_ERR_INVALID_ARG;
    }
    int target_qual = (format_id == 1) ? 40 : 60;
    return reinit_encoder(format_id, target_qual);
}

esp_err_t WhoWeb::set_quality(int quality)
{
    return reinit_encoder(m_current_format.load(), quality);
}

void WhoWeb::set_objects_json(const std::string &objects_json, uint64_t timestamp_ms)
{
    if (objects_json.empty() || objects_json.front() != '[') {
        return;
    }
    if (timestamp_ms == 0) {
        timestamp_ms = static_cast<uint64_t>(esp_timer_get_time() / 1000);
    }
    xSemaphoreTake(m_objects_lock, portMAX_DELAY);
    m_objects_json = objects_json;
    m_result_timestamp_ms.store(timestamp_ms);
    xSemaphoreGive(m_objects_lock);
}

void WhoWeb::set_objects_json(std::string &&objects_json, uint64_t timestamp_ms)
{
    if (objects_json.empty() || objects_json.front() != '[') {
        return;
    }
    if (timestamp_ms == 0) {
        timestamp_ms = static_cast<uint64_t>(esp_timer_get_time() / 1000);
    }
    xSemaphoreTake(m_objects_lock, portMAX_DELAY);
    m_objects_json = std::move(objects_json);
    m_result_timestamp_ms.store(timestamp_ms);
    xSemaphoreGive(m_objects_lock);
}

void WhoWeb::clear_objects()
{
    set_objects_json("[]");
}

bool WhoWeb::fetch_frame_rgb(int format)
{
    if (format == 1 && m_ppa_handle) {
        frame_cap::WhoFrameCapNode *fetch_node = m_frame_node ? m_frame_node->get_prev_node() : nullptr;
        if (fetch_node) {
            who::cam::cam_fb_t raw_frame{};
            if (fetch_node->cam_fb_snapshot(raw_frame, -1, nullptr, 0) && raw_frame.buf) {
                ppa_srm_oper_config_t srm_cfg = {};
                srm_cfg.in.buffer = raw_frame.buf;
                srm_cfg.in.pic_w = raw_frame.width;
                srm_cfg.in.pic_h = raw_frame.height;
                srm_cfg.in.srm_cm = PPA_SRM_COLOR_MODE_RGB565;
                srm_cfg.rgb_swap = false;
                srm_cfg.byte_swap = false;

                if (raw_frame.width == 800 && raw_frame.height == 600) {
                    srm_cfg.in.block_offset_x = 80;
                    srm_cfg.in.block_offset_y = 60;
                    srm_cfg.in.block_w = 640;
                    srm_cfg.in.block_h = 480;
                } else {
                    srm_cfg.in.block_offset_x = 0;
                    srm_cfg.in.block_offset_y = 0;
                    srm_cfg.in.block_w = raw_frame.width;
                    srm_cfg.in.block_h = raw_frame.height;
                }

                srm_cfg.out.buffer = m_rgb_buffer_640;
                srm_cfg.out.buffer_size = static_cast<uint32_t>(m_rgb_buffer_640_size);
                srm_cfg.out.pic_w = 640;
                srm_cfg.out.pic_h = 480;
                srm_cfg.out.block_offset_x = 0;
                srm_cfg.out.block_offset_y = 0;
                srm_cfg.out.srm_cm = PPA_SRM_COLOR_MODE_RGB565;
                srm_cfg.rotation_angle = PPA_SRM_ROTATION_ANGLE_180;
                srm_cfg.scale_x = (srm_cfg.in.block_w == 640) ? 1.0f : ((float)640 / srm_cfg.in.block_w);
                srm_cfg.scale_y = (srm_cfg.in.block_h == 480) ? 1.0f : ((float)480 / srm_cfg.in.block_h);
                srm_cfg.mirror_x = false;
                srm_cfg.mirror_y = false;

                if (ppa_do_scale_rotate_mirror(static_cast<ppa_client_handle_t>(m_ppa_handle), &srm_cfg) == ESP_OK) {
                    return true;
                }
            }
        }
    }

    if (!m_frame_node || !m_rgb_buffer_320) {
        return false;
    }

    who::cam::cam_fb_t frame{};
    if (!m_frame_node->cam_fb_snapshot(frame, -1, m_rgb_buffer_320, m_rgb_buffer_320_size)) {
        return false;
    }

    if (format == 1 && m_rgb_buffer_640) {
        upscale_rgb565_2x(reinterpret_cast<const uint16_t *>(m_rgb_buffer_320),
                          reinterpret_cast<uint16_t *>(m_rgb_buffer_640),
                          320, 240);
    }
    return true;
}

void WhoWeb::encoder_task_entry(void *arg)
{
    static_cast<WhoWeb *>(arg)->encoder_task();
}

void WhoWeb::encoder_task()
{
    uint32_t frame_window_count = 0;
    int64_t window_start_us = esp_timer_get_time();

    while (m_encoder_running.load()) {
        TickType_t start_tick = xTaskGetTickCount();

        if (m_stream_clients.load() == 0) {
            vTaskDelay(pdMS_TO_TICKS(50));
            m_stream_fps.store(0.0f);
            frame_window_count = 0;
            window_start_us = esp_timer_get_time();
            continue;
        }

        int format = m_current_format.load();
        if (!fetch_frame_rgb(format)) {
            vTaskDelay(pdMS_TO_TICKS(5));
            continue;
        }

        uint32_t out_len = 0;
        esp_err_t err = ESP_FAIL;
        int cur_enc_idx = -1;

        xSemaphoreTake(m_jpeg_lock, portMAX_DELAY);
        format = m_current_format.load();
        const uint8_t *enc_src = (format == 1) ? m_rgb_buffer_640 : m_rgb_buffer_320;
        size_t enc_src_size = (format == 1) ? m_rgb_buffer_640_size : m_rgb_buffer_320_size;
        cur_enc_idx = m_encode_index;
        auto encoder = static_cast<jpeg_encoder_handle_t>(m_jpeg_encoder);

        if (encoder) {
            uint8_t *out_dest = m_jpeg_buffers[cur_enc_idx] + STREAM_HEADER_RESERVE;
            uint32_t out_dest_cap = static_cast<uint32_t>(m_jpeg_buffer_size - STREAM_HEADER_RESERVE);

            jpeg_encode_cfg_t enc_cfg = {
                .height = (format == 1) ? 480U : 240U,
                .width = (format == 1) ? 640U : 320U,
                .src_type = JPEG_ENCODE_IN_FORMAT_RGB565,
                .sub_sample = JPEG_DOWN_SAMPLING_YUV422,
                .image_quality = static_cast<uint32_t>(m_jpeg_quality.load()),
                .pixel_reverse = false,
            };

            err = jpeg_encoder_process(encoder, &enc_cfg, enc_src, static_cast<uint32_t>(enc_src_size),
                                       out_dest, out_dest_cap, &out_len);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "HW jpeg_encoder_process err=%d (%s): format=%d", err, esp_err_to_name(err), format);
            }
        }

        if (err == ESP_OK && out_len > 0) {
            m_jpeg_lengths[cur_enc_idx] = static_cast<size_t>(out_len);
            m_ready_index = cur_enc_idx;
            m_ready_seq++;

            for (size_t i = 0; i < JPEG_BUFFER_COUNT; ++i) {
                if (static_cast<int>(i) != m_ready_index && static_cast<int>(i) != m_sending_index) {
                    m_encode_index = static_cast<int>(i);
                    break;
                }
            }
            m_frame_count.fetch_add(1);
        }
        xSemaphoreGive(m_jpeg_lock);

        if (err == ESP_OK && out_len > 0) {
            xSemaphoreGive(m_frame_sem);

            frame_window_count++;
            int64_t now_us = esp_timer_get_time();
            if (now_us - window_start_us >= 1000000) {
                float fps = static_cast<float>(frame_window_count) * 1000000.0f / static_cast<float>(now_us - window_start_us);
                m_stream_fps.store(fps);
                frame_window_count = 0;
                window_start_us = now_us;
            }
        } else {
            ESP_LOGW(TAG, "JPEG encode failed: %s", esp_err_to_name(err));
        }

        int target_fps = 20;
        TickType_t target_period = pdMS_TO_TICKS(1000 / target_fps);
        TickType_t elapsed = xTaskGetTickCount() - start_tick;
        if (elapsed < target_period) {
            vTaskDelay(target_period - elapsed);
        }
    }
}

/* 静态资源路由（直接服务内嵌的官方 WebUI gzip 文件） */
esp_err_t WhoWeb::static_file_handler(httpd_req_t *req)
{
    const char *uri = req->uri;

    if (strcmp(uri, "/") == 0 || strcmp(uri, "/index.html") == 0) {
        httpd_resp_set_type(req, "text/html");
        httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
        return httpd_resp_send(req, reinterpret_cast<const char *>(index_html_gz_start),
                               index_html_gz_end - index_html_gz_start);
    } else if (strcmp(uri, "/loading.jpg") == 0) {
        httpd_resp_set_type(req, "image/jpeg");
        httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
        return httpd_resp_send(req, reinterpret_cast<const char *>(loading_jpg_gz_start),
                               loading_jpg_gz_end - loading_jpg_gz_start);
    } else if (strcmp(uri, "/favicon.ico") == 0) {
        httpd_resp_set_type(req, "image/x-icon");
        httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
        return httpd_resp_send(req, reinterpret_cast<const char *>(favicon_ico_gz_start),
                               favicon_ico_gz_end - favicon_ico_gz_start);
    } else if (strcmp(uri, "/assets/index.js") == 0) {
        httpd_resp_set_type(req, "application/javascript");
        httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
        return httpd_resp_send(req, reinterpret_cast<const char *>(assets_index_js_gz_start),
                               assets_index_js_gz_end - assets_index_js_gz_start);
    } else if (strcmp(uri, "/assets/index.css") == 0) {
        httpd_resp_set_type(req, "text/css");
        httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
        return httpd_resp_send(req, reinterpret_cast<const char *>(assets_index_css_gz_start),
                               assets_index_css_gz_end - assets_index_css_gz_start);
    }

    ESP_LOGW(TAG, "File not found: %s", uri);
    httpd_resp_send_404(req);
    return ESP_FAIL;
}

/* 官方前端摄像机信息接口（支持 320x240 与 640x480 分辨率切换） */
esp_err_t WhoWeb::camera_info_handler(httpd_req_t *req)
{
    auto *self = static_cast<WhoWeb *>(req->user_ctx);
    int cur_fmt = self->m_current_format.load();
    uint32_t cur_w = (cur_fmt == 1) ? 640 : 320;
    uint32_t cur_h = (cur_fmt == 1) ? 480 : 240;
    uint32_t stream_fps = 20;

    char buf[768];
    snprintf(buf, sizeof(buf),
             "{\"cameras\":[{\"index\":0,\"src\":\":%d/stream\",\"currentFrameRate\":%" PRIu32 ","
             "\"currentImageFormat\":%d,\"currentImageFormatDescription\":\"JPEG %" PRIu32 "x%" PRIu32 "\","
             "\"currentQuality\":%u,\"currentResolution\":{\"width\":%" PRIu32 ",\"height\":%" PRIu32 "},"
             "\"imageFormats\":["
             "{\"id\":0,\"description\":\"JPEG 320x240\",\"quality\":{\"min\":1,\"max\":100,\"step\":1,\"default\":60}},"
             "{\"id\":1,\"description\":\"JPEG 640x480\",\"quality\":{\"min\":1,\"max\":100,\"step\":1,\"default\":40}}"
             "]}]}",
             CONFIG_WHO_WEB_STREAM_PORT,
             stream_fps,
             cur_fmt, cur_w, cur_h,
             self->m_jpeg_quality.load(),
             cur_w, cur_h);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    return httpd_resp_sendstr(req, buf);
}

/* 拍照下载 JPEG 接口 */
esp_err_t WhoWeb::capture_image_handler(httpd_req_t *req)
{
    auto *self = static_cast<WhoWeb *>(req->user_ctx);
    int ready_idx = -1;
    size_t ready_len = 0;

    xSemaphoreTake(self->m_jpeg_lock, portMAX_DELAY);
    ready_idx = self->m_ready_index;
    if (ready_idx >= 0) {
        ready_len = self->m_jpeg_lengths[ready_idx];
    }
    xSemaphoreGive(self->m_jpeg_lock);

    if (ready_idx < 0 || ready_len == 0) {
        int format = self->m_current_format.load();
        if (self->fetch_frame_rgb(format)) {
            uint32_t out_len = 0;
            xSemaphoreTake(self->m_jpeg_lock, portMAX_DELAY);
            format = self->m_current_format.load();
            const uint8_t *enc_src = (format == 1) ? self->m_rgb_buffer_640 : self->m_rgb_buffer_320;
            size_t enc_src_size = (format == 1) ? self->m_rgb_buffer_640_size : self->m_rgb_buffer_320_size;
            auto encoder = static_cast<jpeg_encoder_handle_t>(self->m_jpeg_encoder);
            if (encoder) {
                uint8_t *dest = self->m_jpeg_buffers[0] + STREAM_HEADER_RESERVE;
                uint32_t cap = static_cast<uint32_t>(self->m_jpeg_buffer_size - STREAM_HEADER_RESERVE);
                jpeg_encode_cfg_t enc_cfg = {
                    .height = (format == 1) ? 480U : 240U,
                    .width = (format == 1) ? 640U : 320U,
                    .src_type = JPEG_ENCODE_IN_FORMAT_RGB565,
                    .sub_sample = JPEG_DOWN_SAMPLING_YUV422,
                    .image_quality = static_cast<uint32_t>(self->m_jpeg_quality.load()),
                    .pixel_reverse = false,
                };
                esp_err_t enc_err = jpeg_encoder_process(encoder, &enc_cfg, enc_src, static_cast<uint32_t>(enc_src_size),
                                                         dest, cap, &out_len);
                if (enc_err == ESP_OK && out_len > 0) {
                    self->m_jpeg_lengths[0] = static_cast<size_t>(out_len);
                    self->m_ready_index = 0;
                    ready_idx = 0;
                    ready_len = static_cast<size_t>(out_len);
                }
            }
            xSemaphoreGive(self->m_jpeg_lock);
        }
    }

    if (ready_idx < 0 || ready_len == 0) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "No frame ready");
    }

    httpd_resp_set_type(req, "image/jpeg;name=image0.jpg");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    const uint8_t *jpeg_ptr = self->m_jpeg_buffers[ready_idx] + STREAM_HEADER_RESERVE;
    return httpd_resp_send(req, reinterpret_cast<const char *>(jpeg_ptr), ready_len);
}

/* 原始二进制流下载接口 */
esp_err_t WhoWeb::capture_binary_handler(httpd_req_t *req)
{
    auto *self = static_cast<WhoWeb *>(req->user_ctx);
    int format = self->m_current_format.load();
    if (!self->fetch_frame_rgb(format)) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "No frame buffer");
    }

    const uint8_t *bin_src = (format == 1) ? self->m_rgb_buffer_640 : self->m_rgb_buffer_320;
    size_t bin_src_size = (format == 1) ? self->m_rgb_buffer_640_size : self->m_rgb_buffer_320_size;

    httpd_resp_set_type(req, "application/octet-stream;name=image_binary0.bin");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, reinterpret_cast<const char *>(bin_src), bin_src_size);
}

/* 摄像机配置接口（前端设置质量与分辨率） */
esp_err_t WhoWeb::camera_settings_handler(httpd_req_t *req)
{
    auto *self = static_cast<WhoWeb *>(req->user_ctx);
    char buf[256] = {};
    int ret = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (ret > 0) {
        cJSON *root = cJSON_Parse(buf);
        if (root) {
            int new_fmt = self->m_current_format.load();
            int new_qual = self->m_jpeg_quality.load();
            bool fmt_changed = false;
            bool qual_changed = false;

            cJSON *fmt = cJSON_GetObjectItem(root, "image_format");
            if (!fmt) {
                cJSON *cam = cJSON_GetObjectItem(root, "camera");
                if (cam) {
                    fmt = cJSON_GetObjectItem(cam, "image_format");
                    if (!fmt) fmt = cJSON_GetObjectItem(cam, "imageFormat");
                }
            }
            if (!fmt) fmt = cJSON_GetObjectItem(root, "imageFormat");

            if (fmt && cJSON_IsNumber(fmt)) {
                int f = fmt->valueint;
                if ((f == 0 || f == 1) && f != new_fmt) {
                    new_fmt = f;
                    fmt_changed = true;
                    if (new_fmt == 1) {
                        new_qual = 50;
                        qual_changed = true;
                    } else {
                        new_qual = 60;
                        qual_changed = true;
                    }
                }
            }

            cJSON *q = cJSON_GetObjectItem(root, "quality");
            if (!q) q = cJSON_GetObjectItem(root, "jpeg_quality");
            if (q && cJSON_IsNumber(q)) {
                int qual = q->valueint;
                if (qual >= 1 && qual <= 100 && qual != new_qual) {
                    new_qual = qual;
                    qual_changed = true;
                }
            }

            if (fmt_changed || qual_changed) {
                self->reinit_encoder(new_fmt, new_qual);
                ESP_LOGI(TAG, "Configured camera: format=%d (%s), quality=%d",
                         new_fmt, (new_fmt == 1) ? "640x480" : "320x240", new_qual);
            }

            cJSON_Delete(root);
        }
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    return httpd_resp_sendstr(req, "{\"status\":\"ok\"}");
}

/* 实时检测结果 JSON 接口（支持 Factory 与 AI 目标双格式） */
esp_err_t WhoWeb::detections_handler(httpd_req_t *req)
{
    auto *self = static_cast<WhoWeb *>(req->user_ctx);
    if (!self) {
        return ESP_FAIL;
    }

    std::string objects_copy;
    uint64_t result_ts = 0;
    xSemaphoreTake(self->m_objects_lock, portMAX_DELAY);
    objects_copy = self->m_objects_json;
    result_ts = self->m_result_timestamp_ms.load();
    xSemaphoreGive(self->m_objects_lock);

    uint64_t now_ms = static_cast<uint64_t>(esp_timer_get_time() / 1000);
    int64_t age_ms = result_ts ? static_cast<int64_t>(now_ms - result_ts) : -1;

    uint32_t cur_w = (self->m_current_format.load() == 1) ? 640 : 320;
    uint32_t cur_h = (self->m_current_format.load() == 1) ? 480 : 240;

    std::string json;
    json.reserve(objects_copy.size() + 384);
    json.append("{\"title\":\"");
    json.append(self->m_title);
    json.append("\",\"type\":\"");
    json.append(self->m_type);
    json.append("\",\"sequence\":");
    json.append(std::to_string(self->m_frame_count.load()));
    json.append(",\"width\":");
    json.append(std::to_string(cur_w));
    json.append(",\"height\":");
    json.append(std::to_string(cur_h));
    json.append(",\"streamFps\":");
    json.append(std::to_string(self->m_stream_fps.load()));
    json.append(",\"video_fps\":");
    json.append(std::to_string(self->m_stream_fps.load()));
    json.append(",\"age_ms\":");
    json.append(std::to_string(age_ms));
    json.append(",\"enabled\":true,\"processing\":false");

    /* 若为 AprilTag，同时兼容输出 tags 字段 */
    if (self->m_type == "apriltag") {
        json.append(",\"tags\":");
        json.append(objects_copy);
    }

    json.append(",\"objects\":");
    json.append(objects_copy);
    json.append("}");

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, json.data(), json.size());
}

/* 81 端口 MJPEG 实时视频流 */
esp_err_t WhoWeb::stream_handler(httpd_req_t *req)
{
    auto *self = static_cast<WhoWeb *>(req->user_ctx);
    if (!self) {
        return ESP_FAIL;
    }

    struct ClientGuard {
        explicit ClientGuard(std::atomic<int> &count) : clients(count) { ++clients; }
        ~ClientGuard() { --clients; }
        std::atomic<int> &clients;
    } client_guard(self->m_stream_clients);

    int sockfd = httpd_req_to_sockfd(req);
    if (sockfd >= 0) {
        int enable = 1;
        setsockopt(sockfd, IPPROTO_TCP, TCP_NODELAY, &enable, sizeof(enable));
        struct timeval tv = {.tv_sec = 2, .tv_usec = 0};
        setsockopt(sockfd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    }

    httpd_resp_set_type(req, STREAM_CONTENT_TYPE);
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store, no-cache, must-revalidate");
    httpd_resp_set_hdr(req, "Pragma", "no-cache");
    httpd_resp_set_hdr(req, "Connection", "close");

    uint32_t last_sent_seq = 0;

    while (true) {
        xSemaphoreTake(self->m_frame_sem, pdMS_TO_TICKS(100));

        int buf_idx = -1;
        size_t out_len = 0;

        xSemaphoreTake(self->m_jpeg_lock, portMAX_DELAY);
        if (self->m_ready_index >= 0 && self->m_ready_seq != last_sent_seq) {
            buf_idx = self->m_ready_index;
            out_len = self->m_jpeg_lengths[buf_idx];
            self->m_sending_index = buf_idx;
            last_sent_seq = self->m_ready_seq;
        }
        xSemaphoreGive(self->m_jpeg_lock);

        if (buf_idx < 0 || out_len == 0) {
            continue;
        }

        char header_buf[STREAM_HEADER_RESERVE];
        int hlen = snprintf(header_buf, sizeof(header_buf),
                            "\r\n--%s\r\nContent-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n",
                            STREAM_BOUNDARY,
                            static_cast<unsigned>(out_len));

        uint8_t *chunk_start = self->m_jpeg_buffers[buf_idx] + (STREAM_HEADER_RESERVE - hlen);
        memcpy(chunk_start, header_buf, hlen);
        size_t total_len = static_cast<size_t>(hlen) + out_len;

        esp_err_t err = httpd_resp_send_chunk(req, reinterpret_cast<const char *>(chunk_start), total_len);

        xSemaphoreTake(self->m_jpeg_lock, portMAX_DELAY);
        if (self->m_sending_index == buf_idx) {
            self->m_sending_index = -1;
        }
        xSemaphoreGive(self->m_jpeg_lock);

        if (err != ESP_OK) {
            break;
        }
    }

    return ESP_OK;
}

esp_err_t WhoWeb::start_http_servers()
{
    if (CONFIG_WHO_WEB_HTTP_PORT == CONFIG_WHO_WEB_STREAM_PORT) {
        ESP_LOGE(TAG, "HTTP and stream ports must differ");
        return ESP_ERR_INVALID_ARG;
    }

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = CONFIG_WHO_WEB_HTTP_PORT;
    config.ctrl_port = 32768;
    config.max_uri_handlers = 12;
    config.max_open_sockets = 4;
    config.backlog_conn = 4;
    config.lru_purge_enable = true;
    config.stack_size = 6144;
#if CONFIG_SPIRAM_ALLOW_STACK_EXTERNAL_MEMORY
    config.task_caps = (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#endif
    config.keep_alive_enable = false;
    config.uri_match_fn = httpd_uri_match_wildcard;

    httpd_handle_t httpd = nullptr;
    esp_err_t err = httpd_start(&httpd, &config);
#if CONFIG_SPIRAM_ALLOW_STACK_EXTERNAL_MEMORY
    if (err != ESP_OK) {
        config.task_caps = (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        err = httpd_start(&httpd, &config);
    }
#endif
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start HTTP port %d: %s", config.server_port, esp_err_to_name(err));
        return err;
    }

    /* 注册 API 端点（与 ESP32P4C5Mini_Factory 规范完全统一） */
    httpd_uri_t get_info_uri = {};
    get_info_uri.uri = "/api/get_camera_info";
    get_info_uri.method = HTTP_GET;
    get_info_uri.handler = camera_info_handler;
    get_info_uri.user_ctx = this;

    httpd_uri_t capture_img_uri = {};
    capture_img_uri.uri = "/api/capture_image";
    capture_img_uri.method = HTTP_GET;
    capture_img_uri.handler = capture_image_handler;
    capture_img_uri.user_ctx = this;

    httpd_uri_t frame_jpg_uri = {};
    frame_jpg_uri.uri = "/api/frame.jpg";
    frame_jpg_uri.method = HTTP_GET;
    frame_jpg_uri.handler = capture_image_handler;
    frame_jpg_uri.user_ctx = this;

    httpd_uri_t capture_bin_uri = {};
    capture_bin_uri.uri = "/api/capture_binary";
    capture_bin_uri.method = HTTP_GET;
    capture_bin_uri.handler = capture_binary_handler;
    capture_bin_uri.user_ctx = this;

    httpd_uri_t set_cfg_uri = {};
    set_cfg_uri.uri = "/api/set_camera_config";
    set_cfg_uri.method = HTTP_POST;
    set_cfg_uri.handler = camera_settings_handler;
    set_cfg_uri.user_ctx = this;

    httpd_uri_t detections_uri = {};
    detections_uri.uri = "/api/detections";
    detections_uri.method = HTTP_GET;
    detections_uri.handler = detections_handler;
    detections_uri.user_ctx = this;

    /* 通配静态文件路由（在最后匹配） */
    httpd_uri_t static_uri = {};
    static_uri.uri = "/*";
    static_uri.method = HTTP_GET;
    static_uri.handler = static_file_handler;
    static_uri.user_ctx = this;

    err = httpd_register_uri_handler(httpd, &get_info_uri);
    if (err == ESP_OK) err = httpd_register_uri_handler(httpd, &capture_img_uri);
    if (err == ESP_OK) err = httpd_register_uri_handler(httpd, &frame_jpg_uri);
    if (err == ESP_OK) err = httpd_register_uri_handler(httpd, &capture_bin_uri);
    if (err == ESP_OK) err = httpd_register_uri_handler(httpd, &set_cfg_uri);
    if (err == ESP_OK) err = httpd_register_uri_handler(httpd, &detections_uri);
    if (err == ESP_OK) err = httpd_register_uri_handler(httpd, &static_uri);

    if (err != ESP_OK) {
        httpd_stop(httpd);
        return err;
    }
    m_http_server = httpd;

    /* 视频专用服务 (端口 81) */
    httpd_config_t stream_config = HTTPD_DEFAULT_CONFIG();
    stream_config.server_port = CONFIG_WHO_WEB_STREAM_PORT;
    stream_config.ctrl_port = 32769;
    stream_config.max_uri_handlers = 2;
    stream_config.max_open_sockets = 2;
    stream_config.backlog_conn = 2;
    stream_config.lru_purge_enable = true;
    stream_config.stack_size = 6144;
#if CONFIG_SPIRAM_ALLOW_STACK_EXTERNAL_MEMORY
    stream_config.task_caps = (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#endif
    stream_config.keep_alive_enable = false;
    stream_config.uri_match_fn = httpd_uri_match_wildcard;

    httpd_handle_t stream_httpd = nullptr;
    err = httpd_start(&stream_httpd, &stream_config);
#if CONFIG_SPIRAM_ALLOW_STACK_EXTERNAL_MEMORY
    if (err != ESP_OK) {
        stream_config.task_caps = (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        err = httpd_start(&stream_httpd, &stream_config);
    }
#endif
    if (err != ESP_OK) {
        httpd_stop(httpd);
        m_http_server = nullptr;
        ESP_LOGE(TAG, "Failed to start stream server on port %d: %s", stream_config.server_port, esp_err_to_name(err));
        return err;
    }

    httpd_uri_t stream_uri = {};
    stream_uri.uri = "/stream";
    stream_uri.method = HTTP_GET;
    stream_uri.handler = stream_handler;
    stream_uri.user_ctx = this;

    err = httpd_register_uri_handler(stream_httpd, &stream_uri);
    if (err != ESP_OK) {
        httpd_stop(stream_httpd);
        httpd_stop(httpd);
        m_http_server = nullptr;
        return err;
    }
    m_stream_server = stream_httpd;

    return ESP_OK;
}

void WhoWeb::stop_http_servers()
{
    if (m_stream_server) {
        httpd_stop(static_cast<httpd_handle_t>(m_stream_server));
        m_stream_server = nullptr;
    }
    if (m_http_server) {
        httpd_stop(static_cast<httpd_handle_t>(m_http_server));
        m_http_server = nullptr;
    }
}

} // namespace web
} // namespace who
