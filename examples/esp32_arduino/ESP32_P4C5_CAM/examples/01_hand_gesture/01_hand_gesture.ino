#include <Arduino.h>
#include <Wire.h>
#include "driver/i2c.h"

#include <ESP32_P4C5_CAM.h>

// I2C 硬件参数：设备地址、引脚、端口、速率与单次最大缓冲
#define I2C_ADDR 0x54
// Standard ESP32 DevKit default pins: SDA=21, SCL=22 (adjust SDA/SCL pins if using custom boards)
#define I2C_SDA 21
#define I2C_SCL 22
#define I2C_PORT I2C_NUM_0
#define I2C_FREQ_HZ 400000U
#define I2C_BUFFER_SIZE 1024



static esp32_p4c5_cam_handle_t s_cam;

static esp32_p4c5_cam_err_t app_i2c_write(const uint8_t *data, size_t len, uint32_t timeout_ms);
static esp32_p4c5_cam_err_t app_i2c_read(uint8_t *data, size_t len, uint32_t timeout_ms);

static void print_color_result(const esp32_p4c5_cam_color_result_t *result);
static void print_line_result(const esp32_p4c5_cam_line_result_t *result);
static void print_hand_gesture_result(const esp32_p4c5_cam_hand_gesture_result_t *result);
static void print_target_detection_result(const char *label, const esp32_p4c5_cam_target_detection_result_t *result);
static void print_pose_result(const esp32_p4c5_cam_pose_result_t *result);
static void print_apriltag_result(const esp32_p4c5_cam_apriltag_result_t *result);
static void print_qrcode_result(const esp32_p4c5_cam_qrcode_result_t *result);

void setup()
{
    Serial.begin(115200);
    delay(1000);

    // 初始化 I2C 主设备：指定 SDA/SCL、加入总线、设置通信速率
    Wire.setPins(I2C_SDA, I2C_SCL);
    Wire.begin();
    Wire.setClock(I2C_FREQ_HZ);

    // 初始化 ESP32-P4C5-CAM 句柄并注册 I2C 读写回调
    esp32_p4c5_cam_init(&s_cam);
    esp32_p4c5_cam_register_i2c_callbacks(&s_cam, app_i2c_write, app_i2c_read);

    

}

void loop()
{
    // 获取手势识别结果
    esp32_p4c5_cam_hand_gesture_result_t hand_result;
    esp32_p4c5_cam_err_t hand_ret = esp32_p4c5_cam_get_hand_gesture_result(&s_cam, &hand_result, 1000);
    if (hand_ret == ESP32_P4C5_CAM_OK) {
        print_hand_gesture_result(&hand_result);
    } else {
        Serial.print("esp32_p4c5_cam_get_hand_gesture_result failed: ");
        Serial.println((int)hand_ret);
    }
    delay(20);
}

static void print_color_result(const esp32_p4c5_cam_color_result_t *result)
{
    if(result->count == 0) return;
    for (uint8_t i = 0; i < result->count; i++) {
        const esp32_p4c5_cam_color_block_t *block = &result->blocks[i];
        // index: 色块编号；(x,y): 中心坐标；width/height: 色块宽高
        Serial.print("  index=");
        Serial.print(block->index);
        Serial.print(" x=");
        Serial.print(block->x);
        Serial.print(" y=");
        Serial.print(block->y);
        Serial.print(" w=");
        Serial.print(block->width);
        Serial.print(" h=");
        Serial.println(block->height);
    }
}

static void print_line_result(const esp32_p4c5_cam_line_result_t *result)
{
    Serial.print("line_detected=");
    Serial.println(result->detected ? 1 : 0);

    if (!result->detected) {
        return;
    }

    // 直线起点、终点、角度（度）与偏移量
    Serial.print("  start=(");
    Serial.print(result->start_x);
    Serial.print(',');
    Serial.print(result->start_y);
    Serial.print(") end=(");
    Serial.print(result->end_x);
    Serial.print(',');
    Serial.print(result->end_y);
    Serial.print(") angle=");
    Serial.print(result->angle_deg, 2);
    Serial.print(" offset=");
    Serial.println(result->offset, 3);
}

static void print_hand_gesture_result(const esp32_p4c5_cam_hand_gesture_result_t *result)
{
    // total_count: 设备返回的目标总数；parsed_count: 实际解析出的目标数
    Serial.print("hand_gesture_total=");
    Serial.print(result->total_count);
    Serial.print(" parsed=");
    Serial.println(result->parsed_count);

    for (uint8_t i = 0; i < result->parsed_count; i++) {
        const esp32_p4c5_cam_hand_gesture_item_t *item = &result->items[i];
        // gesture: 手势类型；x/y/w/h: 手势框中心坐标与宽高
        Serial.print("  gesture=");
        Serial.print(item->gesture);
        Serial.print(" x=");
        Serial.print(item->x);
        Serial.print(" y=");
        Serial.print(item->y);
        Serial.print(" w=");
        Serial.print(item->width);
        Serial.print(" h=");
        Serial.println(item->height);
    }
}

static void print_target_detection_result(const char *label, const esp32_p4c5_cam_target_detection_result_t *result)
{
    Serial.print(label);
    Serial.print("_total=");
    Serial.print(result->total_count);
    Serial.print(" parsed=");
    Serial.println(result->parsed_count);

    for (uint8_t i = 0; i < result->parsed_count; i++) {
        const esp32_p4c5_cam_target_detection_item_t *item = &result->items[i];
        // index: 目标索引；x/y/w/h: 检测框中心坐标与宽高
        Serial.print("  index=");
        Serial.print(item->index);
        Serial.print(" x=");
        Serial.print(item->x);
        Serial.print(" y=");
        Serial.print(item->y);
        Serial.print(" w=");
        Serial.print(item->width);
        Serial.print(" h=");
        Serial.println(item->height);
    }
}

static void print_pose_result(const esp32_p4c5_cam_pose_result_t *result)
{
    Serial.print("pose_total=");
    Serial.print(result->total_count);
    Serial.print(" parsed=");
    Serial.println(result->parsed_count);

    for (uint8_t i = 0; i < result->parsed_count; i++) {
        const esp32_p4c5_cam_pose_item_t *item = &result->items[i];
        // 人体框信息
        Serial.print("  index=");
        Serial.print(item->index);
        Serial.print(" x=");
        Serial.print(item->x);
        Serial.print(" y=");
        Serial.print(item->y);
        Serial.print(" w=");
        Serial.print(item->width);
        Serial.print(" h=");
        Serial.println(item->height);

        // 关键点数量及各关键点坐标
        Serial.print(" keypoints=");
        Serial.println(item->keypoint_count);

        for (uint8_t k = 0; k < item->keypoint_count; k++) {
            Serial.print("    keypoint");
            Serial.print(item->keypoints[k].index);
            Serial.print("=(");
            Serial.print(item->keypoints[k].x);
            Serial.print(',');
            Serial.print(item->keypoints[k].y);
            Serial.println(')');
        }
    }
}

static void print_apriltag_result(const esp32_p4c5_cam_apriltag_result_t *result)
{
    Serial.print("apriltag_total=");
    Serial.print(result->total_count);
    Serial.print(" parsed=");
    Serial.println(result->parsed_count);

    for (uint8_t i = 0; i < result->parsed_count; i++) {
        const esp32_p4c5_cam_apriltag_item_t *item = &result->items[i];
        // id: AprilTag 标签 ID；rotation: 平面旋转角；trans/rot_xyz: 平移与旋转分量
        Serial.print("  id=");
        Serial.print(item->id);
        Serial.print(" x=");
        Serial.print(item->x);
        Serial.print(" y=");
        Serial.print(item->y);
        Serial.print(" w=");
        Serial.print(item->width);
        Serial.print(" h=");
        Serial.print(item->height);
        Serial.print(" rotation=");
        Serial.print(item->rotation, 3);
        Serial.print(" trans=(");
        Serial.print(item->x_translation, 3);
        Serial.print(',');
        Serial.print(item->y_translation, 3);
        Serial.print(',');
        Serial.print(item->z_translation, 3);
        Serial.print(") rot_xyz=(");
        Serial.print(item->x_rotation, 3);
        Serial.print(',');
        Serial.print(item->y_rotation, 3);
        Serial.print(',');
        Serial.print(item->z_rotation, 3);
        Serial.println(')');
    }
}

static void print_qrcode_result(const esp32_p4c5_cam_qrcode_result_t *result)
{
    Serial.print("qrcode_total=");
    Serial.print(result->total_count);
    Serial.print(" parsed=");
    Serial.println(result->parsed_count);

    for (uint8_t i = 0; i < result->parsed_count; i++) {
        const esp32_p4c5_cam_qrcode_item_t *item = &result->items[i];
        // text_len: 二维码文本长度；text: 解码后的文本内容
        Serial.print("  index=");
        Serial.print(item->index);
        Serial.print(" x=");
        Serial.print(item->x);
        Serial.print(" y=");
        Serial.print(item->y);
        Serial.print(" w=");
        Serial.print(item->width);
        Serial.print(" h=");
        Serial.print(item->height);
        Serial.print(" text_len=");
        Serial.print(item->text_len);
        Serial.print(" text=");
        Serial.println(item->text);
    }
}

static esp32_p4c5_cam_err_t app_i2c_write(const uint8_t *data, size_t len, uint32_t timeout_ms)
{
    if (data == NULL || len == 0) {
        return ESP32_P4C5_CAM_ERR_INVALID_ARG;
    }

    TickType_t wait_ticks = (timeout_ms == 0) ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
    size_t offset = 0;

    // 分块写入：单次最多 I2C_BUFFER_SIZE 字节，超过则循环发送
    while (offset < len) {
        size_t chunk_len = len - offset;
        if (chunk_len > I2C_BUFFER_SIZE) {
            chunk_len = I2C_BUFFER_SIZE;
        }

        esp_err_t ret = i2c_master_write_to_device(I2C_PORT,
                                                   I2C_ADDR,
                                                   &data[offset],
                                                   chunk_len,
                                                   wait_ticks);
        // 将 ESP-IDF 错误码映射为 ESP32-P4C5-CAM 错误码
        switch (ret) {
        case ESP_OK:
            break;
        case ESP_ERR_TIMEOUT:
            return ESP32_P4C5_CAM_ERR_TIMEOUT;
        case ESP_ERR_INVALID_ARG:
            return ESP32_P4C5_CAM_ERR_INVALID_ARG;
        case ESP_ERR_INVALID_SIZE:
            return ESP32_P4C5_CAM_ERR_INVALID_SIZE;
        default:
            return ESP32_P4C5_CAM_ERR_FAIL;
        }

        offset += chunk_len;
    }
    return ESP32_P4C5_CAM_OK;
}

static esp32_p4c5_cam_err_t app_i2c_read(uint8_t *data, size_t len, uint32_t timeout_ms)
{
    if (data == NULL || len == 0) {
        return ESP32_P4C5_CAM_ERR_INVALID_ARG;
    }

    // 读取长度超过缓冲区上限时直接返回错误
    if (len > I2C_BUFFER_SIZE) {
        return ESP32_P4C5_CAM_ERR_INVALID_SIZE;
    }

    // timeout_ms 为 0 时永久等待，否则转换为 FreeRTOS Tick
    TickType_t wait_ticks = (timeout_ms == 0) ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
    esp_err_t ret = i2c_master_read_from_device(I2C_PORT,
                                                I2C_ADDR,
                                                data,
                                                len,
                                                wait_ticks);
    switch (ret) {
    case ESP_OK:
        return ESP32_P4C5_CAM_OK;
    case ESP_ERR_TIMEOUT:
        return ESP32_P4C5_CAM_ERR_TIMEOUT;
    case ESP_ERR_INVALID_ARG:
        return ESP32_P4C5_CAM_ERR_INVALID_ARG;
    case ESP_ERR_INVALID_SIZE:
        return ESP32_P4C5_CAM_ERR_INVALID_SIZE;
    default:
        return ESP32_P4C5_CAM_ERR_FAIL;
    }
}
