#pragma once

#include <atomic>
#include <cstdint>
#include <string>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

struct httpd_req;

namespace who {
namespace frame_cap {
class WhoFrameCapNode;
}
namespace web {

/**
 * ESP32P4C5 CAM Web 回传服务（与 ESP32P4C5Mini_Factory 架构一致）
 *
 * 暴露端点：
 *   http://192.168.5.1/                   官方相机会话前端（带实时 SVG 检测框叠加层）
 *   http://192.168.5.1/api/get_camera_info 前端摄像头信息与分辨率格式查询（支持 320x240 与 640x480）
 *   http://192.168.5.1/api/capture_image  JPEG 单帧捕获下载
 *   http://192.168.5.1/api/capture_binary 原始 RGB565 裸流下载
 *   http://192.168.5.1/api/set_camera_config 设置 JPEG 压缩质量与分辨率
 *   http://192.168.5.1/api/detections     实时检测结果 JSON
 *   http://192.168.5.1/api/frame.jpg      快速获取最新一帧 JPEG
 *   http://192.168.5.1:81/stream          MJPEG 视频流（支持 ?source=0）
 */
class WhoWeb {
public:
    static WhoWeb &get_instance();

    /**
     * 启动网络与图传服务
     *
     * @param title      例程名称（显示在网页顶栏）
     * @param type       检测类型标识（cat, dog, face, apriltag, ...）
     * @param frame_node 视频采集节点（提供 LCD 分辨率 RGB565 帧）
     */
    esp_err_t start(const char *title, const char *type, frame_cap::WhoFrameCapNode *frame_node);

    /** 更新检测结果 JSON（数组格式） */
    void set_objects_json(const std::string &objects_json, uint64_t timestamp_ms = 0);
    void set_objects_json(std::string &&objects_json, uint64_t timestamp_ms = 0);

    /** 清空当前检测结果 */
    void clear_objects();

    /** 动态设置分辨率格式（0: 320x240, 1: 640x480） */
    esp_err_t set_format(int format_id);
    int get_current_format() const { return m_current_format.load(); }

    /** 动态设置压缩质量 (1 - 100) */
    esp_err_t set_quality(int quality);
    uint8_t get_quality() const { return m_jpeg_quality.load(); }

    /** 重新配置硬件 JPEG 编码器 */
    esp_err_t reinit_encoder(int format_id, int quality);

    bool is_started() const { return m_started.load(); }

    uint16_t get_width() const { return (m_current_format.load() == 1) ? 640 : m_width; }
    uint16_t get_height() const { return (m_current_format.load() == 1) ? 480 : m_height; }

private:
    WhoWeb();
    ~WhoWeb();
    WhoWeb(const WhoWeb &) = delete;
    WhoWeb &operator=(const WhoWeb &) = delete;

    static void encoder_task_entry(void *arg);
    void encoder_task();
    bool fetch_frame_rgb(int format);
    esp_err_t start_http_servers();
    void stop_http_servers();
    void release_resources();

    /* HTTP 请求处理函数 (兼容 ESP32P4C5Mini_Factory) */
    static esp_err_t static_file_handler(struct httpd_req *req);
    static esp_err_t camera_info_handler(struct httpd_req *req);
    static esp_err_t capture_image_handler(struct httpd_req *req);
    static esp_err_t capture_binary_handler(struct httpd_req *req);
    static esp_err_t camera_settings_handler(struct httpd_req *req);
    static esp_err_t detections_handler(struct httpd_req *req);
    static esp_err_t stream_handler(struct httpd_req *req);

    std::atomic<bool> m_started{false};
    std::atomic<bool> m_encoder_running{false};
    std::atomic<float> m_stream_fps{0.0f};
    std::atomic<uint32_t> m_frame_count{0};
    std::atomic<uint64_t> m_result_timestamp_ms{0};
    std::atomic<int> m_stream_clients{0};
    std::atomic<uint8_t> m_jpeg_quality{60};
    std::atomic<int> m_current_format{0}; // 0: 320x240, 1: 640x480

    SemaphoreHandle_t m_jpeg_lock = nullptr;
    SemaphoreHandle_t m_objects_lock = nullptr;
    SemaphoreHandle_t m_frame_sem = nullptr;
    TaskHandle_t m_encoder_task = nullptr;
    void *m_jpeg_encoder = nullptr;
    void *m_ppa_handle = nullptr;

    std::string m_title;
    std::string m_type;
    std::string m_objects_json = "[]";

    frame_cap::WhoFrameCapNode *m_frame_node = nullptr;
    uint16_t m_width = 320;
    uint16_t m_height = 240;

    static constexpr size_t JPEG_BUFFER_COUNT = 3;
    static constexpr size_t STREAM_HEADER_RESERVE = 128;

    uint8_t *m_rgb_buffer_320 = nullptr;
    uint8_t *m_rgb_buffer_640 = nullptr;
    uint8_t *m_jpeg_buffers[JPEG_BUFFER_COUNT] = {nullptr, nullptr, nullptr};
    size_t m_jpeg_lengths[JPEG_BUFFER_COUNT] = {0, 0, 0};
    size_t m_rgb_buffer_320_size = 0;
    size_t m_rgb_buffer_640_size = 0;
    size_t m_jpeg_buffer_size = 0;
    int m_encode_index = 0;
    int m_ready_index = -1;
    int m_sending_index = -1;
    uint32_t m_ready_seq = 0;

    void *m_http_server = nullptr;
    void *m_stream_server = nullptr;
};

} // namespace web
} // namespace who
