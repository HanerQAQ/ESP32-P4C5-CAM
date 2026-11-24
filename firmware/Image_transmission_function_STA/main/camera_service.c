/*
 * SPDX-FileCopyrightText: 2024-2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: ESPRESSIF MIT
 */

#include <string.h>
#include <strings.h>
#include <stdlib.h>
#include <limits.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/param.h>
#include <sys/errno.h>
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "cJSON.h"
#include "esp_event.h"
#include "esp_err.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_http_server.h"
#include "example_video_common.h"
#include "camera_service.h"
#include "app_mywifi.h"
#include "esp_wifi.h"
#include "wifi_config_page.h"
#if CONFIG_EXAMPLE_SELECT_JPEG_HW_DRIVER
#include "driver/jpeg_encode.h"
typedef struct {
    jpeg_encode_cfg_t jpeg_enc_config;
    uint32_t jpeg_out_buf_size;
} hw_jpeg_encoder_ctx_t;
#endif

#define EXAMPLE_CAMERA_VIDEO_BUFFER_NUMBER  CONFIG_EXAMPLE_CAMERA_VIDEO_BUFFER_NUMBER

#define EXAMPLE_JPEG_ENC_QUALITY            CONFIG_EXAMPLE_JPEG_COMPRESSION_QUALITY

#define EXAMPLE_PART_BOUNDARY               CONFIG_EXAMPLE_HTTP_PART_BOUNDARY

#define CAMERA_DQBUF_TIMEOUT_SECONDS        2
#define CAMERA_HTTP_BODY_SIZE              512
#define CAMERA_HTTP_BODY_TIMEOUT_US        5000000

/* Wire values from the S3 esp-camera sensor.h, not P4 image_format IDs. */
#define S3_FRAMESIZE_VGA                    8
#define S3_FRAMESIZE_SVGA                   9
#define S3_PIXFORMAT_RGB565                 0

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
static uint32_t s_stream_clients;
static uint32_t s_stream_frames;
/* Completed, successfully delivered frames (stream plus snapshots). */
static uint32_t s_frames_captured;
static uint32_t s_last_jpeg;

static int s_cur_hflip = 1;
static int s_cur_vflip = 1;

static esp_err_t set_camera_resolution(web_cam_video_t *video, uint32_t width, uint32_t height);
static esp_err_t set_camera_jpeg_quality(web_cam_video_t *video, int quality);
static esp_err_t restart_camera_capture(web_cam_video_t *video);

static esp_err_t set_camera_flip(uint32_t id, int value)
{
    if (value != 0 && value != 1) return ESP_ERR_INVALID_ARG;
    if (!s_web_cam || s_web_cam->video_count == 0) return ESP_ERR_INVALID_STATE;
    bool updated = false;
    for (int i = 0; i < s_web_cam->video_count; i++) {
        web_cam_video_t *video = &s_web_cam->video[i];
        if (video->fd < 0) continue;
        if (xSemaphoreTake(video->sem, pdMS_TO_TICKS(1000)) != pdPASS) return ESP_ERR_TIMEOUT;
        struct v4l2_ext_control control = { .id = id, .value = value };
        struct v4l2_ext_controls controls = {
            .ctrl_class = V4L2_CTRL_CLASS_USER, .count = 1, .controls = &control,
        };
        int result = ioctl(video->fd, VIDIOC_S_EXT_CTRLS, &controls);
        if (result == 0) {
            /* /status reports the primary camera, including partial multi-camera failures. */
            if (i == 0) {
                portENTER_CRITICAL(&s_stats_lock);
                if (id == V4L2_CID_HFLIP) s_cur_hflip = value;
                else s_cur_vflip = value;
                portEXIT_CRITICAL(&s_stats_lock);
            }
            updated = true;
        }
        xSemaphoreGive(video->sem);
        if (result != 0) {
            ESP_LOGE(TAG, "video%d: failed to set flip control", i);
            return ESP_FAIL;
        }
    }
    return updated ? ESP_OK : ESP_ERR_INVALID_STATE;
}

esp_err_t camera_service_set_hflip(int hflip)
{
    return set_camera_flip(V4L2_CID_HFLIP, hflip);
}

esp_err_t camera_service_set_vflip(int vflip)
{
    return set_camera_flip(V4L2_CID_VFLIP, vflip);
}

int camera_service_get_hflip(void)
{
    portENTER_CRITICAL(&s_stats_lock);
    int value = s_cur_hflip;
    portEXIT_CRITICAL(&s_stats_lock);
    return value;
}

int camera_service_get_vflip(void)
{
    portENTER_CRITICAL(&s_stats_lock);
    int value = s_cur_vflip;
    portEXIT_CRITICAL(&s_stats_lock);
    return value;
}

esp_err_t camera_service_set_quality(int quality)
{
    if (!s_web_cam || s_web_cam->video_count == 0) return ESP_ERR_INVALID_STATE;
    return set_camera_jpeg_quality(&s_web_cam->video[0], quality);
}

int camera_service_get_quality(void)
{
    portENTER_CRITICAL(&s_stats_lock);
    int quality = (s_web_cam && s_web_cam->video_count > 0) ? s_web_cam->video[0].jpeg_quality : 38;
    portEXIT_CRITICAL(&s_stats_lock);
    return quality;
}

esp_err_t camera_service_set_resolution(uint32_t width, uint32_t height)
{
    if (!s_web_cam || s_web_cam->video_count == 0) return ESP_ERR_INVALID_STATE;
    return set_camera_resolution(&s_web_cam->video[0], width, height);
}

void camera_service_get_resolution(uint32_t *width, uint32_t *height)
{
    portENTER_CRITICAL(&s_stats_lock);
    if (width) *width = (s_web_cam && s_web_cam->video_count > 0) ? s_web_cam->video[0].width : 640;
    if (height) *height = (s_web_cam && s_web_cam->video_count > 0) ? s_web_cam->video[0].height : 480;
    portEXIT_CRITICAL(&s_stats_lock);
}

float camera_service_get_fps(void)
{
    portENTER_CRITICAL(&s_stats_lock);
    float fps = s_stream_fps;
    int64_t last_sent = s_last_sent;
    portEXIT_CRITICAL(&s_stats_lock);
    if (esp_timer_get_time() - last_sent > 2000000) fps = 0;
    return fps;
}

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
    int index = 0;
    char buffer[32];

    if (httpd_req_get_url_query_str(req, buffer, sizeof(buffer)) == ESP_OK) {
        for (int i = 0; i < web_cam->video_count; i++) {
            char source_str[16];
            if (snprintf(source_str, sizeof(source_str), "source=%d", i) > 0) {
                if (strstr(buffer, source_str) != NULL) {
                    index = i;
                    break;
                }
            }
        }
    }

    if (index >= 0 && index < web_cam->video_count && is_valid_web_cam(&web_cam->video[index])) {
        desc->index = index;
        return ESP_OK;
    }
    return ESP_FAIL;
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
        const size_t required = (y_offset + crop_h - 1) * video->capture_stride + x_offset * 2 + row_bytes;
        if (required > buf->bytesused || required > video->buffer_size) return ESP_ERR_INVALID_SIZE;
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
    if (!video || !((width == 640 && height == 480) || (width == 800 && height == 600))) {
        return ESP_ERR_INVALID_ARG;
    }
    if (video->fd < 0 || !video->sem) return ESP_ERR_INVALID_STATE;
    if (width > video->capture_width || height > video->capture_height) return ESP_ERR_NOT_SUPPORTED;
    if (xSemaphoreTake(video->sem, pdMS_TO_TICKS(1000)) != pdPASS) {
        return ESP_ERR_TIMEOUT;
    }
#if !CONFIG_EXAMPLE_SELECT_JPEG_HW_DRIVER
    if (width != video->width || height != video->height) {
        xSemaphoreGive(video->sem);
        return ESP_ERR_NOT_SUPPORTED;
    }
#endif
    portENTER_CRITICAL(&s_stats_lock);
    video->width = width;
    video->height = height;
    portEXIT_CRITICAL(&s_stats_lock);
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
    esp_err_t ret = ESP_OK;
    struct v4l2_buffer buf = {
        .type = V4L2_BUF_TYPE_VIDEO_CAPTURE, .memory = V4L2_MEMORY_MMAP,
    };
    bool dequeued = false;
    bool response_started = false;
    uint32_t image_size = 0;
    uint8_t *image = NULL;
    char timestamp[40];

    /* Keep geometry, V4L2 ownership and the shared JPEG buffer stable through send. */
    if (xSemaphoreTake(video->sem, pdMS_TO_TICKS(3000)) != pdPASS) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Camera busy");
    }
    bool frame_ready = false;
    for (int attempt = 0; attempt < 2 && !frame_ready; ++attempt) {
        video->frame_wait_start_us = esp_timer_get_time();
        if (ioctl(video->fd, VIDIOC_DQBUF, &buf) == 0) {
            frame_ready = true;
        } else {
            video->frame_wait_start_us = 0;
            ESP_LOGW(TAG, "video%d: DQBUF failed (errno=%d), restarting capture pipeline (attempt %d)",
                     video->index, errno, attempt + 1);
            ret = restart_camera_capture(video);
            if (ret != ESP_OK) break;
            memset(&buf, 0, sizeof(buf));
            buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            buf.memory = V4L2_MEMORY_MMAP;
        }
    }
    video->frame_wait_start_us = 0;
    if (!frame_ready) {
        ESP_LOGE(TAG, "video%d: no camera frame available for snapshot", video->index);
        ret = ESP_FAIL;
        goto cleanup;
    }
    dequeued = true;
    ESP_GOTO_ON_FALSE(buf.flags & V4L2_BUF_FLAG_DONE, ESP_ERR_INVALID_RESPONSE, cleanup, TAG, "incomplete frame");
    ESP_GOTO_ON_ERROR(pack_frame(video, &buf), cleanup, TAG, "failed to pack frame");

    image = video->buffer[buf.index];
    image_size = buf.bytesused;
    if (is_jpeg && video->pixel_format != V4L2_PIX_FMT_JPEG) {
        ESP_GOTO_ON_ERROR(example_encoder_process(video->encoder_handle, image, image_size,
                          video->jpeg_out_buf, video->jpeg_out_size, &image_size),
                          cleanup, TAG, "failed to encode video frame");
        image = video->jpeg_out_buf;
    }
    int64_t capture_us = (int64_t)buf.timestamp.tv_sec * 1000000 + buf.timestamp.tv_usec;
    if (capture_us <= 0) capture_us = esp_timer_get_time();
    snprintf(timestamp, sizeof(timestamp), "%" PRId64 ".%06" PRId64,
             capture_us / 1000000, capture_us % 1000000);
    ESP_GOTO_ON_ERROR(httpd_resp_set_hdr(req, "X-Timestamp", timestamp), cleanup, TAG, "failed to set timestamp");
    ESP_GOTO_ON_ERROR(httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*"), cleanup, TAG, "failed to set CORS header");
    ESP_GOTO_ON_ERROR(httpd_resp_set_hdr(req, "Cache-Control", "no-store"), cleanup, TAG, "failed to disable caching");
    response_started = true;
    ret = httpd_resp_send(req, (const char *)image, image_size);

cleanup:
    /* A dequeued buffer is returned exactly once, even when send or QBUF fails. */
    if (dequeued && ioctl(video->fd, VIDIOC_QBUF, &buf) != 0) {
        ESP_LOGE(TAG, "video%d: failed to queue snapshot buffer", video->index);
        ret = ESP_FAIL;
    }
    if (ret == ESP_OK) {
        portENTER_CRITICAL(&s_stats_lock);
        ++s_frames_captured;
        if (is_jpeg) s_last_jpeg = image_size;
        portEXIT_CRITICAL(&s_stats_lock);
    }
    xSemaphoreGive(video->sem);
    if (ret != ESP_OK && !response_started) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Camera capture failed");
    }
    /* httpd_resp_send already completes the response; never append a chunk/error. */
    return ret;
}

static char *get_cameras_json(web_cam_t *web_cam)
{
    cJSON *root = cJSON_CreateObject();
    cJSON *cameras = cJSON_CreateArray();
    if (!root || !cameras) {
        cJSON_Delete(root);
        cJSON_Delete(cameras);
        return NULL;
    }
    cJSON_AddItemToObject(root, "cameras", cameras);

    for (int i = 0; i < web_cam->video_count; i++) {
        char src_str[32];

        if (!is_valid_web_cam(&web_cam->video[i])) {
            continue;
        }

        portENTER_CRITICAL(&s_stats_lock);
        uint32_t width = web_cam->video[i].width;
        uint32_t height = web_cam->video[i].height;
        int quality = web_cam->video[i].jpeg_quality;
        bool quality_supported = web_cam->video[i].support_control_jpeg_quality;
        portEXIT_CRITICAL(&s_stats_lock);
        cJSON *camera = cJSON_CreateObject();
        cJSON_AddNumberToObject(camera, "index", i);
        assert(snprintf(src_str, sizeof(src_str), ":%d/stream", i + 81) > 0);
        cJSON_AddStringToObject(camera, "src", src_str);
        cJSON_AddNumberToObject(camera, "currentFrameRate",
                                (double)(int)(camera_service_get_fps() + 0.5f));

        int cur_fmt_id = (width == 640) ? 0 : 1;
        cJSON_AddNumberToObject(camera, "currentImageFormat", cur_fmt_id);
        assert(snprintf(src_str, sizeof(src_str), "JPEG %" PRIu32 "x%" PRIu32, width, height) > 0);
        cJSON_AddStringToObject(camera, "currentImageFormatDescription", src_str);

        if (quality_supported) {
            cJSON_AddNumberToObject(camera, "currentQuality", quality);
        }

        cJSON *current_resolution = cJSON_CreateObject();
        cJSON_AddNumberToObject(current_resolution, "width", width);
        cJSON_AddNumberToObject(current_resolution, "height", height);
        cJSON_AddItemToObject(camera, "currentResolution", current_resolution);

        cJSON *image_formats = cJSON_CreateArray();

        /* Level 0: 640x480 */
        cJSON *fmt0 = cJSON_CreateObject();
        cJSON_AddNumberToObject(fmt0, "id", 0);
        cJSON_AddStringToObject(fmt0, "description", "JPEG 640x480");
        if (quality_supported) {
            cJSON *q0 = cJSON_CreateObject();
            cJSON_AddNumberToObject(q0, "min", 1);
            cJSON_AddNumberToObject(q0, "max", 100);
            cJSON_AddNumberToObject(q0, "step", 1);
            cJSON_AddNumberToObject(q0, "default", 45);
            cJSON_AddItemToObject(fmt0, "quality", q0);
        }
        cJSON_AddItemToArray(image_formats, fmt0);

        /* Level 1: 800x600 */
        cJSON *fmt1 = cJSON_CreateObject();
        cJSON_AddNumberToObject(fmt1, "id", 1);
        cJSON_AddStringToObject(fmt1, "description", "JPEG 800x600");
        if (quality_supported) {
            cJSON *q1 = cJSON_CreateObject();
            cJSON_AddNumberToObject(q1, "min", 1);
            cJSON_AddNumberToObject(q1, "max", 100);
            cJSON_AddNumberToObject(q1, "step", 1);
            cJSON_AddNumberToObject(q1, "default", 45);
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

/* Caller owns video->sem, or is initializing an unpublished camera. */
static esp_err_t set_camera_jpeg_quality_locked(web_cam_video_t *video, int quality)
{
    if (!video || quality < 1 || quality > 100) return ESP_ERR_INVALID_ARG;
    if (video->pixel_format == V4L2_PIX_FMT_JPEG) {
        struct v4l2_query_ext_ctrl qctrl = { .id = V4L2_CID_JPEG_COMPRESSION_QUALITY };
        if (ioctl(video->fd, VIDIOC_QUERY_EXT_CTRL, &qctrl) != 0) return ESP_ERR_NOT_SUPPORTED;
        if (quality < qctrl.minimum || quality > qctrl.maximum || qctrl.step == 0 ||
            ((quality - qctrl.minimum) % qctrl.step) != 0) return ESP_ERR_INVALID_ARG;
        struct v4l2_ext_control control = { .id = V4L2_CID_JPEG_COMPRESSION_QUALITY, .value = quality };
        struct v4l2_ext_controls controls = {
            .ctrl_class = V4L2_CID_JPEG_CLASS, .count = 1, .controls = &control,
        };
        if (ioctl(video->fd, VIDIOC_S_EXT_CTRLS, &controls) != 0) return ESP_FAIL;
    } else {
        ESP_RETURN_ON_ERROR(example_encoder_set_jpeg_quality(video->encoder_handle, quality), TAG, "failed to set jpeg quality");
    }
    portENTER_CRITICAL(&s_stats_lock);
    video->jpeg_quality = quality;
    video->support_control_jpeg_quality = 1;
    portEXIT_CRITICAL(&s_stats_lock);
    ESP_LOGI(TAG, "video%d: set jpeg quality %d success", video->index, quality);
    return ESP_OK;
}

static esp_err_t set_camera_jpeg_quality(web_cam_video_t *video, int quality)
{
    if (!video || quality < 1 || quality > 100) return ESP_ERR_INVALID_ARG;
    if (video->fd < 0 || !video->sem) return ESP_ERR_INVALID_STATE;
    if (xSemaphoreTake(video->sem, pdMS_TO_TICKS(1000)) != pdPASS) return ESP_ERR_TIMEOUT;
    esp_err_t ret = set_camera_jpeg_quality_locked(video, quality);
    xSemaphoreGive(video->sem);
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

    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    if (!output) return httpd_resp_send_500(req);
    httpd_resp_set_type(req, "application/json");
    ret = httpd_resp_sendstr(req, output);
    free(output);

    return ret;
}

static esp_err_t receive_http_body(httpd_req_t *req, char *body, size_t capacity)
{
    if (req->content_len == 0 || req->content_len >= capacity) return ESP_ERR_INVALID_SIZE;
    size_t offset = 0;
    int64_t deadline = esp_timer_get_time() + CAMERA_HTTP_BODY_TIMEOUT_US;
    while (offset < req->content_len) {
        if (esp_timer_get_time() >= deadline) return ESP_ERR_TIMEOUT;
        int received = httpd_req_recv(req, body + offset, req->content_len - offset);
        /* Do not retry socket timeouts indefinitely. */
        if (received == HTTPD_SOCK_ERR_TIMEOUT) return ESP_ERR_TIMEOUT;
        if (received <= 0) return ESP_FAIL;
        offset += received;
    }
    body[offset] = '\0';
    if (memchr(body, '\0', offset)) return ESP_ERR_INVALID_ARG;
    return ESP_OK;
}

static cJSON *parse_http_json(const char *body)
{
    /* cJSON strings have no length: reject decoded NULs rather than truncate credentials. */
    for (const char *p = body; *p; ++p) {
        if (*p == '\\' && p[1]) {
            if (p[1] == 'u' && strncmp(p + 2, "0000", 4) == 0) return NULL;
            ++p;
        }
    }
    cJSON *root = cJSON_ParseWithOpts(body, NULL, true);
    if (!cJSON_IsObject(root)) {
        cJSON_Delete(root);
        return NULL;
    }
    return root;
}

static bool json_integer(const cJSON *item, int minimum, int maximum)
{
    return cJSON_IsNumber(item) && item->valuedouble == item->valueint &&
           item->valueint >= minimum && item->valueint <= maximum;
}

static bool json_integer_aliases(const cJSON *root, const char *const *names, size_t count,
                                 int minimum, int maximum, bool *present, int *value)
{
    *present = false;
    for (size_t i = 0; i < count; ++i) {
        const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, names[i]);
        if (!item) continue;
        if (!json_integer(item, minimum, maximum) || (*present && *value != item->valueint)) return false;
        *present = true;
        *value = item->valueint;
    }
    return true;
}

static esp_err_t camera_settings_handler(httpd_req_t *req)
{
    char content[CAMERA_HTTP_BODY_SIZE];
    web_cam_t *web_cam = (web_cam_t *)req->user_ctx;
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    esp_err_t ret = receive_http_body(req, content, sizeof(content));
    if (ret != ESP_OK) {
        httpd_resp_set_hdr(req, "Connection", "close");
        httpd_resp_send_err(req, ret == ESP_ERR_TIMEOUT ? HTTPD_408_REQ_TIMEOUT : HTTPD_400_BAD_REQUEST,
                            "Invalid or incomplete request body (maximum 511 bytes)");
        return ESP_FAIL; /* Close any connection with an unread request body. */
    }
    cJSON *root = parse_http_json(content);
    if (!root) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON object");

    int index = 0, format = 0, quality = 0;
    bool has_format = false, has_quality = false;
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, "index");
    bool valid_index = !item || json_integer(item, 0, web_cam->video_count - 1);
    if (item && valid_index) index = item->valueint;
    const char *const format_names[] = { "image_format", "currentImageFormat" };
    const char *const quality_names[] = { "quality", "jpeg_quality", "currentQuality" };
    bool valid = valid_index && index < web_cam->video_count && is_valid_web_cam(&web_cam->video[index]) &&
                 json_integer_aliases(root, format_names, 2, 0, 1, &has_format, &format) &&
                 json_integer_aliases(root, quality_names, 3, 1, 100, &has_quality, &quality) &&
                 (has_format || has_quality);
    cJSON_Delete(root);
    if (!valid) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid camera index, format or quality");

    if (has_quality) ret = set_camera_jpeg_quality(&web_cam->video[index], quality);
    if (ret == ESP_OK && has_format) {
        ret = set_camera_resolution(&web_cam->video[index], format == 0 ? 640 : 800, format == 0 ? 480 : 600);
    }
    if (ret != ESP_OK) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Camera settings failed");
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"status\":\"ok\"}");
}

static esp_err_t wifi_scan_handler(httpd_req_t *req)
{
    char query[64] = {0};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        char val[8] = {0};
        if (httpd_query_key_value(query, "start", val, sizeof(val)) == ESP_OK &&
            strcmp(val, "1") == 0) {
            app_mywifi_scan_trigger();
        }
    }
    char *json = app_mywifi_scan_json();
    if (!json) json = strdup("[]");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "X-Scan-Running", app_mywifi_scan_running() ? "1" : "0");
    esp_err_t ret = httpd_resp_send(req, json, strlen(json));
    free(json);
    return ret;
}

static esp_err_t wifi_submit_handler(httpd_req_t *req)
{
    char body[CAMERA_HTTP_BODY_SIZE];
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    esp_err_t ret = receive_http_body(req, body, sizeof(body));
    if (ret != ESP_OK) {
        httpd_resp_set_hdr(req, "Connection", "close");
        const char *error = ret == ESP_ERR_TIMEOUT ?
            "{\"success\":false,\"error\":\"Request body timeout\"}" :
            "{\"success\":false,\"error\":\"Invalid or incomplete body (maximum 511 bytes)\"}";
        httpd_resp_sendstr(req, error);
        return ESP_FAIL; /* Send HTTP 200 JSON, then close an unread/invalid body. */
    }
    cJSON *root = parse_http_json(body);
    if (!root) return httpd_resp_sendstr(req, "{\"success\":false,\"error\":\"JSON parse error\"}");
    const cJSON *ssid = cJSON_GetObjectItemCaseSensitive(root, "ssid");
    const cJSON *password = cJSON_GetObjectItemCaseSensitive(root, "password");
    const cJSON *use_saved = cJSON_GetObjectItemCaseSensitive(root, "use_saved");
    const cJSON *bssid_item = cJSON_GetObjectItemCaseSensitive(root, "bssid");
    const cJSON *channel_item = cJSON_GetObjectItemCaseSensitive(root, "channel");
    uint8_t bssid[6] = { 0 };
    uint8_t channel = 0;
    bool has_bssid = false;
    bool bssid_valid = true;
    if (bssid_item) {
        unsigned int b[6];
        bssid_valid = false;
        if (cJSON_IsString(bssid_item) && bssid_item->valuestring &&
            strlen(bssid_item->valuestring) == 17 &&
            sscanf(bssid_item->valuestring, "%2x:%2x:%2x:%2x:%2x:%2x",
                   &b[0], &b[1], &b[2], &b[3], &b[4], &b[5]) == 6 &&
            cJSON_IsNumber(channel_item)) {
            int ch = channel_item->valueint;
            if (ch > 0 && ch <= 177) {
                for (int i = 0; i < 6; ++i) bssid[i] = (uint8_t)b[i];
                channel = (uint8_t)ch;
                has_bssid = true;
                bssid_valid = true;
            }
        }
    }
    const char *response = NULL;
    if (!bssid_valid) {
        response = "{\"success\":false,\"error\":\"Invalid bssid/channel\"}";
    } else if (cJSON_IsTrue(use_saved)) {
        char saved_ssid[33] = { 0 };
        char saved_passwd[65] = { 0 };
        bool ok = app_mywifi_load_sta_creds(saved_ssid, sizeof(saved_ssid),
                                            saved_passwd, sizeof(saved_passwd)) == ESP_OK &&
                  saved_ssid[0] != '\0';
        if (ok && cJSON_IsString(ssid) && ssid->valuestring && ssid->valuestring[0] &&
            strcmp(ssid->valuestring, saved_ssid) != 0) {
            ok = false;
        }
        if (!ok) {
            response = "{\"success\":false,\"error\":\"No matching saved Wi-Fi credentials\"}";
        } else {
            ret = has_bssid ?
                  app_mywifi_connect_sta_ex(saved_ssid, saved_passwd, bssid, channel) :
                  app_mywifi_connect_sta(saved_ssid, saved_passwd);
            response = ret == ESP_OK ? "{\"success\":true,\"state\":\"connecting\",\"saved\":true}" :
                                      "{\"success\":false,\"error\":\"Failed to start Wi-Fi connection\"}";
        }
        memset(saved_passwd, 0, sizeof(saved_passwd));
    } else {
        const char *passwd = cJSON_IsString(password) ? password->valuestring : "";
        if (!cJSON_IsString(ssid) || !ssid->valuestring || ssid->valuestring[0] == '\0' ||
            strlen(ssid->valuestring) > 32) {
            response = "{\"success\":false,\"error\":\"SSID must be a nonempty string of at most 32 bytes\"}";
        } else if ((password && !cJSON_IsString(password)) || strlen(passwd) > 64) {
            response = "{\"success\":false,\"error\":\"Password must be a string of at most 64 bytes (empty for open networks)\"}";
        } else {
            /* Wi-Fi owns persistence: credentials are saved only after GOT_IP. */
            ret = has_bssid ?
                  app_mywifi_connect_sta_ex(ssid->valuestring, passwd, bssid, channel) :
                  app_mywifi_connect_sta(ssid->valuestring, passwd);
            response = ret == ESP_OK ? "{\"success\":true,\"state\":\"connecting\"}" :
                                      "{\"success\":false,\"error\":\"Failed to start Wi-Fi connection\"}";
        }
    }
    cJSON_Delete(root);
    return httpd_resp_sendstr(req, response);
}

static esp_err_t wifi_status_handler(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();
    bool connected = app_mywifi_is_sta_connected();
    bool connecting = app_mywifi_is_sta_connecting();
    int reason = app_mywifi_get_disconnect_reason();
    const char *ip = app_mywifi_get_sta_ip();
    bool has_ip = ip && ip[0] != '\0';

    /* Never report "connected" without an IP: the UI would show an empty STA
     * address and stop polling before DHCP completes. */
    const char *state = "disconnected";
    if (connected && has_ip) {
        state = "connected";
    } else if (connecting || connected) {
        state = "connecting";
    } else if (reason > 0) {
        state = "failed";
    }

    cJSON_AddStringToObject(root, "state", state);
    cJSON_AddBoolToObject(root, "connected", connected);
    cJSON_AddBoolToObject(root, "connecting", connecting);
    cJSON_AddStringToObject(root, "ip", has_ip ? ip : "");
    cJSON_AddStringToObject(root, "ssid", app_mywifi_get_sta_ssid());
    cJSON_AddNumberToObject(root, "reason", reason);
    cJSON_AddStringToObject(root, "message", app_mywifi_get_disconnect_message());
    cJSON_AddBoolToObject(root, "ap_active", app_mywifi_is_ap_active());
    cJSON_AddStringToObject(root, "ap_ip", app_mywifi_get_ap_ip());
    cJSON_AddStringToObject(root, "ap_ssid", app_mywifi_get_ap_ssid());
    if (connected) {
        uint8_t sta_channel = 0;
        wifi_second_chan_t second = WIFI_SECOND_CHAN_NONE;
        if (esp_wifi_get_channel(&sta_channel, &second) == ESP_OK && sta_channel > 0) {
            cJSON_AddNumberToObject(root, "sta_channel", sta_channel);
            cJSON_AddStringToObject(root, "sta_band", sta_channel > 14 ? "5G" : "2.4G");
        }
    }

    char *json_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    esp_err_t ret = httpd_resp_send(req, json_str, strlen(json_str));
    free(json_str);
    return ret;
}

static esp_err_t wifi_prepare_handler(httpd_req_t *req)
{
    char body[CAMERA_HTTP_BODY_SIZE];
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    esp_err_t ret = receive_http_body(req, body, sizeof(body));
    if (ret != ESP_OK) {
        httpd_resp_set_hdr(req, "Connection", "close");
        return httpd_resp_sendstr(req, "{\"success\":false,\"error\":\"Invalid body\"}");
    }
    cJSON *root = parse_http_json(body);
    if (!root) return httpd_resp_sendstr(req, "{\"success\":false,\"error\":\"JSON parse error\"}");

    const cJSON *channel = cJSON_GetObjectItemCaseSensitive(root, "channel");
    esp_err_t err = ESP_ERR_INVALID_ARG;
    if (cJSON_IsNumber(channel)) {
        err = app_mywifi_prepare_ap_channel((uint8_t)channel->valueint);
    }
    cJSON_Delete(root);
    return httpd_resp_sendstr(req, err == ESP_OK ? "{\"success\":true}" : "{\"success\":false}");
}

static esp_err_t wifi_saved_handler(httpd_req_t *req)
{
    char ssid[33] = { 0 };
    char passwd[65] = { 0 };
    bool saved = app_mywifi_load_sta_creds(ssid, sizeof(ssid), passwd, sizeof(passwd)) == ESP_OK &&
                 ssid[0] != '\0';
    bool has_password = passwd[0] != '\0';
    memset(passwd, 0, sizeof(passwd)); /* Never expose the stored password. */

    cJSON *root = cJSON_CreateObject();
    if (!root) return ESP_ERR_NO_MEM;
    cJSON_AddBoolToObject(root, "saved", saved);
    if (saved) {
        cJSON_AddStringToObject(root, "ssid", ssid);
        cJSON_AddBoolToObject(root, "has_password", has_password);
    }
    char *json_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json_str) return ESP_ERR_NO_MEM;

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    esp_err_t ret = httpd_resp_send(req, json_str, strlen(json_str));
    free(json_str);
    return ret;
}

static esp_err_t stream_redirect_handler(httpd_req_t *req)
{
    char redirect_url[96];
    char host_hdr[48];
    snprintf(host_hdr, sizeof(host_hdr), "%s", app_mywifi_get_ap_ip());
    if (httpd_req_get_hdr_value_str(req, "Host", host_hdr, sizeof(host_hdr)) != ESP_OK || host_hdr[0] == '\0') {
        snprintf(host_hdr, sizeof(host_hdr), "%s", app_mywifi_get_ap_ip());
    }
    char *colon = strchr(host_hdr, ':');
    if (colon) {
        *colon = '\0';
    }
    snprintf(redirect_url, sizeof(redirect_url), "http://%s:81/stream", host_hdr);
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", redirect_url);
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    return httpd_resp_send(req, NULL, 0);
}

static esp_err_t camera_control_handler(httpd_req_t *req)
{
    char buf[128] = {0};
    char variable[32] = {0};
    char value[32] = {0};
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    if (httpd_req_get_url_query_str(req, buf, sizeof(buf)) != ESP_OK ||
        httpd_query_key_value(buf, "var", variable, sizeof(variable)) != ESP_OK ||
        httpd_query_key_value(buf, "val", value, sizeof(value)) != ESP_OK) {
        return httpd_resp_send_404(req);
    }
    char *end = NULL;
    errno = 0;
    long parsed = strtol(value, &end, 10);
    if (errno == ERANGE || end == value || *end != '\0' || parsed < INT_MIN || parsed > INT_MAX) {
        return httpd_resp_send_500(req);
    }
    int val = (int)parsed;
    esp_err_t ret = ESP_ERR_NOT_SUPPORTED;
    if (strcmp(variable, "hmirror") == 0) {
        ret = camera_service_set_hflip(val);
    } else if (strcmp(variable, "vflip") == 0) {
        ret = camera_service_set_vflip(val);
    } else if (strcmp(variable, "quality") == 0) {
        ret = camera_service_set_quality(val);
    } else if (strcmp(variable, "framesize") == 0) {
        if (val == S3_FRAMESIZE_VGA || val == 640) {
            ret = camera_service_set_resolution(640, 480);
        } else if (val == S3_FRAMESIZE_SVGA || val == 800) {
            ret = camera_service_set_resolution(800, 600);
        }
    }
    if (ret != ESP_OK) return httpd_resp_send_500(req);
    return httpd_resp_send(req, NULL, 0);
}

static esp_err_t camera_status_handler(httpd_req_t *req)
{
    char json_response[1536];
    uint32_t w = 640, h = 480;
    int quality = 38;
    wifi_ap_record_t ap_info;
    int sta_rssi = esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK ? ap_info.rssi : 0;
    portENTER_CRITICAL(&s_stats_lock);
    if (s_web_cam && s_web_cam->video_count > 0) {
        w = s_web_cam->video[0].width;
        h = s_web_cam->video[0].height;
        quality = s_web_cam->video[0].jpeg_quality;
    }
    int hmirror = s_cur_hflip, vflip = s_cur_vflip;
    uint32_t clients = s_stream_clients, stream_frames = s_stream_frames;
    uint32_t captured = s_frames_captured, last_jpeg = s_last_jpeg;
    float fps = s_stream_fps;
    int64_t last_sent = s_last_sent;
    portEXIT_CRITICAL(&s_stats_lock);
    if (esp_timer_get_time() - last_sent > 2000000) fps = 0;
    int framesize = (w == 800 && h == 600) ? S3_FRAMESIZE_SVGA : S3_FRAMESIZE_VGA;

    /* S3 compatibility fields unsupported by this P4 service are explicitly zero. */
    int length = snprintf(json_response, sizeof(json_response),
             "{\"board\":\"ESP32-P4-CAM\",\"width\":%" PRIu32 ",\"height\":%" PRIu32
             ",\"xclk\":0,\"pixformat\":%d,\"framesize\":%d,\"quality\":%d,"
             "\"brightness\":0,\"contrast\":0,\"saturation\":0,\"sharpness\":0,"
             "\"special_effect\":0,\"wb_mode\":0,\"awb\":0,\"awb_gain\":0,\"aec\":0,\"aec2\":0,"
             "\"ae_level\":0,\"aec_value\":0,\"agc\":0,\"agc_gain\":0,\"gainceiling\":0,"
             "\"bpc\":0,\"wpc\":0,\"raw_gma\":0,\"lenc\":0,\"hmirror\":%d,\"vflip\":%d,"
             "\"dcw\":0,\"colorbar\":0,\"led_intensity\":-1,\"face_detect\":0,\"face_enroll\":0,\"face_recognize\":0,"
             "\"0xd3\":0,\"0x111\":0,\"0x132\":0,\"fps\":%.1f,\"stream_clients\":%" PRIu32
             ",\"stream_frames\":%" PRIu32 ",\"frames_captured\":%" PRIu32 ",\"last_jpeg\":%" PRIu32 ",\"sta_rssi\":%d}",
             w, h, S3_PIXFORMAT_RGB565, framesize, quality, hmirror, vflip, fps,
             clients, stream_frames, captured, last_jpeg, sta_rssi);

    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    if (length < 0 || (size_t)length >= sizeof(json_response)) return httpd_resp_send_500(req);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, json_response, length);
}

static esp_err_t wifi_reset_handler(httpd_req_t *req)
{
    app_mywifi_clear_sta_creds();
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    const char *resp = "{\"success\":true,\"message\":\"Wi-Fi credentials cleared\"}";
    return httpd_resp_send(req, resp, strlen(resp));
}

static esp_err_t static_file_handler(httpd_req_t *req)
{
    char uri[128];
    size_t uri_len = strcspn(req->uri, "?");
    if (uri_len >= sizeof(uri)) return httpd_resp_send_404(req);
    memcpy(uri, req->uri, uri_len);
    uri[uri_len] = '\0';

    if (strcmp(uri, "/") == 0 || strcmp(uri, "/index.html") == 0) {
        char host[64] = {0};
        bool provisioning_host = false;
        if (httpd_req_get_hdr_value_str(req, "Host", host, sizeof(host)) == ESP_OK) {
            char *colon = strchr(host, ':');
            if (colon) *colon = '\0';
            provisioning_host = strcmp(host, app_mywifi_get_ap_ip()) == 0;
        }
        httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
        if (provisioning_host || !app_mywifi_is_sta_connected()) {
            httpd_resp_set_status(req, "302 Found");
            httpd_resp_set_hdr(req, "Location", "/wifi");
            return httpd_resp_send(req, NULL, 0);
        }
        httpd_resp_set_type(req, "text/html");
        httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
        return httpd_resp_send(req, (const char *)index_html_gz_start, index_html_gz_end - index_html_gz_start);
    } else if (strcmp(uri, "/wifi") == 0 || strcmp(uri, "/wifi/") == 0) {
        httpd_resp_set_type(req, "text/html; charset=utf-8");
        httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
        return httpd_resp_send(req, wifi_config_html, strlen(wifi_config_html));
    } else if (strcmp(uri, "/cam") == 0 || strcmp(uri, "/cam/") == 0 ||
               strcmp(uri, "/vue") == 0 || strcmp(uri, "/vue/") == 0) {
        httpd_resp_set_type(req, "text/html");
        httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
        httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
        return httpd_resp_send(req, (const char *)index_html_gz_start, index_html_gz_end - index_html_gz_start);
    } else if (strcmp(uri, "/loading.jpg") == 0) {
        httpd_resp_set_type(req, "image/jpeg");
        httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
        httpd_resp_set_hdr(req, "Cache-Control", "public, max-age=86400");
        return httpd_resp_send(req, (const char *)loading_jpg_gz_start, loading_jpg_gz_end - loading_jpg_gz_start);
    } else if (strcmp(uri, "/favicon.ico") == 0) {
        httpd_resp_set_type(req, "image/x-icon");
        httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
        httpd_resp_set_hdr(req, "Cache-Control", "public, max-age=86400");
        return httpd_resp_send(req, (const char *)favicon_ico_gz_start, favicon_ico_gz_end - favicon_ico_gz_start);
    } else if (strcmp(uri, "/assets/index.js") == 0) {
        httpd_resp_set_type(req, "application/javascript");
        httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
        httpd_resp_set_hdr(req, "Cache-Control", "public, max-age=300");
        httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
        return httpd_resp_send(req, (const char *)assets_index_js_gz_start, assets_index_js_gz_end - assets_index_js_gz_start);
    } else if (strcmp(uri, "/assets/index.css") == 0) {
        httpd_resp_set_type(req, "text/css");
        httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
        httpd_resp_set_hdr(req, "Cache-Control", "public, max-age=300");
        httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
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

static esp_err_t image_stream_loop(httpd_req_t *req)
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
        int64_t t0 = esp_timer_get_time();
        video->frame_wait_start_us = t0;
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

        int64_t t1 = esp_timer_get_time();
        ESP_GOTO_ON_ERROR(pack_frame(video, &buf), fail0, TAG, "failed to pack frame");
        int64_t t2 = esp_timer_get_time();

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
        int64_t t3 = esp_timer_get_time();

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
        int64_t t4 = esp_timer_get_time();
        last_frame_time = t4;
        vTaskDelay(1);

        if (t4 - t0 > 80000) {
            ESP_LOGW(TAG, "SLOW FRAME: total=%" PRId64 "ms (dqbuf=%" PRId64 "ms, pack=%" PRId64 "ms, enc=%" PRId64 "ms, send=%" PRId64 "ms)",
                     (t4 - t0) / 1000, (t1 - t0) / 1000, (t2 - t1) / 1000, (t3 - t2) / 1000, (t4 - t3) / 1000);
        }
        int64_t now = esp_timer_get_time();
        ++sent_frames;
        portENTER_CRITICAL(&s_stats_lock);
        ++s_stream_frames;
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

static esp_err_t image_stream_handler(httpd_req_t *req)
{
    portENTER_CRITICAL(&s_stats_lock);
    ++s_stream_clients;
    portEXIT_CRITICAL(&s_stats_lock);
    esp_err_t ret = image_stream_loop(req);
    portENTER_CRITICAL(&s_stats_lock);
    if (s_stream_clients > 0) --s_stream_clients;
    portEXIT_CRITICAL(&s_stats_lock);
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

    char json[320];
    snprintf(json, sizeof(json),
             "{\"title\":\"Image Transmission STA Mode(图像回传STA模式)\",\"type\":\"image_transmission\","
             "\"enabled\":false,\"objects\":[],\"width\":%" PRIu32 ",\"height\":%" PRIu32 ",\"tags\":[],\"video_fps\":%.1f}",
             w, h, fps);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_sendstr(req, json);
}

static esp_err_t tests_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_sendstr(req, "{\"status\":\"ok\"}");
}

static esp_err_t capture_image_handler(httpd_req_t *req)
{
    web_cam_t *web_cam = (web_cam_t *)req->user_ctx;

    request_desc_t desc;
    ESP_RETURN_ON_ERROR(decode_request(web_cam, req, &desc), TAG, "failed to decode request");

    ESP_RETURN_ON_ERROR(httpd_resp_set_type(req, "image/jpeg"), TAG, "failed to set content type");
    char disposition[64];
    const char *filename = (strcmp(req->uri, "/capture") == 0) ? "capture.jpg" : "camera_0_image.jpg";
    ESP_RETURN_ON_FALSE(snprintf(disposition, sizeof(disposition), "inline; filename=%s", filename) > 0,
                        ESP_FAIL, TAG, "failed to format disposition");
    ESP_RETURN_ON_ERROR(httpd_resp_set_hdr(req, "Content-Disposition", disposition), TAG,
                        "failed to set content disposition");

    return capture_video_image(req, &web_cam->video[desc.index], true);
}

static esp_err_t capture_binary_handler(httpd_req_t *req)
{
    web_cam_t *web_cam = (web_cam_t *)req->user_ctx;

    request_desc_t desc;
    ESP_RETURN_ON_ERROR(decode_request(web_cam, req, &desc), TAG, "failed to decode request");

    ESP_RETURN_ON_ERROR(httpd_resp_set_type(req, "application/octet-stream"), TAG, "failed to set content type");
    ESP_RETURN_ON_ERROR(httpd_resp_set_hdr(req, "Content-Disposition", "inline; filename=camera_raw.bin"), TAG,
                        "failed to set content disposition");

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
        ESP_GOTO_ON_ERROR(set_camera_jpeg_quality_locked(video, EXAMPLE_JPEG_ENC_QUALITY), fail0, TAG, "failed to set jpeg quality");
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
    config.max_uri_handlers = 32;
    config.uri_match_fn = httpd_uri_match_wildcard;
    config.lru_purge_enable = true;

    /* Wi-Fi provisioning URI handlers */
    httpd_uri_t wifi_scan_uri = {
        .uri = "/scan",
        .method = HTTP_GET,
        .handler = wifi_scan_handler,
        .user_ctx = NULL
    };

    httpd_uri_t wifi_submit_uri = {
        .uri = "/submit",
        .method = HTTP_POST,
        .handler = wifi_submit_handler,
        .user_ctx = NULL
    };

    httpd_uri_t wifi_connect_uri = {
        .uri = "/connect",
        .method = HTTP_POST,
        .handler = wifi_submit_handler,
        .user_ctx = NULL
    };

    httpd_uri_t wifi_status_uri = {
        .uri = "/connect/status",
        .method = HTTP_GET,
        .handler = wifi_status_handler,
        .user_ctx = NULL
    };

    httpd_uri_t wifi_saved_uri = {
        .uri = "/api/wifi/saved",
        .method = HTTP_GET,
        .handler = wifi_saved_handler,
        .user_ctx = NULL
    };

    httpd_uri_t wifi_prepare_uri = {
        .uri = "/api/wifi/prepare",
        .method = HTTP_POST,
        .handler = wifi_prepare_handler,
        .user_ctx = NULL
    };

    httpd_uri_t wifi_reset_uri = {
        .uri = "/api/wifi/reset",
        .method = HTTP_GET,
        .handler = wifi_reset_handler,
        .user_ctx = NULL
    };

    httpd_uri_t wifi_reset_post_uri = {
        .uri = "/api/wifi/reset",
        .method = HTTP_POST,
        .handler = wifi_reset_handler,
        .user_ctx = NULL
    };

    /* Port 80 /stream redirect handler -> redirects to port 81 */
    httpd_uri_t stream_redirect_uri = {
        .uri = "/stream",
        .method = HTTP_GET,
        .handler = stream_redirect_handler,
        .user_ctx = NULL
    };

    /* Standard S3 /control and /status compatibility endpoints */
    httpd_uri_t control_uri = {
        .uri = "/control",
        .method = HTTP_GET,
        .handler = camera_control_handler,
        .user_ctx = (void *)web_cam
    };

    httpd_uri_t status_uri = {
        .uri = "/status",
        .method = HTTP_GET,
        .handler = camera_status_handler,
        .user_ctx = (void *)web_cam
    };

    /* Traditional snapshot routes */
    httpd_uri_t capture_uri = {
        .uri = "/capture",
        .method = HTTP_GET,
        .handler = capture_image_handler,
        .user_ctx = (void *)web_cam
    };

    httpd_uri_t frame_jpg_uri = {
        .uri = "/api/frame.jpg",
        .method = HTTP_GET,
        .handler = capture_image_handler,
        .user_ctx = (void *)web_cam
    };

    /* Unified static file handler for all static resources (registered last) */
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

    httpd_uri_t resolution_uri = {
        .uri = "/api/resolution",
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
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(stream_httpd, &capture_uri), TAG,
                        "failed to register capture handler");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(stream_httpd, &frame_jpg_uri), TAG,
                        "failed to register frame.jpg handler");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(stream_httpd, &capture_binary_uri), TAG,
                        "failed to register capture binary handler");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(stream_httpd, &camera_info_uri), TAG,
                        "failed to register camera info handler");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(stream_httpd, &camera_settings_uri), TAG,
                        "failed to register camera settings handler");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(stream_httpd, &resolution_uri), TAG,
                        "failed to register resolution handler");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(stream_httpd, &control_uri), TAG,
                        "failed to register control handler");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(stream_httpd, &status_uri), TAG,
                        "failed to register status handler");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(stream_httpd, &stream_redirect_uri), TAG,
                        "failed to register stream redirect handler");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(stream_httpd, &detections_uri), TAG,
                        "failed to register detections handler");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(stream_httpd, &tests_uri), TAG,
                        "failed to register tests handler");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(stream_httpd, &wifi_scan_uri), TAG,
                        "failed to register wifi scan handler");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(stream_httpd, &wifi_submit_uri), TAG,
                        "failed to register wifi submit handler");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(stream_httpd, &wifi_connect_uri), TAG,
                        "failed to register wifi connect handler");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(stream_httpd, &wifi_status_uri), TAG,
                        "failed to register wifi status handler");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(stream_httpd, &wifi_saved_uri), TAG,
                        "failed to register wifi saved handler");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(stream_httpd, &wifi_prepare_uri), TAG,
                        "failed to register wifi prepare handler");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(stream_httpd, &wifi_reset_uri), TAG,
                        "failed to register wifi reset GET handler");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(stream_httpd, &wifi_reset_post_uri), TAG,
                        "failed to register wifi reset POST handler");
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
    ESP_LOGI(TAG, "Camera capture started (pure image transmission mode)");
    ESP_LOGI(TAG, "Camera capture started before network initialization");

    return ESP_OK;
}

esp_err_t camera_service_start(void)
{
    static web_cam_t empty_camera;
    return http_server_init(s_web_cam ? s_web_cam : &empty_camera);
}
