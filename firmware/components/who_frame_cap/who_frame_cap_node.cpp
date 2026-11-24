/**
 * @file who_frame_cap_node.cpp
 * @brief 帧采集管道节点实现文件
 * 
 * 该文件实现了 ESP-WHO 框架中帧采集管道的核心节点类，包括：
 * - WhoFrameCapNode: 帧采集节点基类，提供通用的节点管理和任务调度
 * - WhoFetchNode: 帧采集节点，从摄像头获取原始帧数据
 * - WhoDecodeNode: 帧解码节点，将 MJPEG 格式解码为 RGB 格式
 * - WhoPPAResizeNode: PPA 硬件缩放节点，利用 ESP32P4 的 PPA 外设进行图像缩放
 * 
 * 采用流水线架构，节点之间通过 FreeRTOS 队列进行数据传递，
 * 每个节点运行在独立的任务中，实现高效的并行处理。
 * 
 * ==================== 架构逻辑图 ====================
 * 
 * 【整体架构】
 * 
 *     ┌─────────────────────────────────────────────────────────────────┐
 *     │                        帧采集管道 (FrameCap Pipeline)            │
 *     ├─────────────────────────────────────────────────────────────────┤
 *     │                                                                 │
 *     │   ┌──────────┐    Queue    ┌──────────┐    Queue    ┌──────────┐│
 *     │   │ Fetch    │ ──────────> │ Decode   │ ──────────> │ PPAResize││
 *     │   │ Node     │             │ Node     │             │ Node     ││
 *     │   │ (采集)   │             │ (解码)   │             │ (缩放)   ││
 *     │   └────┬─────┘             └────┬─────┘             └────┬─────┘│
 *     │        │                        │                        │       │
 *     │        v                        v                        v       │
 *     │   ┌──────────────────────────────────────────────────────────┐   │
 *     │   │                    环形缓冲区 (Ring Buffer)              │   │
 *     │   │  [帧0] → [帧1] → [帧2] → ... → [帧N] (满时通知订阅者)    │   │
 *     │   └──────────────────────────────────────────────────────────┘   │
 *     │                                                                 │
 *     └─────────────────────────────────────────────────────────────────┘
 * 
 * 【节点任务主循环流程】
 * 
 *     ┌──────────────────┐
 *     │   任务启动       │
 *     └────────┬─────────┘
 *              v
 *     ┌──────────────────┐
 *     │ 接收输入帧       │◄─────────────────────┐
 *     │ (阻塞等待)       │                      │
 *     └────────┬─────────┘                      │
 *              v                                │
 *     ┌──────────────────┐                      │
 *     │ 检查控制信号      │                      │
 *     └────────┬─────────┘                      │
 *              │                                │
 *        ┌─────┴─────┐                          │
 *        ▼           ▼                          │
 *    [STOP?]      [PAUSE?]                      │
 *        │           │                          │
 *       YES         YES                         │
 *        │           │                          │
 *        v           v                          │
 *    ┌───────┐  ┌─────────┐                     │
 *    │ 退出  │  │等待恢复  │                     │
 *    └───────┘  └────┬────┘                     │
 *                    │                          │
 *                    v                          │
 *     ┌──────────────────┐                      │
 *     │ process() 处理   │                      │
 *     │ (子类实现)       │                      │
 *     └────────┬─────────┘                      │
 *              │                                │
 *              ▼                                │
 *     ┌──────────────────┐                      │
 *     │ 处理失败?        │                      │
 *     └────────┬─────────┘                      │
 *              │ YES                            │
 *              ├────────────────────────────────┤
 *              │ NO                             │
 *              v                                │
 *     ┌──────────────────┐                      │
 *     │ 发送到输出队列   │                      │
 *     │ (覆盖/阻塞模式)  │                      │
 *     └────────┬─────────┘                      │
 *              v                                │
 *     ┌──────────────────┐                      │
 *     │ 更新环形缓冲区   │                      │
 *     └────────┬─────────┘                      │
 *              │                                │
 *              ▼                                │
 *     ┌──────────────────┐                      │
 *     │ 缓冲区满?        │                      │
 *     └────────┬─────────┘                      │
 *              │ YES                            │
 *              v                                │
 *     ┌──────────────────┐                      │
 *     │ 通知订阅者       │                      │
 *     │ (NEW_FRAME事件)  │                      │
 *     └────────┬─────────┘                      │
 *              │                                │
 *              └────────────────────────────────┘
 * 
 * 【数据流时序】
 * 
 *   时间轴 ─────────────────────────────────────────────────────>
 * 
 *   Frame 0:  [Fetch] → [Decode] → [PPAResize] → [Detect] → [Display]
 *   Frame 1:            [Fetch] → [Decode] → [PPAResize] → [Detect] → [Display]
 *   Frame 2:                     [Fetch] → [Decode] → [PPAResize] → [Detect] → [Display]
 *             ↑                    ↑                    ↑
 *          采集延迟             处理延迟            显示延迟
 * 
 *   环形缓冲区大小 = MODEL_TIME + 额外缓冲
 *   确保显示帧与检测结果同步
 * 
 * ===============================================================
 */

#include "who_frame_cap_node.hpp"
#include "hal/cache_hal.h"
#include "hal/cache_ll.h"
#include <cstring>

using namespace who::cam;
static const char *TAG = "WhoFrameCapNode";

namespace who {
namespace frame_cap {
static bool pix_type_to_ppa_srm(dl::image::pix_type_t type,
                                ppa_srm_color_mode_t *mode,
                                bool *byte_swap)
{
    *byte_swap = false;
    switch (type) {
    case dl::image::DL_IMAGE_PIX_TYPE_RGB888:
        *mode = PPA_SRM_COLOR_MODE_RGB888;
        return true;
    case dl::image::DL_IMAGE_PIX_TYPE_RGB565LE:
        *mode = PPA_SRM_COLOR_MODE_RGB565;
        return true;
    case dl::image::DL_IMAGE_PIX_TYPE_RGB565BE:
        *mode = PPA_SRM_COLOR_MODE_RGB565;
        *byte_swap = true;
        return true;
    default:
        return false;
    }
}

static esp_err_t resize_ppa_rotate_180(const dl::image::img_t &src_img,
                                       dl::image::img_t &dst_img,
                                       ppa_client_handle_t ppa_handle,
                                       const std::vector<int> &crop_area = {})
{
    ppa_srm_color_mode_t input_srm_color_mode;
    ppa_srm_color_mode_t output_srm_color_mode;
    bool byte_swap = false;
    bool out_byte_swap = false;
    if (!pix_type_to_ppa_srm(src_img.pix_type, &input_srm_color_mode, &byte_swap) ||
        !pix_type_to_ppa_srm(dst_img.pix_type, &output_srm_color_mode, &out_byte_swap)) {
        return ESP_FAIL;
    }

    ppa_srm_oper_config_t srm_oper_config = {};
    if (crop_area.empty()) {
        srm_oper_config.in.block_offset_y = 0;
        srm_oper_config.in.block_offset_x = 0;
        srm_oper_config.in.block_h = src_img.height;
        srm_oper_config.in.block_w = src_img.width;
    } else {
        srm_oper_config.in.block_offset_y = crop_area[1];
        srm_oper_config.in.block_offset_x = crop_area[0];
        srm_oper_config.in.block_h = crop_area[3] - crop_area[1];
        srm_oper_config.in.block_w = crop_area[2] - crop_area[0];
    }

    srm_oper_config.in.buffer = src_img.data;
    srm_oper_config.in.pic_h = src_img.height;
    srm_oper_config.in.pic_w = src_img.width;
    srm_oper_config.in.srm_cm = input_srm_color_mode;
    srm_oper_config.rgb_swap = false;
    srm_oper_config.byte_swap = byte_swap;

    srm_oper_config.out.buffer = dst_img.data;
    size_t align = cache_hal_get_cache_line_size(CACHE_LL_LEVEL_EXT_MEM, CACHE_TYPE_DATA);
    srm_oper_config.out.buffer_size = dl::image::align_up(dst_img.bytes(), align);
    srm_oper_config.out.pic_h = dst_img.height;
    srm_oper_config.out.pic_w = dst_img.width;
    srm_oper_config.out.block_offset_x = 0;
    srm_oper_config.out.block_offset_y = 0;
    srm_oper_config.out.srm_cm = output_srm_color_mode;
    srm_oper_config.rotation_angle = PPA_SRM_ROTATION_ANGLE_180;
    srm_oper_config.scale_x = dl::image::get_ppa_scale(srm_oper_config.in.block_w, dst_img.width);
    srm_oper_config.scale_y = dl::image::get_ppa_scale(srm_oper_config.in.block_h, dst_img.height);
    srm_oper_config.mirror_x = false;
    srm_oper_config.mirror_y = false;

    return ppa_do_scale_rotate_mirror(ppa_handle, &srm_oper_config);
}

/**
 * @brief WhoFrameCapNode 构造函数
 * 
 * @param name 节点名称，用于日志和调试
 * @param ringbuf_len 环形缓冲区长度，必须 >= 1
 * @param out_queue_overwrite 输出队列是否允许覆盖（true=覆盖模式，false=阻塞等待）
 */
WhoFrameCapNode::WhoFrameCapNode(const std::string &name, uint8_t ringbuf_len, bool out_queue_overwrite) :
    task::WhoTask(name),           // 初始化父类任务
    m_out_queue_overwrite(out_queue_overwrite),  // 设置输出队列模式
    m_out_queue(nullptr),          // 输出队列初始化为空
    m_prev_node(nullptr),          // 前一个节点初始化为空
    m_next_node(nullptr),          // 后一个节点初始化为空
    m_in_queue(nullptr),           // 输入队列初始化为空
    m_cam_fbs(ringbuf_len),        // 初始化帧缓冲环形队列
    m_mutex(xSemaphoreCreateMutex()) // 创建互斥锁保护帧缓冲
{
    // 确保环形缓冲区至少有一个元素
    assert(ringbuf_len >= 1);
}

/**
 * @brief WhoFrameCapNode 析构函数
 * 
 * 释放互斥锁资源
 */
WhoFrameCapNode::~WhoFrameCapNode()
{
    vSemaphoreDelete(m_mutex);
}

/**
 * @brief 异步暂停节点任务
 * 
 * 向输入队列发送空指针，唤醒阻塞的任务处理循环，使其进入暂停状态
 * 
 * @return true 暂停成功，false 暂停失败
 */
bool WhoFrameCapNode::pause_async()
{
    if (task::WhoTask::pause_async()) {
        cam_fb_t *fb = nullptr;
        if (m_in_queue) {
            xQueueSend(m_in_queue, &fb, 0);  // 发送空指针唤醒任务
        }
        return true;
    }
    return false;
}

/**
 * @brief 异步停止节点任务
 * 
 * 向输入队列发送空指针，唤醒阻塞的任务处理循环，使其退出并停止
 * 
 * @return true 停止成功，false 停止失败
 */
bool WhoFrameCapNode::stop_async()
{
    if (task::WhoTask::stop_async()) {
        cam_fb_t *fb = nullptr;
        if (m_in_queue) {
            xQueueSend(m_in_queue, &fb, 0);  // 发送空指针唤醒任务
        }
        return true;
    }
    return false;
}

/**
 * @brief 查看帧缓冲区中的指定帧（非破坏性读取）
 * 
 * @param index 帧索引，-1 表示最新帧
 * @return cam_fb_t* 帧缓冲区指针，失败返回 nullptr
 */
cam_fb_t *WhoFrameCapNode::cam_fb_peek(int index)
{
    xSemaphoreTake(m_mutex, portMAX_DELAY);  // 获取互斥锁
    
    // 检查缓冲区是否为空
    if (m_cam_fbs.empty()) {
        ESP_LOGW(TAG, "%s: Unable to peek from an empty frame buffer.", get_name().c_str());
        xSemaphoreGive(m_mutex);
        return nullptr;
    }
    
    // index=-1 表示获取最新帧（最后一个）
    if (index == -1) {
        index = m_cam_fbs.size() - 1;
    }
    
    // 检查索引是否有效
    if (index < 0 || index > m_cam_fbs.size() - 1) {
        ESP_LOGW(TAG,
                 "%s: Invalid index %d, valid index should be [-1, %d].",
                 get_name().c_str(),
                 index,
                 m_cam_fbs.size() - 1);
        xSemaphoreGive(m_mutex);
        return nullptr;
    }
    
    cam_fb_t *ret = m_cam_fbs[index];
    xSemaphoreGive(m_mutex);  // 释放互斥锁
    return ret;
}

bool WhoFrameCapNode::cam_fb_snapshot(cam_fb_t &frame, int index, void *pixel_buffer, size_t pixel_buffer_size)
{
    xSemaphoreTake(m_mutex, portMAX_DELAY);
    if (index == -1) {
        index = static_cast<int>(m_cam_fbs.size()) - 1;
    }
    if (index < 0 || index >= static_cast<int>(m_cam_fbs.size())) {
        xSemaphoreGive(m_mutex);
        return false;
    }

    const cam_fb_t *fb = m_cam_fbs[index];
    if (!fb || !fb->buf || fb->len == 0 || fb->width == 0 || fb->height == 0 ||
        (pixel_buffer && fb->len > pixel_buffer_size)) {
        xSemaphoreGive(m_mutex);
        return false;
    }

    frame = *fb;
    // A snapshot must never be returned to the camera driver as an owned frame.
    frame.ret = nullptr;
    if (pixel_buffer) {
        memcpy(pixel_buffer, fb->buf, fb->len);
        frame.buf = pixel_buffer;
    }
    xSemaphoreGive(m_mutex);
    return true;
}

/**
 * @brief 添加新帧信号订阅者
 * 
 * 当环形缓冲区满时，会向所有订阅者发送 NEW_FRAME 事件
 * 
 * @param task 订阅任务指针
 */
void WhoFrameCapNode::add_new_frame_signal_subscriber(task::WhoTask *task)
{
    m_tasks.emplace_back(task);
}

/**
 * @brief 获取前一个节点
 * 
 * @return WhoFrameCapNode* 前一个节点指针，若无则返回 nullptr 并打印错误
 */
WhoFrameCapNode *WhoFrameCapNode::get_prev_node()
{
    if (!m_prev_node) {
        ESP_LOGE(TAG, "No prev node.");
    }
    return m_prev_node;
}

/**
 * @brief 获取后一个节点
 * 
 * @return WhoFrameCapNode* 后一个节点指针，若无则返回 nullptr 并打印错误
 */
WhoFrameCapNode *WhoFrameCapNode::get_next_node()
{
    if (!m_next_node) {
        ESP_LOGE(TAG, "No next node.");
    }
    return m_next_node;
}

/**
 * @brief 节点主任务循环
 * 
 * 该函数是每个帧采集节点的核心处理逻辑，执行以下步骤：
 * 1. 从输入队列接收帧数据
 * 2. 检查任务状态（暂停/停止）
 * 3. 调用 process() 处理帧数据
 * 4. 将处理后的帧发送到输出队列
 * 5. 更新环形缓冲区
 * 6. 当缓冲区满时通知订阅者
 */
void WhoFrameCapNode::task()
{
    while (true) {
        cam_fb_t *in_fb = nullptr;
        
        // 从输入队列接收帧（阻塞等待）
        if (m_in_queue) {
            xQueueReceive(m_in_queue, &in_fb, portMAX_DELAY);
        }
        
        // 检查任务控制信号（暂停/停止）
        EventBits_t event_bits = xEventGroupWaitBits(m_event_group, TASK_PAUSE | TASK_STOP, pdTRUE, pdFALSE, 0);
        
        // 如果收到停止信号，退出循环
        if (event_bits & TASK_STOP) {
            break;
        } 
        // 如果收到暂停信号，等待恢复或停止
        else if (event_bits & TASK_PAUSE) {
            xEventGroupSetBits(m_event_group, TASK_PAUSED);
            EventBits_t pause_event_bits =
                xEventGroupWaitBits(m_event_group, TASK_RESUME | TASK_STOP, pdTRUE, pdFALSE, portMAX_DELAY);
            if (pause_event_bits & TASK_STOP) {
                break;
            } else {
                continue;  // 恢复后继续处理
            }
        }
        
        // 调用子类实现的 process() 方法处理帧
        cam_fb_t *out_fb = process(in_fb);
        
        // 如果处理失败，跳过当前帧
        if (!out_fb) {
            continue;
        }
        
        // 将处理后的帧发送到输出队列
        if (m_out_queue) {
            if (m_out_queue_overwrite) {
                // 覆盖模式：直接覆盖队列中的旧数据（低延迟优先）
                xQueueOverwrite(m_out_queue, &out_fb);
            } else {
                // 阻塞模式：等待队列有空间（数据完整性优先）
                xQueueSend(m_out_queue, &out_fb, portMAX_DELAY);
            }
        }
        
        // 更新环形缓冲区
        xSemaphoreTake(m_mutex, portMAX_DELAY);
        update_ringbuf(out_fb);
        bool full_ringbuf = m_cam_fbs.full();
        xSemaphoreGive(m_mutex);
        
        // 当缓冲区满时，通知所有订阅任务有新帧可用
        if (full_ringbuf) {
            for (const auto &task : m_tasks) {
                if (task->is_active()) {
                    xEventGroupSetBits(task->get_event_group(), NEW_FRAME);
                }
            }
        }
    }
    
    // 标记任务已停止并删除任务
    xEventGroupSetBits(m_event_group, TASK_STOPPED);
    vTaskDelete(NULL);
}

// ==================== WhoFetchNode 实现 ====================

/**
 * @brief WhoFetchNode 析构函数
 * 
 * 释放摄像头实例
 */
WhoFetchNode::~WhoFetchNode()
{
    delete m_cam;
}

/**
 * @brief 清理帧缓冲区
 * 
 * 将环形缓冲区中的所有帧返回给摄像头驱动
 */
void WhoFetchNode::cleanup()
{
    xSemaphoreTake(m_mutex, portMAX_DELAY);
    while (!m_cam_fbs.empty()) {
        auto fb = m_cam_fbs.pop();
        m_cam->cam_fb_return(fb);  // 将帧返回给摄像头
    }
    xSemaphoreGive(m_mutex);
}

/**
 * @brief 处理帧数据（获取新帧）
 * 
 * @param fb 输入帧（对于 FetchNode，此参数未使用）
 * @return cam_fb_t* 从摄像头获取的新帧
 */
cam_fb_t *WhoFetchNode::process(who::cam::cam_fb_t *fb)
{
    return m_cam->cam_fb_get();  // 从摄像头获取下一帧
}

/**
 * @brief 更新环形缓冲区
 * 
 * 如果缓冲区已满，先弹出最旧的帧并返回给摄像头，再压入新帧
 * 
 * @param fb 新帧指针
 */
void WhoFetchNode::update_ringbuf(who::cam::cam_fb_t *fb)
{
    if (m_cam_fbs.full()) {
        auto fb_prev = m_cam_fbs.pop();
        m_cam->cam_fb_return(fb_prev);  // 返回旧帧给摄像头
    }
    m_cam_fbs.push(fb);  // 压入新帧
}

// ==================== WhoDecodeNode 实现 ====================

/**
 * @brief 清理解码节点资源
 * 
 * 清空输入队列和环形缓冲区中的所有帧
 */
void WhoDecodeNode::cleanup()
{
    // 清空输入队列
    while (uxQueueMessagesWaiting(m_in_queue) > 0) {
        cam_fb_t *tmp;
        xQueueReceive(m_in_queue, &tmp, 0);
    }
    
    // 清空环形缓冲区
    xSemaphoreTake(m_mutex, portMAX_DELAY);
    while (!m_cam_fbs.empty()) {
        auto fb = m_cam_fbs.pop();
        heap_caps_free(fb->buf);  // 释放帧数据缓冲区
        delete fb;                // 释放帧结构
    }
    xSemaphoreGive(m_mutex);
}

/**
 * @brief 解码 MJPEG 帧
 * 
 * 根据硬件配置选择硬件解码或软件解码
 * 
 * @param fb 输入的 MJPEG 帧
 * @return cam_fb_t* 解码后的 RGB 帧，解码失败返回 nullptr
 */
cam_fb_t *WhoDecodeNode::process(who::cam::cam_fb_t *fb)
{
    auto timestamp = fb->timestamp;  // 保存原始时间戳
    
    // 选择解码方式：硬件解码（如果支持）或软件解码
#if CONFIG_SOC_JPEG_CODEC_SUPPORTED
    auto img = hw_decode_jpeg({fb->buf, fb->len}, m_pix_type);
#else
    auto img = sw_decode_jpeg({fb->buf, fb->len}, m_pix_type);
#endif
    
    // 解码失败处理（帧数据损坏等情况）
    if (!img.data) {
        return nullptr;
    }
    
    return new cam_fb_t(img, timestamp);  // 创建新帧并返回
}

/**
 * @brief 更新环形缓冲区
 * 
 * 如果缓冲区已满，先释放最旧帧的内存，再压入新帧
 * 
 * @param fb 解码后的新帧
 */
void WhoDecodeNode::update_ringbuf(who::cam::cam_fb_t *fb)
{
    if (m_cam_fbs.full()) {
        auto fb_prev = m_cam_fbs.pop();
        heap_caps_free(fb_prev->buf);  // 释放帧数据
        delete fb_prev;                // 释放帧结构
    }
    m_cam_fbs.push(fb);
}

/**
 * @brief WhoPPAResizeNode 构造函数
 * 
 * 初始化 PPA 客户端并分配输出图像缓冲区
 * 
 * @param name 节点名称
 * @param dst_w 目标图像宽度
 * @param dst_h 目标图像高度
 * @param dst_pix_type 目标像素格式
 * @param ringbuf_len 环形缓冲区长度
 * @param out_queue_overwrite 输出队列覆盖模式
 */
WhoPPAResizeNode::WhoPPAResizeNode(const std::string &name,
                                   uint16_t dst_w,
                                   uint16_t dst_h,
                                   dl::image::pix_type_t dst_pix_type,
                                   uint8_t ringbuf_len,
                                   bool out_queue_overwrite) :
    WhoFrameCapNode(name, ringbuf_len, out_queue_overwrite),  // 初始化父类
    m_dst_w(dst_w),           // 目标宽度
    m_dst_h(dst_h),           // 目标高度
    m_dst_imgs(ringbuf_len + 1),  // 输出图像缓冲区（比环形缓冲区多一个）
    m_img_idx(0)              // 当前使用的图像索引
{
    // 配置 PPA 客户端
    ppa_client_config_t ppa_client_config = {};
    ppa_client_config.oper_type = PPA_OPERATION_SRM;  // 选择缩放旋转镜像操作
    ESP_ERROR_CHECK(ppa_register_client(&ppa_client_config, &m_ppa_srm_handle));
    
    // 获取缓存行对齐要求
    size_t align = cache_hal_get_cache_line_size(CACHE_LL_LEVEL_EXT_MEM, CACHE_TYPE_DATA);
    
    // 预分配输出图像缓冲区
    for (int i = 0; i < m_dst_imgs.size(); i++) {
        auto &img = m_dst_imgs[i];
        img.width = dst_w;
        img.height = dst_h;
        img.pix_type = dst_pix_type;
        
        // 计算缓冲区大小（仅第一次计算）
        if (i == 0) {
            m_buf_size = dl::image::align_up(img.bytes(), align);
        }
        
        // 分配 DMA 可访问的 SPI RAM 缓冲区
        img.data = heap_caps_aligned_calloc(align, 1, m_buf_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA);
        if (img.data == nullptr) {
            ESP_LOGE(TAG,
                     "%s: Failed to allocate PPA output buffer %d/%d (%u bytes)",
                     get_name().c_str(),
                     i + 1,
                     static_cast<int>(m_dst_imgs.size()),
                     static_cast<unsigned>(m_buf_size));
            ESP_ERROR_CHECK(ESP_ERR_NO_MEM);
        }
    }
}

/**
 * @brief WhoPPAResizeNode 析构函数
 * 
 * 注销 PPA 客户端并释放所有输出图像缓冲区
 */
WhoPPAResizeNode::~WhoPPAResizeNode()
{
    ESP_ERROR_CHECK(ppa_unregister_client(m_ppa_srm_handle));
    
    // 释放所有预分配的图像缓冲区
    for (int i = 0; i < m_dst_imgs.size(); i++) {
        heap_caps_free(m_dst_imgs[i].data);
    }
}

/**
 * @brief 清理 PPA 缩放节点资源
 * 
 * 清空输入队列和环形缓冲区
 */
void WhoPPAResizeNode::cleanup()
{
    // 清空输入队列
    while (uxQueueMessagesWaiting(m_in_queue) > 0) {
        cam_fb_t *tmp;
        xQueueReceive(m_in_queue, &tmp, 0);
    }
    
    // 清空环形缓冲区
    xSemaphoreTake(m_mutex, portMAX_DELAY);
    while (!m_cam_fbs.empty()) {
        auto fb = m_cam_fbs.pop();
        delete fb;
    }
    xSemaphoreGive(m_mutex);
}

/**
 * @brief 使用 PPA 硬件缩放图像
 * 
 * @param fb 输入帧
 * @return cam_fb_t* 缩放后的帧
 */
cam_fb_t *WhoPPAResizeNode::process(who::cam::cam_fb_t *fb)
{
    auto timestamp = fb->timestamp;  // 保存原始时间戳
    auto dst_img = get_dst_img();    // 获取下一个输出缓冲区
    
    // 打印输入图像尺寸
    // ESP_LOGI(TAG, "Input frame size: width=%d, height=%d", fb->width, fb->height);
    
    if (fb->width == 800 && fb->height == 600) {
        resize_ppa_rotate_180(*fb, dst_img, m_ppa_srm_handle, {80, 60, 720, 540});
    } else {
        dl::image::resize_ppa(*fb, dst_img, m_ppa_srm_handle);
    }
    
    return new cam_fb_t(dst_img, timestamp);  // 创建新帧并返回
}

/**
 * @brief 更新环形缓冲区
 * 
 * @param fb 缩放后的新帧
 */
void WhoPPAResizeNode::update_ringbuf(who::cam::cam_fb_t *fb)
{
    if (m_cam_fbs.full()) {
        auto fb_prev = m_cam_fbs.pop();
        delete fb_prev;
    }
    m_cam_fbs.push(fb);
}

/**
 * @brief 获取下一个输出图像缓冲区（循环使用）
 * 
 * @return dl::image::img_t 输出图像结构
 */
dl::image::img_t WhoPPAResizeNode::get_dst_img()
{
    auto &img = m_dst_imgs[m_img_idx];
    m_img_idx = (m_img_idx + 1) % m_dst_imgs.size();  // 循环索引
    return img;
}



} // namespace frame_cap
} // namespace who
