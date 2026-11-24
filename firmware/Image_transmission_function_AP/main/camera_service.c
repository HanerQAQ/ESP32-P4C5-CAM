/*
 * SPDX-FileCopyrightText: 2024-2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: ESPRESSIF MIT
 */

#include <string.h>
#include <stdlib.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/param.h>
#include <sys/errno.h>
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "cJSON.h"
#include "esp_event.h"
#include "esp_err.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_http_server.h"
#include "example_video_common.h"
#include "camera_service.h"
#if CONFIG_EXAMPLE_SELECT_JPEG_HW_DRIVER
#include "driver/jpeg_encode.h"
typedef struct {
    jpeg_encode_cfg_t jpeg_enc_config;
    uint32_t jpeg_out_buf_size;
} hw_jpeg_encoder_ctx_t;
#endif
#include "hardware_test.h"

#define EXAMPLE_CAMERA_VIDEO_BUFFER_NUMBER  CONFIG_EXAMPLE_CAMERA_VIDEO_BUFFER_NUMBER

#define EXAMPLE_JPEG_ENC_QUALITY            CONFIG_EXAMPLE_JPEG_COMPRESSION_QUALITY

#define EXAMPLE_PART_BOUNDARY               CONFIG_EXAMPLE_HTTP_PART_BOUNDARY

#define CAMERA_DQBUF_TIMEOUT_SECONDS        2

static const char *STREAM_CONTENT_TYPE = "multipart/x-mixed-replace;boundary=" EXAMPLE_PART_BOUNDARY;
static const char *STREAM_BOUNDARY = "\r\n--" EXAMPLE_PART_BOUNDARY "\r\n";
static const char *STREAM_PART = "Content-Type: image/jpeg\r\nContent-Length: %u\r\nX-Timestamp: %d.%06d\r\n\r\n";

extern volatile uint8_t wifi_tx_throttling;

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

/**
 * @brief Web cam control structure
 */
typedef struct web_cam_video {
    int fd;
    uint8_t index;

    example_encoder_handle_t encoder_handle;
    uint8_t *jpeg_out_buf;
    uint32_t jpeg_out_size;

    uint8_t *buffer[EXAMPLE_CAMERA_VIDEO_BUFFER_NUMBER];
    uint32_t buffer_size;
    uint32_t capture_width;
    uint32_t capture_height;
    uint32_t capture_stride;

    uint32_t width;
    uint32_t height;
    uint32_t pixel_format;
    uint8_t jpeg_quality;

    uint32_t frame_rate;

    SemaphoreHandle_t sem;
    TaskHandle_t frame_watchdog_task;
    volatile int64_t frame_wait_start_us;

    uint32_t support_control_jpeg_quality   : 1;
} web_cam_video_t;

typedef struct web_cam {
    uint8_t video_count;
    web_cam_video_t video[0];
} web_cam_t;

typedef struct web_cam_video_config {
    const char *dev_name;
    uint32_t buffer_count;
} web_cam_video_config_t;

typedef struct request_desc {
    int index;
} request_desc_t;

static const char *TAG = "camera_service";
static web_cam_t *s_web_cam;
static portMUX_TYPE s_stats_lock = portMUX_INITIALIZER_UNLOCKED;
static float s_stream_fps;
static int64_t s_last_sent;

static void camera_frame_watchdog_task(void *arg)
{
    web_cam_video_t *video = arg;
    uint32_t last_reported_period = 0;

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));

        int64_t wait_start_us = video->frame_wait_start_us;
        if (wait_start_us == 0) {
            last_reported_period = 0;
            continue;
        }

        uint32_t waiting_seconds = (uint32_t)((esp_timer_get_time() - wait_start_us) / 1000000);
        uint32_t period = waiting_seconds / CAMERA_DQBUF_TIMEOUT_SECONDS;
        if (period > 0 && period != last_reported_period) {
            ESP_LOGE(TAG, "video%d: still waiting for a camera frame (%" PRIu32 " seconds)",
                     video->index, waiting_seconds);
            last_reported_period = period;
        }
    }
}

static bool is_valid_web_cam(web_cam_video_t *video)
{
    return video->fd != -1;
}

static esp_err_t decode_request(web_cam_t *web_cam, httpd_req_t *req, request_desc_t *desc)
{
    esp_err_t ret;
    int index = -1;
    char buffer[32];

    if ((ret = httpd_req_get_url_query_str(req, buffer, sizeof(buffer))) != ESP_OK) {
        return ret;
    }
    ESP_LOGD(TAG, "source: %s", buffer);

    for (int i = 0; i < web_cam->video_count; i++) {
        char source_str[16];

        if (snprintf(source_str, sizeof(source_str), "source=%d", i) <= 0) {
            return ESP_FAIL;
        }

        if (strcmp(buffer, source_str) == 0) {
            index = i;
            break;
        }
    }
    if (index == -1) {
        return ESP_ERR_INVALID_ARG;
    }

    desc->index = index;
    return ESP_OK;
}

/* Pack row padding only; retain the entire native image with no crop. */
static esp_err_t pack_frame(web_cam_video_t *video, struct v4l2_buffer *buf)
{
    if (buf->index >= EXAMPLE_CAMERA_VIDEO_BUFFER_NUMBER || !video->buffer[buf->index]) {
        return ESP_ERR_INVALID_SIZE;
    }
    uint8_t *frame = video->buffer[buf->index];
    if (video->width == 640 && video->height == 480) {
        /* Center crop 640x480 from native 800x600 camera capture */
        const uint32_t crop_w = 640;
        const uint32_t crop_h = 480;
        const uint32_t x_offset = (video->capture_width > crop_w) ? (video->capture_width - crop_w) / 2 : 0;
        const uint32_t y_offset = (video->capture_height > crop_h) ? (video->capture_height - crop_h) / 2 : 0;
        const size_t row_bytes = crop_w * 2;
        for (uint32_t y = 0; y < crop_h; ++y) {
            const uint8_t *src_row = frame + (y + y_offset) * video->capture_stride + (x_offset * 2);
            uint8_t *dst_row = frame + y * row_bytes;
            memmove(dst_row, src_row, row_bytes);
        }
        buf->bytesused = row_bytes * crop_h;
    } else {
        /* Native full frame 800x600 */
        const size_t row_bytes = video->width * 2;
        const size_t required = (video->height - 1) * video->capture_stride + row_bytes;
        if (required > buf->bytesused || required > video->buffer_size) return ESP_ERR_INVALID_SIZE;
        if (video->capture_stride != row_bytes) {
            for (uint32_t y = 0; y < video->height; ++y)
                memmove(frame + y * row_bytes, frame + y * video->capture_stride, row_bytes);
        }
        buf->bytesused = row_bytes * video->height;
    }
    return ESP_OK;
}

static esp_err_t set_camera_resolution(web_cam_video_t *video, uint32_t width, uint32_t height)
{
    if (!video) return ESP_ERR_INVALID_ARG;
    if (width != 640 && width != 800) return ESP_ERR_INVALID_ARG;
    if (height != 480 && height != 600) return ESP_ERR_INVALID_ARG;
    if (xSemaphoreTake(video->sem, pdMS_TO_TICKS(1000)) != pdPASS) {
        return ESP_ERR_TIMEOUT;
    }
    video->width = width;
    video->height = height;
#if CONFIG_EXAMPLE_SELECT_JPEG_HW_DRIVER
    if (video->encoder_handle) {
        hw_jpeg_encoder_ctx_t *enc = (hw_jpeg_encoder_ctx_t *)video->encoder_handle;
        enc->jpeg_enc_config.width = width;
        enc->jpeg_enc_config.height = height;
    }
#endif
    xSemaphoreGive(video->sem);
    ESP_LOGI(TAG, "video%d: resolution switched to %" PRIu32 "x%" PRIu32, video->index, width, height);
    return ESP_OK;
}

static esp_err_t capture_video_image(httpd_req_t *req, web_cam_video_t *video, bool is_jpeg)
{
    esp_err_t ret;
    struct v4l2_buffer buf;
    const char *type_str = is_jpeg ? "JPEG" : "binary";
    uint32_t jpeg_encoded_size;

    memset(&buf, 0, sizeof(buf));
    buf.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buf.memory = V4L2_MEMORY_MMAP;
    video->frame_wait_start_us = esp_timer_get_time();
    if (ioctl(video->fd, VIDIOC_DQBUF, &buf) != 0) {
        video->frame_wait_start_us = 0;
        ESP_LOGE(TAG, "video%d: no camera frame received within %d seconds (errno=%d)",
                 video->index, CAMERA_DQBUF_TIMEOUT_SECONDS, errno);
        return ESP_FAIL;
    }
    video->frame_wait_start_us = 0;
    if (!(buf.flags & V4L2_BUF_FLAG_DONE)) {
        ioctl(video->fd, VIDIOC_QBUF, &buf);
        return ESP_ERR_INVALID_RESPONSE;
    }
    ESP_GOTO_ON_ERROR(pack_frame(video, &buf), fail0, TAG, "failed to pack frame");

    if (!is_jpeg || video->pixel_format == V4L2_PIX_FMT_JPEG) {
        /* Directly send the buffer of raw data */
        ESP_GOTO_ON_ERROR(httpd_resp_send(req, (char *)video->buffer[buf.index], buf.bytesused), fail0, TAG, "failed to send %s", type_str);
        jpeg_encoded_size = buf.bytesused;
    } else {
        ESP_GOTO_ON_FALSE(xSemaphoreTake(video->sem, portMAX_DELAY) == pdPASS, ESP_FAIL, fail0, TAG, "failed to take semaphore");
        ret = example_encoder_process(video->encoder_handle, video->buffer[buf.index], buf.bytesused,
                                      video->jpeg_out_buf, video->jpeg_out_size, &jpeg_encoded_size);
        xSemaphoreGive(video->sem);
        ESP_GOTO_ON_ERROR(ret, fail0, TAG, "failed to encode video frame");
        ESP_GOTO_ON_ERROR(httpd_resp_send(req, (char *)video->jpeg_out_buf, jpeg_encoded_size), fail0, TAG, "failed to send %s", type_str);
    }

    ESP_RETURN_ON_ERROR(ioctl(video->fd, VIDIOC_QBUF, &buf), TAG, "failed to queue video frame");

    ESP_GOTO_ON_ERROR(httpd_resp_sendstr_chunk(req, NULL), fail0, TAG, "failed to send null");

    ESP_LOGD(TAG, "send %s image%d size: %" PRIu32, type_str, video->index, jpeg_encoded_size);

    return ESP_OK;

fail0:
    ioctl(video->fd, VIDIOC_QBUF, &buf);
    return ret;
}

static char *get_cameras_json(web_cam_t *web_cam)
{
    cJSON *root = cJSON_CreateObject();
    cJSON *cameras = cJSON_CreateArray();
    cJSON_AddItemToObject(root, "cameras", cameras);

    for (int i = 0; i < web_cam->video_count; i++) {
        char src_str[32];

        if (!is_valid_web_cam(&web_cam->video[i])) {
            continue;
        }

        cJSON *camera = cJSON_CreateObject();
        cJSON_AddNumberToObject(camera, "index", i);
        assert(snprintf(src_str, sizeof(src_str), ":%d/stream", i + 81) > 0);
        cJSON_AddStringToObject(camera, "src", src_str);
        cJSON_AddNumberToObject(camera, "currentFrameRate", web_cam->video[i].frame_rate);
        
        int cur_fmt_id = (web_cam->video[i].width == 640) ? 0 : 1;
        cJSON_AddNumberToObject(camera, "currentImageFormat", cur_fmt_id);
        assert(snprintf(src_str, sizeof(src_str), "JPEG %" PRIu32 "x%" PRIu32, web_cam->video[i].width, web_cam->video[i].height) > 0);
        cJSON_AddStringToObject(camera, "currentImageFormatDescription", src_str);

        if (web_cam->video[i].support_control_jpeg_quality) {
            cJSON_AddNumberToObject(camera, "currentQuality", web_cam->video[i].jpeg_quality);
        }

        cJSON *current_resolution = cJSON_CreateObject();
        cJSON_AddNumberToObject(current_resolution, "width", web_cam->video[i].width);
        cJSON_AddNumberToObject(current_resolution, "height", web_cam->video[i].height);
        cJSON_AddItemToObject(camera, "currentResolution", current_resolution);

        cJSON *image_formats = cJSON_CreateArray();

        /* Level 0: 640x480 */
        cJSON *fmt0 = cJSON_CreateObject();
        cJSON_AddNumberToObject(fmt0, "id", 0);
        cJSON_AddStringToObject(fmt0, "description", "JPEG 640x480");
        if (web_cam->video[i].support_control_jpeg_quality) {
            cJSON *q0 = cJSON_CreateObject();
            cJSON_AddNumberToObject(q0, "min", 1);
            cJSON_AddNumberToObject(q0, "max", 100);
            cJSON_AddNumberToObject(q0, "step", 1);
            cJSON_AddNumberToObject(q0, "default", 70);
            cJSON_AddItemToObject(fmt0, "quality", q0);
        }
        cJSON_AddItemToArray(image_formats, fmt0);

        /* Level 1: 800x600 */
        cJSON *fmt1 = cJSON_CreateObject();
        cJSON_AddNumberToObject(fmt1, "id", 1);
        cJSON_AddStringToObject(fmt1, "description", "JPEG 800x600");
        if (web_cam->video[i].support_control_jpeg_quality) {
            cJSON *q1 = cJSON_CreateObject();
            cJSON_AddNumberToObject(q1, "min", 1);
            cJSON_AddNumberToObject(q1, "max", 100);
            cJSON_AddNumberToObject(q1, "step", 1);
            cJSON_AddNumberToObject(q1, "default", 70);
            cJSON_AddItemToObject(fmt1, "quality", q1);
        }
        cJSON_AddItemToArray(image_formats, fmt1);

        cJSON_AddItemToObject(camera, "imageFormats", image_formats);
        cJSON_AddItemToArray(cameras, camera);
    }

    char *output = cJSON_Print(root);
    cJSON_Delete(root);
    return output;
}

static esp_err_t set_camera_jpeg_quality(web_cam_video_t *video, int quality)
{
    esp_err_t ret = ESP_OK;
    int quality_reset = quality;

    if (video->pixel_format == V4L2_PIX_FMT_JPEG) {
        struct v4l2_ext_controls controls = {0};
        struct v4l2_ext_control control[1];
        struct v4l2_query_ext_ctrl qctrl = {0};

        qctrl.id = V4L2_CID_JPEG_COMPRESSION_QUALITY;
        if (ioctl(video->fd, VIDIOC_QUERY_EXT_CTRL, &qctrl) == 0) {
            if ((quality > qctrl.maximum) || (quality < qctrl.minimum) ||
                    (((quality - qctrl.minimum) % qctrl.step) != 0)) {

                if (quality > qctrl.maximum) {
                    quality_reset = qctrl.maximum;
                } else if (quality < qctrl.minimum) {
                    quality_reset = qctrl.minimum;
                } else {
                    quality_reset = qctrl.minimum + ((quality - qctrl.minimum) / qctrl.step) * qctrl.step;
                }

                ESP_LOGW(TAG, "video%d: JPEG compression quality=%d is out of sensor's range, reset to %d", video->index, quality, quality_reset);
            }

            controls.ctrl_class = V4L2_CID_JPEG_CLASS;
            controls.count = 1;
            controls.controls = control;
            control[0].id = V4L2_CID_JPEG_COMPRESSION_QUALITY;
            control[0].value = quality_reset;
            ESP_RETURN_ON_ERROR(ioctl(video->fd, VIDIOC_S_EXT_CTRLS, &controls), TAG, "failed to set jpeg compression quality");

            video->jpeg_quality = quality_reset;
            video->support_control_jpeg_quality = 1;
        } else {
            video->support_control_jpeg_quality = 0;
            ESP_LOGW(TAG, "video%d: JPEG compression quality control is not supported", video->index);
        }
    } else {
        ESP_RETURN_ON_ERROR(example_encoder_set_jpeg_quality(video->encoder_handle, quality_reset), TAG, "failed to set jpeg quality");
        video->jpeg_quality = quality_reset;
    }

    if (video->support_control_jpeg_quality) {
        ESP_LOGI(TAG, "video%d: set jpeg quality %d success", video->index, quality_reset);
    }

    return ret;
}

static esp_err_t set_expose(int fd, uint8_t level)
{
    struct v4l2_ext_controls controls;
    struct v4l2_ext_control control[1];
    controls.ctrl_class = V4L2_CTRL_CLASS_USER;
    controls.count = 1;
    controls.controls = control;
    control[0].id = V4L2_CID_CAMERA_AE_LEVEL;
    control[0].value = level;

    if (ioctl(fd, VIDIOC_S_EXT_CTRLS, &controls) != 0) {
        ESP_LOGE(TAG, "failed to set AE level");
        close(fd);
        return ESP_FAIL;
    }

    return ESP_OK;
}

static esp_err_t camera_info_handler(httpd_req_t *req)
{
    esp_err_t ret;
    web_cam_t *web_cam = (web_cam_t *)req->user_ctx;
    char *output = get_cameras_json(web_cam);

    httpd_resp_set_type(req, "application/json");
    ret = httpd_resp_sendstr(req, output);
    free(output);

    return ret;
}

static esp_err_t camera_settings_handler(httpd_req_t *req)
{
    esp_err_t ret = ESP_OK;
    char *content;
    web_cam_t *web_cam = (web_cam_t *)req->user_ctx;

    content = (char *)calloc(1, req->content_len + 1);
    ESP_RETURN_ON_FALSE(content, ESP_ERR_NO_MEM, TAG, "failed to allocate memory");

    ESP_GOTO_ON_FALSE(httpd_req_recv(req, content, req->content_len) > 0, ESP_FAIL, fail0, TAG, "failed to recv content");
    ESP_LOGD(TAG, "content: %s", content);

    cJSON *json_root = cJSON_Parse(content);
    free(content);
    content = NULL;
    ESP_GOTO_ON_FALSE(json_root, ESP_FAIL, fail0, TAG, "failed to parse JSON");

    cJSON *json_index = cJSON_GetObjectItem(json_root, "index");
    ESP_GOTO_ON_FALSE(json_index && cJSON_IsNumber(json_index), ESP_ERR_INVALID_ARG, fail1, TAG, "missing or invalid index field");
    int index = json_index->valueint;
    ESP_GOTO_ON_FALSE(index >= 0 && index < web_cam->video_count && is_valid_web_cam(&web_cam->video[index]), ESP_ERR_INVALID_ARG, fail1, TAG, "invalid index");

    cJSON *json_image_format = cJSON_GetObjectItem(json_root, "image_format");
    if (json_image_format && cJSON_IsNumber(json_image_format)) {
        int fmt = json_image_format->valueint;
        if (fmt == 0) {
            set_camera_resolution(&web_cam->video[index], 640, 480);
        } else if (fmt == 1) {
            set_camera_resolution(&web_cam->video[index], 800, 600);
        }
    }

    cJSON *json_jpeg_quality = cJSON_GetObjectItem(json_root, "jpeg_quality");
    if (json_jpeg_quality && cJSON_IsNumber(json_jpeg_quality)) {
        int jpeg_quality = json_jpeg_quality->valueint;
        set_camera_jpeg_quality(&web_cam->video[index], jpeg_quality);
    }

    ESP_LOGI(TAG, "Camera settings updated for video%d", index);
    cJSON_Delete(json_root);
    json_root = NULL;

    httpd_resp_sendstr(req, "OK");
    return ESP_OK;

fail1:
    if (json_root) {
        cJSON_Delete(json_root);
    }
fail0:
    if (ret == HTTPD_SOCK_ERR_TIMEOUT) {
        httpd_resp_send_408(req);
    } else {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON format");
    }
    if (content) {
        free(content);
    }
    return ret;
}

static esp_err_t static_file_handler(httpd_req_t *req)
{
    const char *uri = req->uri;

    /* Route to appropriate static file based on URI */
    if (strcmp(uri, "/") == 0) {
        httpd_resp_set_type(req, "text/html");
        httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
        return httpd_resp_send(req, (const char *)index_html_gz_start, index_html_gz_end - index_html_gz_start);
    } else if (strcmp(uri, "/loading.jpg") == 0) {
        httpd_resp_set_type(req, "image/jpeg");
        httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
        return httpd_resp_send(req, (const char *)loading_jpg_gz_start, loading_jpg_gz_end - loading_jpg_gz_start);
    } else if (strcmp(uri, "/favicon.ico") == 0) {
        httpd_resp_set_type(req, "image/x-icon");
        httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
        return httpd_resp_send(req, (const char *)favicon_ico_gz_start, favicon_ico_gz_end - favicon_ico_gz_start);
    } else if (strcmp(uri, "/assets/index.js") == 0) {
        httpd_resp_set_type(req, "application/javascript");
        httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
        return httpd_resp_send(req, (const char *)assets_index_js_gz_start, assets_index_js_gz_end - assets_index_js_gz_start);
    } else if (strcmp(uri, "/assets/index.css") == 0) {
        httpd_resp_set_type(req, "text/css");
        httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
        return httpd_resp_send(req, (const char *)assets_index_css_gz_start, assets_index_css_gz_end - assets_index_css_gz_start);
    }

    /* If no static file matches, return 404 */
    ESP_LOGW(TAG, "File not found: %s", uri);
    httpd_resp_send_404(req);
    return ESP_FAIL;
}

static esp_err_t restart_camera_capture(web_cam_video_t *video)
{
    int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

    video->frame_wait_start_us = 0;
    ESP_RETURN_ON_ERROR(ioctl(video->fd, VIDIOC_STREAMOFF, &type), TAG,
                        "video%d: failed to stop camera capture", video->index);

    for (int i = 0; i < EXAMPLE_CAMERA_VIDEO_BUFFER_NUMBER; i++) {
        struct v4l2_buffer buf = {
            .type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
            .memory = V4L2_MEMORY_MMAP,
            .index = i,
        };
        ESP_RETURN_ON_ERROR(ioctl(video->fd, VIDIOC_QBUF, &buf), TAG,
                            "video%d: failed to requeue buffer %d", video->index, i);
    }

    ESP_RETURN_ON_ERROR(ioctl(video->fd, VIDIOC_STREAMON, &type), TAG,
                        "video%d: failed to restart camera capture", video->index);
    ESP_LOGW(TAG, "video%d: camera capture restarted after frame timeout", video->index);
    return ESP_OK;
}

static esp_err_t image_stream_handler(httpd_req_t *req)
{
    esp_err_t ret;
    struct v4l2_buffer buf;
    char http_string[128];
    bool locked = false;
    web_cam_video_t *video = (web_cam_video_t *)req->user_ctx;

    ESP_RETURN_ON_FALSE(snprintf(http_string, sizeof(http_string), "%" PRIu32, video->frame_rate) > 0,
                        ESP_FAIL, TAG, "failed to format framerate buffer");

    ESP_RETURN_ON_ERROR(httpd_resp_set_type(req, STREAM_CONTENT_TYPE), TAG, "failed to set content type");
    ESP_RETURN_ON_ERROR(httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*"), TAG, "failed to set access control allow origin");
    ESP_RETURN_ON_ERROR(httpd_resp_set_hdr(req, "X-Framerate", http_string), TAG, "failed to set x framerate");

    int stream_sock = httpd_req_to_sockfd(req);
    ESP_LOGI(TAG, "video%d: /stream client connected (socket=%d)",
             video->index, stream_sock);
    if (stream_sock >= 0) {
        int nodelay = 1;
        setsockopt(stream_sock, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));
        struct timeval tv = { .tv_sec = 0, .tv_usec = 300000 }; // 300ms max send timeout
        setsockopt(stream_sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    }
    bool first_frame_sent = false;
    int64_t stats_start = esp_timer_get_time();
    int64_t last_frame_time = 0;
    unsigned sent_frames = 0;
    uint32_t consecutive_frame_timeouts = 0;

    while (1) {
        int hlen;
        struct timespec ts;
        uint32_t jpeg_encoded_size;

        locked = false;

        memset(&buf, 0, sizeof(buf));
        buf.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        video->frame_wait_start_us = esp_timer_get_time();
        if (ioctl(video->fd, VIDIOC_DQBUF, &buf) != 0) {
            video->frame_wait_start_us = 0;
            consecutive_frame_timeouts++;
            ESP_LOGE(TAG,
                     "video%d: no frame in %d seconds (errno=%d, timeouts=%" PRIu32 ")",
                     video->index, CAMERA_DQBUF_TIMEOUT_SECONDS, errno,
                     consecutive_frame_timeouts);
            ESP_RETURN_ON_ERROR(restart_camera_capture(video), TAG,
                                "video%d: camera capture recovery failed", video->index);
            continue;
        }
        video->frame_wait_start_us = 0;
        if (consecutive_frame_timeouts > 0) {
            ESP_LOGI(TAG, "video%d: camera frame reception recovered after %" PRIu32 " timeouts",
                     video->index, consecutive_frame_timeouts);
            consecutive_frame_timeouts = 0;
        }
        if (!(buf.flags & V4L2_BUF_FLAG_DONE)) {
            ESP_RETURN_ON_ERROR(ioctl(video->fd, VIDIOC_QBUF, &buf), TAG, "failed to queue video frame");
            continue;
        }

        /* Check if SDIO / Wi-Fi TX queue is currently throttled by C5 coprocessor */
        if (wifi_tx_throttling) {
            ioctl(video->fd, VIDIOC_QBUF, &buf);
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        /* Enforce minimum interval of 50ms (~18-20 FPS) since previous frame finished sending */
        int64_t now_us = esp_timer_get_time();
        if (last_frame_time > 0 && (now_us - last_frame_time) < 50000) {
            ioctl(video->fd, VIDIOC_QBUF, &buf);
            continue;
        }

        /* Non-blocking socket write check: drop frame immediately if TCP transmit buffer is congested */
        if (stream_sock >= 0) {
            fd_set wfds;
            FD_ZERO(&wfds);
            FD_SET(stream_sock, &wfds);
            struct timeval tv_poll = { .tv_sec = 0, .tv_usec = 0 };
            if (select(stream_sock + 1, NULL, &wfds, NULL, &tv_poll) <= 0) {
                ioctl(video->fd, VIDIOC_QBUF, &buf);
                continue;
            }
        }

        ESP_GOTO_ON_ERROR(pack_frame(video, &buf), fail0, TAG, "failed to pack frame");

        if (video->pixel_format == V4L2_PIX_FMT_JPEG) {
            video->jpeg_out_buf = video->buffer[buf.index];
            jpeg_encoded_size = buf.bytesused;
        } else {
            ESP_GOTO_ON_FALSE(xSemaphoreTake(video->sem, portMAX_DELAY) == pdPASS, ESP_FAIL, fail0, TAG, "failed to take semaphore");
            locked = true;

            ESP_GOTO_ON_ERROR(example_encoder_process(video->encoder_handle, video->buffer[buf.index], buf.bytesused,
                              video->jpeg_out_buf, video->jpeg_out_size, &jpeg_encoded_size),
                              fail0, TAG, "failed to encode video frame");
            xSemaphoreGive(video->sem);
            locked = false;
        }

        ESP_GOTO_ON_ERROR(clock_gettime(CLOCK_MONOTONIC, &ts), fail0, TAG, "failed to get time");

        /* Send boundary + header in a single chunk to reduce TCP packet overhead */
        char part_hdr[160];
        hlen = snprintf(part_hdr, sizeof(part_hdr),
                        "%sContent-Type: image/jpeg\r\nContent-Length: %" PRIu32 "\r\nX-Timestamp: %ld.%06ld\r\n\r\n",
                        STREAM_BOUNDARY, jpeg_encoded_size, (long)ts.tv_sec, (long)(ts.tv_nsec / 1000));
        ESP_GOTO_ON_FALSE(hlen > 0, ESP_FAIL, fail0, TAG, "failed to format part header");
        ESP_GOTO_ON_ERROR(httpd_resp_send_chunk(req, part_hdr, hlen), fail0, TAG, "failed to send header");
        ESP_GOTO_ON_ERROR(httpd_resp_send_chunk(req, (char *)video->jpeg_out_buf, jpeg_encoded_size),
                          fail0, TAG, "failed to send jpeg");
        int64_t now = esp_timer_get_time();
        last_frame_time = now;
        vTaskDelay(1);
        ++sent_frames;
        portENTER_CRITICAL(&s_stats_lock);
        s_last_sent = now;
        if (now - stats_start >= 1000000) {
            s_stream_fps = sent_frames * 1000000.0f / (now - stats_start);
            sent_frames = 0;
            stats_start = now;
        }
        portEXIT_CRITICAL(&s_stats_lock);
        if (!first_frame_sent) {
            ESP_LOGI(TAG, "video%d: first JPEG frame sent (%" PRIu32 " bytes)",
                     video->index, jpeg_encoded_size);
            first_frame_sent = true;
        }
        if (locked) {
            xSemaphoreGive(video->sem);
            locked = false;
        }

        ESP_RETURN_ON_ERROR(ioctl(video->fd, VIDIOC_QBUF, &buf), TAG, "failed to queue video frame");
    }

    return ESP_OK;

fail0:
    if (locked) {
        xSemaphoreGive(video->sem);
    }
    ioctl(video->fd, VIDIOC_QBUF, &buf);
    return ret;
}

static esp_err_t detections_handler(httpd_req_t *req)
{
    portENTER_CRITICAL(&s_stats_lock);
    float fps = s_stream_fps;
    int64_t last_sent = s_last_sent;
    portEXIT_CRITICAL(&s_stats_lock);
    if (esp_timer_get_time() - last_sent > 2000000) fps = 0;

    uint32_t w = 800, h = 600;
    if (s_web_cam && s_web_cam->video_count > 0) {
        w = s_web_cam->video[0].width;
        h = s_web_cam->video[0].height;
    }

    char json[256];
    snprintf(json, sizeof(json),
             "{\"title\":\"图像回传AP模式（Image Transmission AP Mode）\",\"width\":%" PRIu32 ",\"height\":%" PRIu32 ",\"tags\":[],\"video_fps\":%.1f}",
             w, h, fps);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_sendstr(req, json);
}

static esp_err_t tests_handler(httpd_req_t *req)
{
    char json[256];
    ESP_RETURN_ON_ERROR(hardware_test_json(json, sizeof(json)), TAG, "Test JSON failed");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_sendstr(req, json);
}

static esp_err_t capture_image_handler(httpd_req_t *req)
{
    web_cam_t *web_cam = (web_cam_t *)req->user_ctx;

    request_desc_t desc;
    ESP_RETURN_ON_ERROR(decode_request(web_cam, req, &desc), TAG, "failed to decode request");

    char type_ptr[32];
    ESP_RETURN_ON_FALSE(snprintf(type_ptr, sizeof(type_ptr), "image/jpeg;name=image%d.jpg", desc.index) > 0, ESP_FAIL, TAG, "failed to format buffer");
    ESP_RETURN_ON_ERROR(httpd_resp_set_type(req, type_ptr), TAG, "failed to set content type");

    return capture_video_image(req, &web_cam->video[desc.index], true);
}

static esp_err_t capture_binary_handler(httpd_req_t *req)
{
    web_cam_t *web_cam = (web_cam_t *)req->user_ctx;

    request_desc_t desc;
    ESP_RETURN_ON_ERROR(decode_request(web_cam, req, &desc), TAG, "failed to decode request");

    char type_ptr[56];
    ESP_RETURN_ON_FALSE(snprintf(type_ptr, sizeof(type_ptr), "application/octet-stream;name=image_binary%d.bin", desc.index) > 0, ESP_FAIL, TAG, "failed to format buffer");
    ESP_RETURN_ON_ERROR(httpd_resp_set_type(req, type_ptr), TAG, "failed to set content type");

    return capture_video_image(req, &web_cam->video[desc.index], false);
}

static esp_err_t init_web_cam_video(web_cam_video_t *video, const web_cam_video_config_t *config, int index)
{
    int fd;
    int ret;
    struct v4l2_format format;
    struct v4l2_streamparm sparm;
    struct v4l2_requestbuffers req;
    struct v4l2_captureparm *cparam = &sparm.parm.capture;
    struct v4l2_fract *timeperframe = &cparam->timeperframe;

    struct v4l2_ext_controls controls;
    controls.ctrl_class = V4L2_CTRL_CLASS_USER;
    struct v4l2_ext_control control[1];
    controls.count = 1;
    controls.controls = control;

    fd = open(config->dev_name, O_RDWR);
    ESP_RETURN_ON_FALSE(fd >= 0, ESP_ERR_NOT_FOUND, TAG, "Open video device %s failed", config->dev_name);

    struct timeval dqbuf_timeout = {
        .tv_sec = CAMERA_DQBUF_TIMEOUT_SECONDS,
        .tv_usec = 0,
    };
    ESP_GOTO_ON_ERROR(ioctl(fd, VIDIOC_S_DQBUF_TIMEOUT, &dqbuf_timeout), fail0, TAG,
                      "failed to set frame receive timeout for %s", config->dev_name);

    memset(&format, 0, sizeof(struct v4l2_format));
    format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    ESP_GOTO_ON_ERROR(ioctl(fd, VIDIOC_G_FMT, &format), fail0, TAG, "Failed get fmt from %s", config->dev_name);

#if CONFIG_EXAMPLE_SELECT_JPEG_HW_DRIVER
    if (format.fmt.pix.pixelformat == V4L2_PIX_FMT_RGB565X) {
#if CONFIG_ESP_VIDEO_ENABLE_SWAP_BYTE
        ESP_LOGW(TAG, "The hardware JPEG encoder does not support RGB565 big endian. Instead, use RGB565 little endian by enabling the byte swap function.");

        format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        format.fmt.pix.pixelformat = V4L2_PIX_FMT_RGB565;
        ESP_GOTO_ON_ERROR(ioctl(fd, VIDIOC_S_FMT, &format), fail0, TAG, "failed to set fmt to %s", config->dev_name);
#else
        ESP_GOTO_ON_ERROR(ESP_FAIL, fail0, TAG, "The hardware JPEG encoder does not support RGB565 big endian. Please enable the byte swap function ESP_VIDEO_ENABLE_SWAP_BYTE in menuconfig.");
#endif
    }
#endif


    control[0].id = V4L2_CID_HFLIP;
    control[0].value = 1;
    ESP_GOTO_ON_ERROR(ioctl(fd, VIDIOC_S_EXT_CTRLS, &controls), fail0, TAG, "Failed to set hflip control for %s", config->dev_name);
    vTaskDelay(pdMS_TO_TICKS(50));
    control[0].id = V4L2_CID_VFLIP;
    control[0].value = 1;
    ESP_GOTO_ON_ERROR(ioctl(fd, VIDIOC_S_EXT_CTRLS, &controls), fail0, TAG, "Failed to set vflip control for %s", config->dev_name);

    ret = set_expose(fd, 55);
    if (ret != ESP_OK) {
        fd = -1;
        goto fail0;
    }

    memset(&sparm, 0, sizeof(sparm));
    sparm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    ESP_GOTO_ON_ERROR(ioctl(fd, VIDIOC_G_PARM, &sparm), fail0, TAG, "failed to get frame rate from %s", config->dev_name);
    video->frame_rate = timeperframe->denominator / timeperframe->numerator;

    /* Preserve the native sensor dimensions. */
    ESP_GOTO_ON_FALSE(format.fmt.pix.pixelformat == V4L2_PIX_FMT_RGB565 &&
                      format.fmt.pix.width >= 640 && format.fmt.pix.height >= 480,
                      ESP_ERR_NOT_SUPPORTED, fail0, TAG, "Need RGB565 input at least 640x480");
    video->capture_width = format.fmt.pix.width;
    video->capture_height = format.fmt.pix.height;
    video->capture_stride = format.fmt.pix.bytesperline ? format.fmt.pix.bytesperline : video->capture_width * 2;
    ESP_GOTO_ON_FALSE(video->capture_stride >= video->capture_width * 2,
                      ESP_ERR_INVALID_SIZE, fail0, TAG, "Invalid camera stride");

    memset(&req, 0, sizeof(req));
    req.count  = EXAMPLE_CAMERA_VIDEO_BUFFER_NUMBER;
    req.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_MMAP;
    ESP_GOTO_ON_ERROR(ioctl(fd, VIDIOC_REQBUFS, &req), fail0, TAG, "failed to req buffers from %s", config->dev_name);

    for (int i = 0; i < EXAMPLE_CAMERA_VIDEO_BUFFER_NUMBER; i++) {
        struct v4l2_buffer buf;

        memset(&buf, 0, sizeof(buf));
        buf.type        = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory      = V4L2_MEMORY_MMAP;
        buf.index       = i;
        ESP_GOTO_ON_ERROR(ioctl(fd, VIDIOC_QUERYBUF, &buf), fail0, TAG, "failed to query vbuf from %s", config->dev_name);

        video->buffer[i] = mmap(NULL, buf.length, PROT_READ | PROT_WRITE, MAP_SHARED, fd, buf.m.offset);
        ESP_GOTO_ON_FALSE(video->buffer[i] != MAP_FAILED, ESP_ERR_NO_MEM, fail0, TAG, "failed to mmap buffer");
        video->buffer_size = buf.length;

        ESP_GOTO_ON_ERROR(ioctl(fd, VIDIOC_QBUF, &buf), fail0, TAG, "failed to queue frame vbuf from %s", config->dev_name);
    }

    video->fd = fd;
    video->width = format.fmt.pix.width;
    video->height = format.fmt.pix.height;
    ESP_LOGI(TAG, "Full frame: %" PRIu32 "x%" PRIu32, video->width, video->height);
    video->pixel_format = format.fmt.pix.pixelformat;
    video->jpeg_quality = 38;

    if (video->pixel_format == V4L2_PIX_FMT_JPEG) {
        ESP_GOTO_ON_ERROR(set_camera_jpeg_quality(video, 50), fail0, TAG, "failed to set jpeg quality");
    } else {
        example_encoder_config_t encoder_config = {0};

        encoder_config.width = video->width;
        encoder_config.height = video->height;
        encoder_config.pixel_format = video->pixel_format;
        encoder_config.quality = 38;
        ESP_GOTO_ON_ERROR(example_encoder_init(&encoder_config, &video->encoder_handle), fail0, TAG, "failed to init encoder");

        ESP_GOTO_ON_ERROR(example_encoder_alloc_output_buffer(video->encoder_handle, &video->jpeg_out_buf, &video->jpeg_out_size),
                          fail1, TAG, "failed to alloc jpeg output buf");

        video->support_control_jpeg_quality = 1;
    }

    video->sem = xSemaphoreCreateBinary();
    ESP_GOTO_ON_FALSE(video->sem, ESP_ERR_NO_MEM, fail2, TAG, "failed to create semaphore");
    xSemaphoreGive(video->sem);

    /* Default to 640x480 resolution */
    set_camera_resolution(video, 640, 480);

    return ESP_OK;

fail2:
    if (video->pixel_format != V4L2_PIX_FMT_JPEG) {
        example_encoder_free_output_buffer(video->encoder_handle, video->jpeg_out_buf);
        video->jpeg_out_buf = NULL;
    }
fail1:
    if (video->pixel_format != V4L2_PIX_FMT_JPEG) {
        example_encoder_deinit(video->encoder_handle);
        video->encoder_handle = NULL;
    }
fail0:
    close(fd);
    video->fd = -1;
    return ret;
}

static esp_err_t deinit_web_cam_video(web_cam_video_t *video)
{
    if (video->frame_watchdog_task) {
        vTaskDelete(video->frame_watchdog_task);
        video->frame_watchdog_task = NULL;
    }

    if (video->sem) {
        vSemaphoreDelete(video->sem);
        video->sem = NULL;
    }

    if (video->pixel_format != V4L2_PIX_FMT_JPEG) {
        example_encoder_free_output_buffer(video->encoder_handle, video->jpeg_out_buf);
        example_encoder_deinit(video->encoder_handle);
    }

    close(video->fd);
    return ESP_OK;
}

static esp_err_t new_web_cam(const web_cam_video_config_t *config, int config_count, web_cam_t **ret_wc)
{
    int i;
    web_cam_t *wc;
    esp_err_t ret = ESP_FAIL;
    int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

    wc = calloc(1, sizeof(web_cam_t) + config_count * sizeof(web_cam_video_t));
    ESP_RETURN_ON_FALSE(wc, ESP_ERR_NO_MEM, TAG, "failed to alloc web cam");
    wc->video_count = config_count;

    for (i = 0; i < config_count; i++) {
        wc->video[i].index = i;
        wc->video[i].fd = -1;

        ret = init_web_cam_video(&wc->video[i], &config[i], i);
        if (ret == ESP_ERR_NOT_FOUND) {
            ESP_LOGW(TAG, "failed to find web_cam %d", i);
            continue;
        } else if (ret != ESP_OK) {
            ESP_LOGE(TAG, "failed to initialize web_cam %d", i);
            goto fail0;
        }

        ESP_LOGI(TAG, "video%d: width=%" PRIu32 " height=%" PRIu32 " format=" V4L2_FMT_STR, i, wc->video[i].width,
                 wc->video[i].height, V4L2_FMT_STR_ARG(wc->video[i].pixel_format));
    }

    for (i = 0; i < config_count; i++) {
        if (is_valid_web_cam(&wc->video[i])) {
            ESP_GOTO_ON_ERROR(ioctl(wc->video[i].fd, VIDIOC_STREAMON, &type), fail1, TAG, "failed to start stream");
            BaseType_t task_created = xTaskCreate(camera_frame_watchdog_task, "camera_frame_wd",
                                                  2048, &wc->video[i], 5,
                                                  &wc->video[i].frame_watchdog_task);
            ESP_GOTO_ON_FALSE(task_created == pdPASS, ESP_ERR_NO_MEM, fail1, TAG,
                              "failed to create video%d frame watchdog", i);
        }
    }

    *ret_wc = wc;

    return ESP_OK;

fail1:
    for (int j = i - 1; j >= 0; j--) {
        if (is_valid_web_cam(&wc->video[j])) {
            ioctl(wc->video[j].fd, VIDIOC_STREAMOFF, &type);
        }
    }
    i = config_count; // deinit all web_cam
fail0:
    for (int j = i - 1; j >= 0; j--) {
        if (is_valid_web_cam(&wc->video[j])) {
            deinit_web_cam_video(&wc->video[j]);
        }
    }
    free(wc);
    return ret;
}

static esp_err_t http_server_init(web_cam_t *web_cam)
{
    esp_err_t ret;
    httpd_handle_t stream_httpd;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.uri_match_fn = httpd_uri_match_wildcard;
    config.lru_purge_enable = true;

    /* Unified static file handler for all static resources */
    httpd_uri_t static_file_uri = {
        .uri = "/*",
        .method = HTTP_GET,
        .handler = static_file_handler,
        .user_ctx = (void *)web_cam
    };

    /* API handlers */
    httpd_uri_t capture_image_uri = {
        .uri = "/api/capture_image",
        .method = HTTP_GET,
        .handler = capture_image_handler,
        .user_ctx = (void *)web_cam
    };

    httpd_uri_t capture_binary_uri = {
        .uri = "/api/capture_binary",
        .method = HTTP_GET,
        .handler = capture_binary_handler,
        .user_ctx = (void *)web_cam
    };

    httpd_uri_t camera_info_uri = {
        .uri = "/api/get_camera_info",
        .method = HTTP_GET,
        .handler = camera_info_handler,
        .user_ctx = (void *)web_cam
    };

    httpd_uri_t camera_settings_uri = {
        .uri = "/api/set_camera_config",
        .method = HTTP_POST,
        .handler = camera_settings_handler,
        .user_ctx = (void *)web_cam
    };

    httpd_uri_t detections_uri = {
        .uri = "/api/detections",
        .method = HTTP_GET,
        .handler = detections_handler,
        .user_ctx = (void *)web_cam
    };

    config.stack_size = 1024 * 6;
    const httpd_uri_t tests_uri = {
        .uri = "/api/tests", .method = HTTP_GET, .handler = tests_handler,
    };
    ESP_LOGI(TAG, "Starting web server on port: '%d'", config.server_port);
    ESP_RETURN_ON_ERROR(httpd_start(&stream_httpd, &config), TAG,
                        "failed to start web server on port %d", config.server_port);
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(stream_httpd, &capture_image_uri), TAG,
                        "failed to register capture image handler");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(stream_httpd, &capture_binary_uri), TAG,
                        "failed to register capture binary handler");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(stream_httpd, &camera_info_uri), TAG,
                        "failed to register camera info handler");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(stream_httpd, &camera_settings_uri), TAG,
                        "failed to register camera settings handler");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(stream_httpd, &detections_uri), TAG,
                        "failed to register detections handler");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(stream_httpd, &tests_uri), TAG,
                        "failed to register tests handler");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(stream_httpd, &static_file_uri), TAG,
                        "failed to register static file handler");

    for (int i = 0; i < web_cam->video_count; i++) {
        if (!is_valid_web_cam(&web_cam->video[i])) {
            continue;
        }

        httpd_uri_t stream_0_uri = {
            .uri = "/stream",
            .method = HTTP_GET,
            .handler = image_stream_handler,
            .user_ctx = (void *) &web_cam->video[i]
        };

        /* CPU0 streams continuously; CPU1 detects on its own copied image. */
        config.stack_size = 1024 * 8;
        config.core_id = 0;
        config.task_priority = 5;
        config.server_port += 1;
        config.ctrl_port += 1;
        ESP_LOGI(TAG, "Starting video%d stream server on port: '%d'", i, config.server_port);
        ret = httpd_start(&stream_httpd, &config);
        ESP_RETURN_ON_ERROR(ret, TAG, "failed to start video%d stream server on port %d",
                            i, config.server_port);
        ESP_RETURN_ON_ERROR(httpd_register_uri_handler(stream_httpd, &stream_0_uri), TAG,
                            "failed to register video%d stream handler", i);
    }

    return ESP_OK;
}

esp_err_t camera_service_init(void)
{
    web_cam_video_config_t config[] = {
#if EXAMPLE_ENABLE_MIPI_CSI_CAM_SENSOR
        {
            .dev_name = ESP_VIDEO_MIPI_CSI_DEVICE_NAME,
        },
#endif
#if EXAMPLE_ENABLE_DVP_CAM_SENSOR
        {
            .dev_name = ESP_VIDEO_DVP_DEVICE_NAME,
        },
#endif
#if EXAMPLE_ENABLE_SPI_CAM_0_SENSOR
        {
            .dev_name = ESP_VIDEO_SPI_DEVICE_NAME,
        },
#endif
#if EXAMPLE_ENABLE_SPI_CAM_1_SENSOR
        {
            .dev_name = ESP_VIDEO_SPI_DEVICE_1_NAME,
        },
#endif
#if EXAMPLE_ENABLE_USB_UVC_CAM_SENSOR
        {
            .dev_name = ESP_VIDEO_USB_UVC_DEVICE_NAME(0),
        },
#endif
    };

    int config_count = sizeof(config) / sizeof(config[0]);
    assert(config_count > 0);

    ESP_RETURN_ON_ERROR(example_video_init(), TAG, "Failed to initialize video subsystem");
    ESP_RETURN_ON_ERROR(new_web_cam(config, config_count, &s_web_cam), TAG,
                        "Failed to initialize and start camera capture");

    ESP_LOGI(TAG, "Camera capture started before network initialization");

    return ESP_OK;
}

esp_err_t camera_service_start(void)
{
    static web_cam_t empty_camera;
    return http_server_init(s_web_cam ? s_web_cam : &empty_camera);
}
