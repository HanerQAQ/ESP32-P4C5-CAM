/**
 * @file app.c
 * @brief STM32 HAL example for ESP32-P4C5-CAM visual module (01_hand_gesture).
 *
 * Demonstrates communicating with ESP32-P4C5-CAM over I2C
 * (slave address 0x54, 400 kHz) to retrieve and display vision results.
 */

#include "ESP32_P4C5_CAM.h"
#include "ESP32_P4C5_CAM_port.h"

#if defined(STM32F4) || defined(STM32F1) || defined(STM32H7) || defined(STM32G4)
#include "main.h"
#endif

#ifdef HAL_I2C_MODULE_ENABLED
extern I2C_HandleTypeDef ESP32_P4C5_CAM_I2C_HANDLE;
#endif

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

// Polling period in milliseconds
#define ESP32_P4C5_CAM_DEMO_POLL_PERIOD_MS        200U

// 若使用颜色/巡线识别示例，可取消对应模式宏的注释。
// #define STARTUP_VISION_MODE ESP32_P4C5_CAM_VISION_MODE_COLOR
// #define STARTUP_VISION_MODE ESP32_P4C5_CAM_VISION_MODE_LINE

static esp32_p4c5_cam_handle_t s_cam;      // ESP32-P4C5-CAM 句柄
static bool s_esp32_p4c5_cam_ready = false;      // 模块就绪标志
// static esp32_p4c5_cam_color_config_t s_color_config;


// 扫描 I2C2 总线上的设备并打印地址
static void print_i2c_scan(void)
{
    uint8_t found_count = 0;

    printf("I2C scan start\r\n");
    for (uint8_t address = 0x03; address <= 0x77; address++) {
        if (HAL_I2C_IsDeviceReady(&ESP32_P4C5_CAM_I2C_HANDLE, (uint16_t)(address << 1), 1, 10) == HAL_OK) {
            printf("I2C device found at 0x%02X\r\n", address);
            found_count++;
        }
    }

    if (found_count == 0U) {
        printf("I2C scan result: no device found\r\n");
    }
}

// 打印颜色配置信息
static void print_color_config(const esp32_p4c5_cam_color_config_t *config)
{
    printf("mode=%u registered_colors=%u\r\n",
           config->mode,
           config->registered_color_count);
}

// 打印颜色识别结果
static void print_color_result(const esp32_p4c5_cam_color_result_t *result)
{
    if (result->count == 0U) {
        return;
    }

    printf("color_count=%u\r\n", result->count);
    for (uint8_t i = 0; i < result->count; i++) {
        const esp32_p4c5_cam_color_block_t *block = &result->blocks[i];
        // index: 色块编号；(x,y): 中心坐标；width/height: 色块宽高
        printf("  index=%u x=%u y=%u w=%u h=%u\r\n",
               block->index,
               block->x,
               block->y,
               block->width,
               block->height);
    }
}

// 打印巡线识别结果
static void print_line_result(const esp32_p4c5_cam_line_result_t *result)
{
    printf("line_detected=%u\r\n", result->detected ? 1U : 0U);
    if (!result->detected) {
        return;
    }

    // 直线起点、终点、角度（度）与偏移量
    printf("  start=(%u,%u) end=(%u,%u) angle=%.2f offset=%.3f\r\n",
           result->start_x,
           result->start_y,
           result->end_x,
           result->end_y,
           (double)result->angle_deg,
           (double)result->offset);
}

// 打印手势识别结果
static void print_hand_gesture_result(const esp32_p4c5_cam_hand_gesture_result_t *result)
{
    // total_count: 设备返回的目标总数；parsed_count: 实际解析出的目标数
    printf("hand_gesture_total=%u parsed=%u\r\n", result->total_count, result->parsed_count);
    for (uint8_t i = 0; i < result->parsed_count; i++) {
        const esp32_p4c5_cam_hand_gesture_item_t *item = &result->items[i];
        // gesture: 手势类型；x/y/w/h: 手势框中心坐标与宽高
        printf("  gesture=%u x=%u y=%u w=%u h=%u\r\n",
               item->gesture,
               item->x,
               item->y,
               item->width,
               item->height);
    }
}

// 打印通用目标检测结果（猫、狗、行人等）
static void print_target_detection_result(const char *label,
                                          const esp32_p4c5_cam_target_detection_result_t *result)
{
    printf("%s_total=%u parsed=%u\r\n", label, result->total_count, result->parsed_count);
    for (uint8_t i = 0; i < result->parsed_count; i++) {
        const esp32_p4c5_cam_target_detection_item_t *item = &result->items[i];
        // index: 目标索引；x/y/w/h: 检测框中心坐标与宽高
        printf("  index=%u x=%u y=%u w=%u h=%u\r\n",
               item->index,
               item->x,
               item->y,
               item->width,
               item->height);
    }
}

// 打印人体姿态识别结果
static void print_pose_result(const esp32_p4c5_cam_pose_result_t *result)
{
    printf("pose_total=%u parsed=%u\r\n", result->total_count, result->parsed_count);
    for (uint8_t i = 0; i < result->parsed_count; i++) {
        const esp32_p4c5_cam_pose_item_t *item = &result->items[i];
        // 人体框信息及关键点数量
        printf("  index=%u x=%u y=%u w=%u h=%u keypoints=%u\r\n",
               item->index,
               item->x,
               item->y,
               item->width,
               item->height,
               item->keypoint_count);
    }
}

// 打印 AprilTag 识别结果
static void print_apriltag_result(const esp32_p4c5_cam_apriltag_result_t *result)
{
    printf("apriltag_total=%u parsed=%u\r\n", result->total_count, result->parsed_count);
    for (uint8_t i = 0; i < result->parsed_count; i++) {
        const esp32_p4c5_cam_apriltag_item_t *item = &result->items[i];
        // id: 标签 ID；rotation: 平面旋转角
        printf("  id=%u x=%u y=%u w=%u h=%u rotation=%.3f\r\n",
               item->id,
               item->x,
               item->y,
               item->width,
               item->height,
               (double)item->rotation);
    }
}

// 打印二维码识别结果
static void print_qrcode_result(const esp32_p4c5_cam_qrcode_result_t *result)
{
    printf("qrcode_total=%u parsed=%u\r\n", result->total_count, result->parsed_count);
    for (uint8_t i = 0; i < result->parsed_count; i++) {
        const esp32_p4c5_cam_qrcode_item_t *item = &result->items[i];
        // text_len: 二维码文本长度；text: 解码后的文本内容
        printf("  index=%u x=%u y=%u w=%u h=%u text_len=%u text=%s\r\n",
               item->index,
               item->x,
               item->y,
               item->width,
               item->height,
               item->text_len,
               item->text);
    }
}

// 运行当前启用的 ESP32-P4C5-CAM 演示功能：获取手势识别结果并打印
static void run_enabled_esp32_p4c5_cam_demo(void)
{
     esp32_p4c5_cam_hand_gesture_result_t hand_result;
     esp32_p4c5_cam_err_t hand_ret = esp32_p4c5_cam_get_hand_gesture_result(&s_cam, &hand_result, 1000);
     if (hand_ret == ESP32_P4C5_CAM_OK) {
         print_hand_gesture_result(&hand_result);
     } else {
         printf("esp32_p4c5_cam_get_hand_gesture_result failed: %d\r\n", (int)hand_ret);
     }
}

// 应用主任务入口：初始化外设、扫描 I2C、初始化 ESP32-P4C5-CAM，并循环获取识别结果
void app_task_entry(void *argument)
{
    (void)argument;

    printf("ESP32-P4C5-CAM STM32 visual demo startup\r\n");
    printf("STM32 I2C bus: I2C2 PB10=SCL, PB11=SDA, address=0x%02X\r\n",
           ESP32_P4C5_CAM_I2C_ADDRESS);
    print_i2c_scan();

    // 初始化 ESP32-P4C5-CAM 并等待模块就绪
    s_esp32_p4c5_cam_ready = esp32_p4c5_cam_port_init(&s_cam) &&
                        esp32_p4c5_cam_port_is_ready(100);
    printf("ESP32-P4C5-CAM init result: %s\r\n", s_esp32_p4c5_cam_ready ? "ok" : "fail");

    
    for (;;) {
        static uint32_t last_retry_ms = 0;

        // 未就绪时每隔 5 秒重试初始化
        if (!s_esp32_p4c5_cam_ready) {
            uint32_t now = esp32_p4c5_cam_port_get_tick_ms();
            if ((now - last_retry_ms) >= 5000U) {
                last_retry_ms = now;
                s_esp32_p4c5_cam_ready = esp32_p4c5_cam_port_init(&s_cam) &&
                                    esp32_p4c5_cam_port_is_ready(100);
                printf("ESP32-P4C5-CAM retry result: %s\r\n", s_esp32_p4c5_cam_ready ? "ok" : "fail");
            }
            esp32_p4c5_cam_port_delay_ms(100);
            continue;
        }

        // 运行演示功能并周期性轮询
        run_enabled_esp32_p4c5_cam_demo();
        esp32_p4c5_cam_port_delay_ms(ESP32_P4C5_CAM_DEMO_POLL_PERIOD_MS);
    }
}
