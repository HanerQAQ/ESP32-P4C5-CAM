#include <Arduino.h>
#include <Wire.h>
#include "driver/i2c.h"

#include <ESP32_P4C5_CAM.h>

#define I2C_ADDR 0x54
// Standard ESP32 DevKit default pins: SDA=21, SCL=22 (adjust SDA/SCL pins if using custom boards)
#define I2C_SDA 21
#define I2C_SCL 22
#define I2C_PORT I2C_NUM_0
#define I2C_FREQ_HZ 400000U
#define I2C_BUFFER_SIZE 1024

// 视觉模式选择宏：本示例启用颜色识别模式；
// 如需切换为巡线模式，注释掉 COLOR 宏并取消 LINE 宏的注释。
#define STARTUP_VISION_MODE ESP32_P4C5_CAM_VISION_MODE_COLOR
// #define STARTUP_VISION_MODE ESP32_P4C5_CAM_VISION_MODE_LINE

static esp32_p4c5_cam_handle_t s_cam;      // ESP32-P4C5-CAM 句柄
static esp32_p4c5_cam_color_config_t config;     // 视觉模式与已注册颜色数量配置

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

    // 初始化 I2C 总线
    Wire.setPins(I2C_SDA, I2C_SCL);
    Wire.begin();
    Wire.setClock(I2C_FREQ_HZ);

    // 初始化 ESP32-P4C5-CAM 并注册 I2C 读写回调
    esp32_p4c5_cam_init(&s_cam);
    esp32_p4c5_cam_register_i2c_callbacks(&s_cam, app_i2c_write, app_i2c_read);

    
    bool setting_success = false;
    esp32_p4c5_cam_err_t ret;

    // 根据宏选择视觉模式：颜色识别可注册多个颜色，巡线只能注册一个目标颜色
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
        Serial.print("esp32_p4c5_cam_set_vision_mode failed: ");
        Serial.println((int)ret);
        return;
    }

    // 获取当前视觉配置并打印模式与已注册颜色数量
    ret = esp32_p4c5_cam_get_color_config(&s_cam, &config, 1000);
    if (ret != ESP32_P4C5_CAM_OK) {
        Serial.print("esp32_p4c5_cam_get_color_config failed: ");
        Serial.println((int)ret);
        return;
    }

    Serial.print("mode=");
    Serial.print(config.mode);
    Serial.print(" registered_colors=");
    Serial.println(config.registered_color_count);
}

void loop()
{
    // 根据当前视觉模式调用对应 API 获取结果
    esp32_p4c5_cam_err_t ret;
    if (config.mode == ESP32_P4C5_CAM_VISION_MODE_COLOR) {
        esp32_p4c5_cam_color_result_t result;
        ret = esp32_p4c5_cam_get_color_result(&s_cam, &result, 1000);
        if (ret == ESP32_P4C5_CAM_OK) {
            print_color_result(&result);
        }
    } else if (config.mode == ESP32_P4C5_CAM_VISION_MODE_LINE) {
        esp32_p4c5_cam_line_result_t result;
        ret = esp32_p4c5_cam_get_line_result(&s_cam, &result, 1000);
        if (ret == ESP32_P4C5_CAM_OK) {
            print_line_result(&result);
        }
    } else {
        ret = ESP32_P4C5_CAM_ERR_INVALID_RESPONSE;
    }

    if (ret != ESP32_P4C5_CAM_OK) {
        Serial.print("esp32_p4c5_cam_get_result failed: ");
        Serial.println((int)ret);
    }

    delay(20);
}

// 打印颜色块结果：index 为颜色编号，x/y/w/h 为色块中心坐标与宽高
static void print_color_result(const esp32_p4c5_cam_color_result_t *result)
{
    if(result->count == 0) return;
    for (uint8_t i = 0; i < result->count; i++) {
        const esp32_p4c5_cam_color_block_t *block = &result->blocks[i];
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

// 打印巡线结果：detected 表示是否检测到线；start/end 为线段端点，angle 为角度，offset 为偏离量
static void print_line_result(const esp32_p4c5_cam_line_result_t *result)
{
    Serial.print("line_detected=");
    Serial.println(result->detected ? 1 : 0);

    if (!result->detected) {
        return;
    }

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
    Serial.print("hand_gesture_total=");
    Serial.print(result->total_count);
    Serial.print(" parsed=");
    Serial.println(result->parsed_count);

    for (uint8_t i = 0; i < result->parsed_count; i++) {
        const esp32_p4c5_cam_hand_gesture_item_t *item = &result->items[i];
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

// 打印二维码结果：index 为二维码序号，x/y/w/h 为位置与尺寸，text 为解码内容
static void print_qrcode_result(const esp32_p4c5_cam_qrcode_result_t *result)
{
    Serial.print("qrcode_total=");
    Serial.print(result->total_count);
    Serial.print(" parsed=");
    Serial.println(result->parsed_count);

    for (uint8_t i = 0; i < result->parsed_count; i++) {
        const esp32_p4c5_cam_qrcode_item_t *item = &result->items[i];
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

// I2C 底层写函数：将长数据按 I2C_BUFFER_SIZE 分块发送，并把 esp_err 映射为 esp32_p4c5_cam_err
static esp32_p4c5_cam_err_t app_i2c_write(const uint8_t *data, size_t len, uint32_t timeout_ms)
{
    if (data == NULL || len == 0) {
        return ESP32_P4C5_CAM_ERR_INVALID_ARG;
    }

    TickType_t wait_ticks = (timeout_ms == 0) ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
    size_t offset = 0;
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

// I2C 底层读函数：单次读取长度不得超过 I2C_BUFFER_SIZE，并将 esp_err 映射为 esp32_p4c5_cam_err
static esp32_p4c5_cam_err_t app_i2c_read(uint8_t *data, size_t len, uint32_t timeout_ms)
{
    if (data == NULL || len == 0) {
        return ESP32_P4C5_CAM_ERR_INVALID_ARG;
    }

    if (len > I2C_BUFFER_SIZE) {
        return ESP32_P4C5_CAM_ERR_INVALID_SIZE;
    }

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
