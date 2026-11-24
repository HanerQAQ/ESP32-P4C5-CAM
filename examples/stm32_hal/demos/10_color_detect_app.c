/**
 * @file app.c
 * @brief STM32 HAL example for ESP32-P4C5-CAM visual module (10_color_detect).
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
#define ESP32_P4C5_CAM_DEMO_POLL_PERIOD_MS        200U // 演示轮询周期，单位毫秒

// 颜色识别示例：默认启用颜色模式
#define STARTUP_VISION_MODE ESP32_P4C5_CAM_VISION_MODE_COLOR
// #define STARTUP_VISION_MODE ESP32_P4C5_CAM_VISION_MODE_LINE

static esp32_p4c5_cam_handle_t s_cam;       // ESP32-P4C5-CAM 句柄
static bool s_esp32_p4c5_cam_ready = false;        // 模块就绪标志
static esp32_p4c5_cam_color_config_t s_color_config; // 颜色/巡线配置


// 扫描 I2C 总线上的设备并打印地址
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

// 打印颜色/巡线模块配置信息
static void print_color_config(const esp32_p4c5_cam_color_config_t *config)
{
    printf("mode=%u registered_colors=%u\r\n",
           config->mode,
           config->registered_color_count);
}

// 打印颜色识别结果
static void print_color_result(const esp32_p4c5_cam_color_result_t *result)
{
    printf("color_count=%u\r\n", result->count);
    for (uint8_t i = 0; i < result->count; i++) {
        const esp32_p4c5_cam_color_block_t *block = &result->blocks[i];
        printf("  index=%u x=%u y=%u w=%u h=%u\r\n",
               block->index,
               block->x,
               block->y,
               block->width,
               block->height);
    }
}

// 打印巡线结果
static void print_line_result(const esp32_p4c5_cam_line_result_t *result)
{
    printf("line_detected=%u\r\n", result->detected ? 1U : 0U);
    if (!result->detected) {
        return;
    }

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
    printf("hand_gesture_total=%u parsed=%u\r\n", result->total_count, result->parsed_count);
    for (uint8_t i = 0; i < result->parsed_count; i++) {
        const esp32_p4c5_cam_hand_gesture_item_t *item = &result->items[i];
        printf("  gesture=%u x=%u y=%u w=%u h=%u\r\n",
               item->gesture,
               item->x,
               item->y,
               item->width,
               item->height);
    }
}

// 打印目标检测结果
static void print_target_detection_result(const char *label,
                                          const esp32_p4c5_cam_target_detection_result_t *result)
{
    printf("%s_total=%u parsed=%u\r\n", label, result->total_count, result->parsed_count);
    for (uint8_t i = 0; i < result->parsed_count; i++) {
        const esp32_p4c5_cam_target_detection_item_t *item = &result->items[i];
        printf("  index=%u x=%u y=%u w=%u h=%u\r\n",
               item->index,
               item->x,
               item->y,
               item->width,
               item->height);
    }
}

// 打印姿态识别结果
static void print_pose_result(const esp32_p4c5_cam_pose_result_t *result)
{
    printf("pose_total=%u parsed=%u\r\n", result->total_count, result->parsed_count);
    for (uint8_t i = 0; i < result->parsed_count; i++) {
        const esp32_p4c5_cam_pose_item_t *item = &result->items[i];
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

// 颜色/巡线演示：根据当前配置模式获取对应结果并打印
static void run_enabled_esp32_p4c5_cam_demo(void)
{
/****************************************color recognition example*********************************************************/
     esp32_p4c5_cam_err_t ret;
     if (s_color_config.mode == ESP32_P4C5_CAM_VISION_MODE_COLOR) {
         esp32_p4c5_cam_color_result_t result;
         ret = esp32_p4c5_cam_get_color_result(&s_cam, &result, 1000);
         if (ret == ESP32_P4C5_CAM_OK) {
             print_color_result(&result);
         }
     } else if (s_color_config.mode == ESP32_P4C5_CAM_VISION_MODE_LINE) {
         esp32_p4c5_cam_line_result_t result;
         ret = esp32_p4c5_cam_get_line_result(&s_cam, &result, 1000);
         if (ret == ESP32_P4C5_CAM_OK) {
             print_line_result(&result);
         }
     } else {
         ret = ESP32_P4C5_CAM_ERR_INVALID_RESPONSE;
     }

     if (ret != ESP32_P4C5_CAM_OK) {
         printf("esp32_p4c5_cam_get_result failed: %d\r\n", (int)ret);
     }

}

// 应用主任务入口：初始化外设，配置视觉模式，等待就绪后周期性运行演示
void app_task_entry(void *argument)
{
    (void)argument;

    printf("ESP32-P4C5-CAM STM32 visual demo startup\r\n");
    printf("STM32 I2C bus: I2C2 PB10=SCL, PB11=SDA, address=0x%02X\r\n",
           ESP32_P4C5_CAM_I2C_ADDRESS);
    print_i2c_scan();

    s_esp32_p4c5_cam_ready = esp32_p4c5_cam_port_init(&s_cam) &&
                        esp32_p4c5_cam_port_is_ready(100);
    printf("ESP32-P4C5-CAM init result: %s\r\n", s_esp32_p4c5_cam_ready ? "ok" : "fail");

    if (s_esp32_p4c5_cam_ready) {
        esp32_p4c5_cam_err_t ret = ESP32_P4C5_CAM_OK;

        // 根据 STARTUP_VISION_MODE 设置视觉模式：颜色识别或巡线
        bool setting_success = false;
        if (STARTUP_VISION_MODE == ESP32_P4C5_CAM_VISION_MODE_COLOR) {
            const uint8_t color_targets[] = {1, 2, 3, 4, 5};
            ret = esp32_p4c5_cam_set_vision_mode(&s_cam,
                                            ESP32_P4C5_CAM_VISION_MODE_COLOR,
                                            color_targets,
                                            sizeof(color_targets) / sizeof(color_targets[0]),
                                            &setting_success,
                                            1000);
        } else {
            const uint8_t line_target[] = {1};
            ret = esp32_p4c5_cam_set_vision_mode(&s_cam,
                                            ESP32_P4C5_CAM_VISION_MODE_LINE,
                                            line_target,
                                            sizeof(line_target) / sizeof(line_target[0]),
                                            &setting_success,
                                            1000);
        }

        if (ret != ESP32_P4C5_CAM_OK || !setting_success) {
            printf("esp32_p4c5_cam_set_vision_mode failed: %d success=%d\r\n", (int)ret, (int)setting_success);
        } else {
            // 读取并打印当前颜色/巡线配置
            ret = esp32_p4c5_cam_get_color_config(&s_cam, &s_color_config, 1000);
            if (ret != ESP32_P4C5_CAM_OK) {
                printf("esp32_p4c5_cam_get_color_config failed: %d\r\n", (int)ret);
            } else {
                print_color_config(&s_color_config);
            }
        }
    }

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

        // 模块就绪后周期性运行演示
        run_enabled_esp32_p4c5_cam_demo();
        esp32_p4c5_cam_port_delay_ms(ESP32_P4C5_CAM_DEMO_POLL_PERIOD_MS);
    }
}
