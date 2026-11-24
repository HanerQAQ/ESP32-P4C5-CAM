#include "detection_service.h"
#include "hardware_test.h"

#include <math.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <sys/param.h>
#include "apriltag.h"
#include "common/homography.h"
#include "common/image_u8.h"
#include "common/matd.h"
#include "common/zarray.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "tag36h11.h"

#define MAX_TAGS 16
#define RAD_TO_DEG (180.0f / (float)M_PI)

#if configUSE_PREEMPTION != 1 || configUSE_TIME_SLICING != 1
#error "AprilTag idle-priority worker requires preemptive time slicing"
#endif

typedef struct {
    int id;
    float p[4][2];
    float center[2];
    float rotation;
    float translation[3];
    float rotation_xyz[3];
} web_tag_t;

static const char *TAG = "apriltag_web";
static apriltag_family_t *s_family;
static apriltag_detector_t *s_detector;
static uint8_t *s_gray;
static uint32_t s_width;
static uint32_t s_height;
static uint32_t s_detect_width, s_detect_height;
static portMUX_TYPE s_request_lock = portMUX_INITIALIZER_UNLOCKED;
static bool s_processing;
static bool s_enabled;
static TaskHandle_t s_worker;
static uint32_t s_frame_generation;
static uint32_t s_mode_generation, s_result_generation;

void detection_service_toggle_mode(void)
{
    portENTER_CRITICAL(&s_request_lock);
    s_enabled = !s_enabled;
    ++s_mode_generation;
    portEXIT_CRITICAL(&s_request_lock);
}
static float s_detection_ms;
static int64_t s_result_time;
static void detect_frame(uint32_t generation);

static uint32_t s_sequence;
static web_tag_t s_tags[MAX_TAGS];
static size_t s_tag_count;
static SemaphoreHandle_t s_mutex;

static void detection_worker(void *arg)
{
    while (true) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        portENTER_CRITICAL(&s_request_lock);
        uint32_t generation = s_frame_generation;
        bool run = s_enabled && generation == s_mode_generation;
        portEXIT_CRITICAL(&s_request_lock);
        if (run) detect_frame(generation);
        portENTER_CRITICAL(&s_request_lock);
        s_processing = false;
        portEXIT_CRITICAL(&s_request_lock);
        /* Give IDLE1 and other CPU1 work a scheduling opportunity. */
        vTaskDelay(1);
    }
}

esp_err_t detection_service_init(uint32_t width, uint32_t height)
{
    if (!width || !height) {
        return ESP_ERR_INVALID_ARG;
    }
    s_width = width;
    s_height = height;
    /* 4x downsample on each axis; keep the full field of view. */
    s_detect_width = width / 4;
    s_detect_height = height / 4;
    if (!s_detect_width || !s_detect_height) return ESP_ERR_INVALID_ARG;
    /* Hosted SDIO TX requires internal DMA RAM. Keep this large CPU-only
     * image in PSRAM even when an internal allocation could succeed. */
    s_gray = heap_caps_malloc(s_detect_width * s_detect_height, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_gray) {
        return ESP_ERR_NO_MEM;
    }
    s_mutex = xSemaphoreCreateMutex();
    if (!s_mutex) {
        return ESP_ERR_NO_MEM;
    }
    s_family = tag36h11_create();
    s_detector = apriltag_detector_create();
    if (!s_family || !s_detector) {
        return ESP_ERR_NO_MEM;
    }
    apriltag_detector_add_family_bits(s_detector, s_family, 1);
    s_detector->quad_decimate = 1.0f;
    s_detector->quad_sigma = 0.0f;
    s_detector->nthreads = 1;
    s_detector->debug = 0;
    s_detector->refine_edges = 1;
    if (xTaskCreatePinnedToCore(detection_worker, "apriltag", 32768, NULL,
                               tskIDLE_PRIORITY, &s_worker, 1) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "tag36h11 CPU1 worker: %" PRIu32 "x%" PRIu32 " (camera %" PRIu32 "x%" PRIu32 ")",
             s_detect_width, s_detect_height, width, height);
    ESP_LOGI(TAG, "Gray buffer in PSRAM: %" PRIu32 " bytes; internal DMA free=%u, largest=%u",
             s_detect_width * s_detect_height,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_8BIT));
    return ESP_OK;
}

void detection_service_process_rgb565(const uint8_t *frame, size_t frame_size)
{
    if (!s_worker || !frame || frame_size < s_width * s_height * 2) {
        return;
    }

    portENTER_CRITICAL(&s_request_lock);
    bool enabled = s_enabled;
    uint32_t generation = s_mode_generation;
    bool run = enabled && !s_processing;
    if (run) s_processing = true;
    portEXIT_CRITICAL(&s_request_lock);
    if (!run) {
        return;
    }

    /* Reserve the single PSRAM buffer until the worker finishes. Busy frames
     * are dropped, never queued. Copy/downsample before returning the camera
     * buffer to its driver; the worker never holds a camera buffer pointer. */
    for (uint32_t y = 0; y < s_detect_height; ++y) {
        for (uint32_t x = 0; x < s_detect_width; ++x) {
            uint32_t sum = 0;
            for (uint32_t dy = 0; dy < 4; ++dy) {
                const uint16_t *row = (const uint16_t *)frame + (y * 4 + dy) * s_width;
                for (uint32_t dx = 0; dx < 4; ++dx) {
                    uint16_t pixel = row[x * 4 + dx];
                    uint32_t r = (pixel >> 11) & 31, g = (pixel >> 5) & 63, b = pixel & 31;
                    sum += (r * 634 + g * 607 + b * 239) >> 8;
                }
            }
            s_gray[y * s_detect_width + x] = sum / 16;
        }
    }
    portENTER_CRITICAL(&s_request_lock);
    s_frame_generation = generation;
    portEXIT_CRITICAL(&s_request_lock);
    xTaskNotifyGive(s_worker);
}

static void detect_frame(uint32_t generation)
{
    int64_t start = esp_timer_get_time();
    ESP_LOGD(TAG, "Continuous detection started on CPU%d, priority=%u",
             xPortGetCoreID(), (unsigned)uxTaskPriorityGet(NULL));
    image_u8_t image = {.width = (int)s_detect_width, .height = (int)s_detect_height,
                        .stride = (int)s_detect_width, .buf = s_gray};
    zarray_t *detections = apriltag_detector_detect(s_detector, &image);
    web_tag_t next[MAX_TAGS] = {0};
    size_t count = MIN((size_t)zarray_size(detections), (size_t)MAX_TAGS);
    const float fx = 125.7f * ((float)s_detect_width / 320.0f);
    const float fy = 125.7f * ((float)s_detect_height / 240.0f);
    const float sx = (float)s_width / s_detect_width;
    const float sy = (float)s_height / s_detect_height;

    for (size_t i = 0; i < count; ++i) {
        apriltag_detection_t *det;
        zarray_get(detections, (int)i, &det);
        next[i].id = det->id;
        next[i].center[0] = det->c[0] * sx;
        next[i].center[1] = det->c[1] * sy;
        for (int corner = 0; corner < 4; ++corner) {
            next[i].p[corner][0] = det->p[corner][0] * sx;
            next[i].p[corner][1] = det->p[corner][1] * sy;
        }
        next[i].rotation = atan2f(det->p[1][1] - det->p[0][1],
                                  det->p[1][0] - det->p[0][0]) * RAD_TO_DEG;
        matd_t *pose = homography_to_pose(det->H, -fx, fy, s_detect_width / 2.0f, s_detect_height / 2.0f);
        if (pose) {
            next[i].translation[0] = MATD_EL(pose, 0, 3);
            next[i].translation[1] = MATD_EL(pose, 1, 3);
            next[i].translation[2] = MATD_EL(pose, 2, 3);
            next[i].rotation_xyz[0] = atan2f(MATD_EL(pose, 2, 1), MATD_EL(pose, 2, 2)) * RAD_TO_DEG;
            next[i].rotation_xyz[1] = atan2f(-MATD_EL(pose, 2, 0),
                sqrtf(MATD_EL(pose, 2, 1) * MATD_EL(pose, 2, 1) +
                      MATD_EL(pose, 2, 2) * MATD_EL(pose, 2, 2))) * RAD_TO_DEG;
            next[i].rotation_xyz[2] = atan2f(MATD_EL(pose, 1, 0), MATD_EL(pose, 0, 0)) * RAD_TO_DEG;
            matd_destroy(pose);
        }
    }
    apriltag_detections_destroy(detections);
    if (count > 0) {
        hardware_test_camera_pass();

    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    memcpy(s_tags, next, count * sizeof(web_tag_t));
    s_tag_count = count;
    s_result_generation = generation;
    ++s_sequence;
    s_result_time = esp_timer_get_time();
    s_detection_ms = (s_result_time - start) / 1000.0f;
    xSemaphoreGive(s_mutex);
    ESP_LOGD(TAG, "Continuous detection completed: %u tags, %.1f ms",
             (unsigned)count, (esp_timer_get_time() - start) / 1000.0);
}

esp_err_t detection_service_get_json(char *buffer, size_t buffer_size)
{
    if (!buffer || buffer_size == 0 || !s_mutex) {
        return ESP_ERR_INVALID_ARG;
    }
    size_t used = 0;
    portENTER_CRITICAL(&s_request_lock);
    bool enabled = s_enabled, processing = s_processing;
    uint32_t generation = s_mode_generation;
    portEXIT_CRITICAL(&s_request_lock);
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    int n = snprintf(buffer, buffer_size,
                     "{\"sequence\":%" PRIu32 ",\"width\":%" PRIu32 ",\"height\":%" PRIu32
                     ",\"detection_ms\":%.1f,\"age_ms\":%.1f,\"enabled\":%s,\"processing\":%s,\"tags\":[",
                     s_sequence, s_width, s_height, s_detection_ms,
                     s_result_time ? (esp_timer_get_time() - s_result_time) / 1000.0 : -1.0,
                     enabled ? "true" : "false", processing ? "true" : "false");
    if (n < 0 || (size_t)n >= buffer_size) goto overflow;
    used = (size_t)n;
    for (size_t i = 0; i < (enabled && generation == s_result_generation ? s_tag_count : 0); ++i) {
        web_tag_t *t = &s_tags[i];
        n = snprintf(buffer + used, buffer_size - used,
            "%s{\"id\":%d,\"center\":[%.1f,%.1f],\"corners\":[[%.1f,%.1f],[%.1f,%.1f],[%.1f,%.1f],[%.1f,%.1f]],"
            "\"rotation\":%.2f,\"translation\":[%.4f,%.4f,%.4f],\"rotation_xyz\":[%.2f,%.2f,%.2f]}",
            i ? "," : "", t->id, t->center[0], t->center[1],
            t->p[0][0], t->p[0][1], t->p[1][0], t->p[1][1],
            t->p[2][0], t->p[2][1], t->p[3][0], t->p[3][1], t->rotation,
            t->translation[0], t->translation[1], t->translation[2],
            t->rotation_xyz[0], t->rotation_xyz[1], t->rotation_xyz[2]);
        if (n < 0 || (size_t)n >= buffer_size - used) goto overflow;
        used += (size_t)n;
    }
    n = snprintf(buffer + used, buffer_size - used, "]}");
    xSemaphoreGive(s_mutex);
    return (n < 0 || (size_t)n >= buffer_size - used) ? ESP_ERR_NO_MEM : ESP_OK;

overflow:
    xSemaphoreGive(s_mutex);
    buffer[0] = '\0';
    return ESP_ERR_NO_MEM;
}
