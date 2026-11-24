#include "app_myi2c.h"
#include "camera_service.h"
#include "app_mywifi.h"
#include "driver/i2c_slave.h"
#include "driver/gpio.h"
#include "cJSON.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "app_myi2c";

#define I2C_RX_BUF_SIZE 512
#define I2C_TX_MAX_SIZE 1024

typedef struct {
    uint16_t len;
    uint8_t  data[I2C_RX_BUF_SIZE];
} i2c_rx_msg_t;

static i2c_slave_dev_handle_t s_slave_handle = NULL;
static QueueHandle_t s_rx_queue = NULL;
static SemaphoreHandle_t s_i2c_mutex = NULL;

static uint8_t s_tx_frames[I2C_TX_MAX_SIZE];
static int s_tx_frames_len = 0;

/*
 * ESP32-P4C5-CAM Protocol response framing (standard 32-byte chunk format)
 * Header: 0xAA 0x55
 * Byte 2-3: data_len (uint16_t LE)
 * Byte 4-5: fragment_id (uint16_t LE, starts from 1)
 * Byte 6-7: total_fragments (uint16_t LE)
 * Byte 8..8+chunk_len-1: DATA
 * Byte 8+chunk_len: XOR CHECKSUM
 * Padded with 0s to 32 bytes
 */
static void i2c_build_response_frames(const uint8_t *payload, uint16_t payload_len, uint8_t *out_buf, int *out_len)
{
    uint16_t actual_len = (payload_len == 0) ? 1 : payload_len;
    uint8_t dummy = 0;
    const uint8_t *p_data = (payload_len == 0) ? &dummy : payload;

    uint16_t total_fragments = (actual_len + 21) / 22;
    if (total_fragments == 0) total_fragments = 1;

    *out_len = total_fragments * 32;

    for (uint16_t f = 1; f <= total_fragments; f++) {
        uint8_t *frame = out_buf + (f - 1) * 32;
        uint16_t offset = (f - 1) * 22;
        uint16_t chunk_len = (actual_len - offset > 22) ? 22 : (actual_len - offset);

        frame[0] = 0xAA;
        frame[1] = 0x55;
        frame[2] = (uint8_t)(chunk_len & 0xFF);
        frame[3] = (uint8_t)((chunk_len >> 8) & 0xFF);
        frame[4] = (uint8_t)(f & 0xFF);
        frame[5] = (uint8_t)((f >> 8) & 0xFF);
        frame[6] = (uint8_t)(total_fragments & 0xFF);
        frame[7] = (uint8_t)((total_fragments >> 8) & 0xFF);

        memcpy(&frame[8], p_data + offset, chunk_len);

        uint8_t checksum = 0;
        for (size_t i = 0; i < chunk_len; i++) {
            checksum ^= frame[8 + i];
        }
        frame[8 + chunk_len] = checksum;

        if (8 + chunk_len + 1 < 32) {
            memset(&frame[8 + chunk_len + 1], 0, 32 - (8 + chunk_len + 1));
        }
    }
}

static void send_response_frames(const uint8_t *resp_data, uint16_t resp_len)
{
    if (!s_slave_handle) return;
    if (s_i2c_mutex) xSemaphoreTake(s_i2c_mutex, portMAX_DELAY);

    i2c_build_response_frames(resp_data, resp_len, s_tx_frames, &s_tx_frames_len);

    i2c_slave_reset_tx_fifo(s_slave_handle);
    uint32_t written = 0;
    esp_err_t err = i2c_slave_write(s_slave_handle, s_tx_frames, (uint32_t)s_tx_frames_len, &written, 50);
    if (err != ESP_OK) {
        ESP_LOGD(TAG, "i2c_slave_write: %s, written=%" PRIu32, esp_err_to_name(err), written);
    }

    if (s_i2c_mutex) xSemaphoreGive(s_i2c_mutex);
}

static void refresh_default_tx_buffer(void)
{
    uint32_t w = 640, h = 480;
    camera_service_get_resolution(&w, &h);

    char json_buf[256];
    snprintf(json_buf, sizeof(json_buf),
             "{\"status\":\"ok\",\"type\":\"image_trans\",\"board\":\"ESP32-P4\","
             "\"sta_ip\":\"%s\",\"ap_ip\":\"%s\",\"stream_port\":81,\"w\":%" PRIu32 ",\"h\":%" PRIu32 "}",
             app_mywifi_get_sta_ip(), app_mywifi_get_ap_ip(), w, h);

    send_response_frames((const uint8_t *)json_buf, (uint16_t)strlen(json_buf));
}

static void process_i2c_command(const uint8_t *data, uint16_t len)
{
    if (!data || len == 0) return;

    // Check for JSON format
    const char *pstart = strchr((const char *)data, '{');
    if (pstart) {
        // Fast-path get_info query
        if (strstr(pstart, "\"get_info\"")) {
            uint32_t w = 640, h = 480;
            camera_service_get_resolution(&w, &h);
            float fps = camera_service_get_fps();
            int hflip = camera_service_get_hflip();
            int vflip = camera_service_get_vflip();

            char fast_resp[384];
            snprintf(fast_resp, sizeof(fast_resp),
                     "{\"status\":\"ok\",\"type\":\"image_trans\",\"board\":\"ESP32-P4\","
                     "\"wifi\":\"%s\",\"sta_ip\":\"%s\",\"ap_ip\":\"%s\",\"stream_port\":81,"
                     "\"w\":%" PRIu32 ",\"h\":%" PRIu32 ",\"fps\":%.1f,\"hflip\":%d,\"vflip\":%d}",
                     app_mywifi_get_disconnect_message(),
                     app_mywifi_get_sta_ip(),
                     app_mywifi_get_ap_ip(),
                     w, h, fps, hflip, vflip);

            send_response_frames((const uint8_t *)fast_resp, (uint16_t)strlen(fast_resp));
            return;
        }

        // Fast-path vision_setting
        if (strstr(pstart, "\"vision_setting\"")) {
            bool ok = false;
            const char *phm = strstr(pstart, "\"hmirror\":");
            if (phm) {
                camera_service_set_hflip(atoi(phm + 10) ? 1 : 0);
                ok = true;
            }
            const char *pvf = strstr(pstart, "\"vflip\":");
            if (pvf) {
                camera_service_set_vflip(atoi(pvf + 8) ? 1 : 0);
                ok = true;
            }
            const char *pq = strstr(pstart, "\"quality\":");
            if (pq) {
                camera_service_set_quality(atoi(pq + 10));
                ok = true;
            }

            uint8_t resp = ok ? 1 : 0;
            send_response_frames(&resp, 1);
            return;
        }

        // General cJSON parsing
        cJSON *root = cJSON_Parse(pstart);
        if (root) {
            cJSON *cmd_item = cJSON_GetObjectItem(root, "command");
            if (cmd_item && cJSON_IsString(cmd_item)) {
                const char *cmd = cmd_item->valuestring;
                if (strcmp(cmd, "module_reset") == 0) {
                    uint8_t resp = 1;
                    send_response_frames(&resp, 1);
                    cJSON_Delete(root);
                    vTaskDelay(pdMS_TO_TICKS(100));
                    esp_restart();
                    return;
                } else if (strcmp(cmd, "vision_setting") == 0) {
                    cJSON *params = cJSON_GetObjectItem(root, "params");
                    bool ok = false;
                    if (params && cJSON_IsObject(params)) {
                        cJSON *hmirror = cJSON_GetObjectItem(params, "hmirror");
                        if (hmirror && cJSON_IsNumber(hmirror)) {
                            camera_service_set_hflip(hmirror->valueint ? 1 : 0);
                            ok = true;
                        }
                        cJSON *vflip = cJSON_GetObjectItem(params, "vflip");
                        if (vflip && cJSON_IsNumber(vflip)) {
                            camera_service_set_vflip(vflip->valueint ? 1 : 0);
                            ok = true;
                        }
                        cJSON *quality = cJSON_GetObjectItem(params, "quality");
                        if (quality && cJSON_IsNumber(quality)) {
                            camera_service_set_quality(quality->valueint);
                            ok = true;
                        }
                    }
                    uint8_t resp = ok ? 1 : 0;
                    send_response_frames(&resp, 1);
                } else {
                    refresh_default_tx_buffer();
                }
            }
            cJSON_Delete(root);
            return;
        }
    }

    // Binary register writes [REG, VALUE] (backward compatibility with S3)
    if (len >= 2) {
        uint8_t reg = data[0];
        uint8_t val = data[1];
        if (reg == 0x01) {
            camera_service_set_hflip(val ? 1 : 0);
        } else if (reg == 0x02) {
            camera_service_set_vflip(val ? 1 : 0);
        } else if (reg == 0x04) {
            if (val == 0x01) {
                vTaskDelay(pdMS_TO_TICKS(100));
                esp_restart();
            }
        }
    }
}

static void i2c_slave_task(void *arg)
{
    i2c_rx_msg_t msg;
    ESP_LOGI(TAG, "I2C Slave task running on 0x%02X (SCL=GPIO%d, SDA=GPIO%d)",
             P4C5_CAM_I2C_SLAVE_ADDR, P4C5_CAM_I2C_SCL_PIN, P4C5_CAM_I2C_SDA_PIN);

    refresh_default_tx_buffer();

    while (1) {
        if (xQueueReceive(s_rx_queue, &msg, portMAX_DELAY) == pdTRUE) {
            process_i2c_command(msg.data, msg.len);
        }
    }
}

static bool on_i2c_slave_receive(i2c_slave_dev_handle_t i2c_slave,
                                 const i2c_slave_rx_done_event_data_t *evt_data,
                                 void *arg)
{
    (void)i2c_slave;
    (void)arg;
    if (!evt_data || evt_data->length == 0 || !s_rx_queue) {
        return false;
    }

    i2c_rx_msg_t msg;
    msg.len = (evt_data->length > I2C_RX_BUF_SIZE - 1) ? I2C_RX_BUF_SIZE - 1 : (uint16_t)evt_data->length;
    memcpy(msg.data, evt_data->buffer, msg.len);
    msg.data[msg.len] = '\0';

    BaseType_t high_task_woken = pdFALSE;
    xQueueSendFromISR(s_rx_queue, &msg, &high_task_woken);
    return high_task_woken == pdTRUE;
}

static bool on_i2c_slave_request(i2c_slave_dev_handle_t i2c_slave,
                                 const i2c_slave_request_event_data_t *evt_data,
                                 void *arg)
{
    (void)i2c_slave;
    (void)evt_data;
    (void)arg;
    return false;
}

esp_err_t app_myi2c_init(void)
{
    s_rx_queue = xQueueCreate(8, sizeof(i2c_rx_msg_t));
    if (!s_rx_queue) {
        ESP_LOGE(TAG, "Failed to create I2C RX queue");
        return ESP_ERR_NO_MEM;
    }

    s_i2c_mutex = xSemaphoreCreateMutex();
    if (!s_i2c_mutex) {
        ESP_LOGE(TAG, "Failed to create I2C mutex");
        return ESP_ERR_NO_MEM;
    }

    i2c_slave_config_t config = {
        .i2c_port = P4C5_CAM_I2C_PORT,
        .sda_io_num = P4C5_CAM_I2C_SDA_PIN,
        .scl_io_num = P4C5_CAM_I2C_SCL_PIN,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .send_buf_depth = 1024,
        .receive_buf_depth = 1024,
        .slave_addr = P4C5_CAM_I2C_SLAVE_ADDR,
        .addr_bit_len = I2C_ADDR_BIT_LEN_7,
        .flags.enable_internal_pullup = true,
    };

    esp_err_t err = i2c_new_slave_device(&config, &s_slave_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2c_new_slave_device failed: %s", esp_err_to_name(err));
        return err;
    }

    i2c_slave_event_callbacks_t cbs = {
        .on_receive = on_i2c_slave_receive,
        .on_request = on_i2c_slave_request,
    };
    err = i2c_slave_register_event_callbacks(s_slave_handle, &cbs, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2c_slave_register_event_callbacks failed: %s", esp_err_to_name(err));
        return err;
    }

    xTaskCreate(i2c_slave_task, "i2c_slave_task", 4096, NULL, 5, NULL);
    ESP_LOGI(TAG, "ESP32-P4C5-CAM I2C Slave initialized (Addr: 0x%02X)", P4C5_CAM_I2C_SLAVE_ADDR);
    return ESP_OK;
}
