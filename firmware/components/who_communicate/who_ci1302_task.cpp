#include "who_ci1302_task.hpp"

#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "who_speech_recognition_store.hpp"
#include "p4c5_cam_board.h"

#include <algorithm>
#include <cassert>
#include <cstring>

static const char *TAG = "WhoCi1302Task";

namespace who {
namespace communicate {

WhoCi1302Task *WhoCi1302Task::get_instance()
{
    static WhoCi1302Task instance("CI1302");
    return &instance;
}

WhoCi1302Task::WhoCi1302Task(const std::string &name) :
    task::WhoTask(name),
    m_tx_queue(xQueueCreate(TX_QUEUE_SIZE, sizeof(tx_msg_t))),
    m_hardware_inited(false),
    m_rx_frame{},
    m_rx_index(0),
    m_both_pressed_at(0),
    m_reset_started_at(0),
    m_reset_triggered(false),
    m_reset_active(false)
{
    assert(m_tx_queue);
}

WhoCi1302Task::~WhoCi1302Task()
{
    deinit_hardware();
    vQueueDelete(m_tx_queue);
}

bool WhoCi1302Task::send(const uint8_t *data, size_t len, TickType_t timeout)
{
    if (!data || len == 0) {
        return false;
    }

    size_t offset = 0;
    while (offset < len) {
        tx_msg_t msg = {};
        msg.len = std::min(TX_CHUNK_SIZE, len - offset);
        memcpy(msg.data, data + offset, msg.len);
        if (xQueueSend(m_tx_queue, &msg, timeout) != pdTRUE) {
            return false;
        }
        offset += msg.len;
    }
    return true;
}

esp_err_t WhoCi1302Task::init_hardware()
{
    if (m_hardware_inited) {
        return ESP_OK;
    }

    ESP_RETURN_ON_ERROR(bsp_buttons_init(), TAG, "initialize buttons");
    ESP_RETURN_ON_ERROR(bsp_ci1302_uart_init(), TAG, "initialize CI1302 UART");
    m_hardware_inited = true;
    return ESP_OK;
}

void WhoCi1302Task::deinit_hardware()
{
    if (!m_hardware_inited) {
        return;
    }

    if (m_reset_active) {
        gpio_set_level(BSP_VOICE_POWER, 1);
        gpio_set_level(BSP_VOICE_ENABLE, 1);
        m_reset_active = false;
    }

    esp_err_t err = bsp_ci1302_uart_deinit();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "deinitialize CI1302 UART failed: %s", esp_err_to_name(err));
        return;
    }
    m_hardware_inited = false;
}

void WhoCi1302Task::handle_pause()
{
    xEventGroupSetBits(m_event_group, TASK_PAUSED);
    EventBits_t event_bits =
        xEventGroupWaitBits(m_event_group, TASK_RESUME | TASK_STOP, pdTRUE, pdFALSE, portMAX_DELAY);
    if (event_bits & TASK_STOP) {
        xEventGroupSetBits(m_event_group, TASK_STOP);
    }
}

void WhoCi1302Task::process_rx_byte(uint8_t byte)
{
    switch (m_rx_index) {
    case 0:
        if (byte == 0xAA) {
            m_rx_frame[m_rx_index++] = byte;
        }
        break;
    case 1:
        if (byte == 0x55) {
            m_rx_frame[m_rx_index++] = byte;
        } else {
            m_rx_index = byte == 0xAA ? 1 : 0;
            if (m_rx_index == 1) {
                m_rx_frame[0] = 0xAA;
            }
        }
        break;
    case 2:
    case 3:
        m_rx_frame[m_rx_index++] = byte;
        break;
    case 4:
        if (byte == 0xFB) {
            store_speech_recognition_result(m_rx_frame[2], m_rx_frame[3]);
            ESP_LOGI(TAG, "RX frame: 0x%02X 0x%02X", m_rx_frame[2], m_rx_frame[3]);
            m_rx_index = 0;
        } else {
            m_rx_index = byte == 0xAA ? 1 : 0;
            if (m_rx_index == 1) {
                m_rx_frame[0] = 0xAA;
            }
        }
        break;
    default:
        m_rx_index = 0;
        break;
    }
}

void WhoCi1302Task::drain_uart_rx()
{
    uint8_t data[UART_BUFFER_SIZE];
    int len = uart_read_bytes(BSP_VOICE_UART_NUM, data, sizeof(data), 0);
    if (len > 0) {
        // ESP_LOG_BUFFER_HEXDUMP(TAG, data, len, ESP_LOG_DEBUG);
        for (int i = 0; i < len; ++i) {
            process_rx_byte(data[i]);
        }
    } else if (len < 0) {
        ESP_LOGW(TAG, "UART read failed");
    }
}

void WhoCi1302Task::drain_uart_tx()
{
    tx_msg_t msg = {};
    while (xQueueReceive(m_tx_queue, &msg, 0) == pdTRUE) {
        int written = uart_write_bytes(BSP_VOICE_UART_NUM,
                                       reinterpret_cast<const char *>(msg.data),
                                       msg.len);
        if (written != static_cast<int>(msg.len)) {
            ESP_LOGW(TAG, "UART TX incomplete: %d/%u", written, static_cast<unsigned>(msg.len));
        } else {
            ESP_LOGI(TAG, "UART TX (%u bytes):", static_cast<unsigned>(msg.len));
            ESP_LOG_BUFFER_HEXDUMP(TAG, msg.data, msg.len, ESP_LOG_INFO);
        }
    }
}

void WhoCi1302Task::process_buttons(int64_t now)
{
    if (m_reset_active) {
        if (now - m_reset_started_at >= RESET_TIME_US) {
            ESP_ERROR_CHECK(gpio_set_level(BSP_VOICE_POWER, 1));
            ESP_ERROR_CHECK(gpio_set_level(BSP_VOICE_ENABLE, 1));
            m_reset_active = false;
            ESP_LOGI(TAG, "CI1302 reset complete");
        }
        return;
    }

    const bool both_pressed = gpio_get_level(BSP_BUTTON_A) == 0 &&
                              gpio_get_level(BSP_BUTTON_B) == 0;
    if (!both_pressed) {
        m_both_pressed_at = 0;
        m_reset_triggered = false;
    } else if (m_both_pressed_at == 0) {
        m_both_pressed_at = now;
    } else if (!m_reset_triggered && now - m_both_pressed_at >= BUTTON_HOLD_TIME_US) {
        m_reset_triggered = true;
        m_reset_active = true;
        m_reset_started_at = now;
        ESP_LOGI(TAG, "Resetting CI1302");
        ESP_ERROR_CHECK(gpio_set_level(BSP_VOICE_POWER, 0));
        ESP_ERROR_CHECK(gpio_set_level(BSP_VOICE_ENABLE, 0));
        uart_flush_input(BSP_VOICE_UART_NUM);
    }
}

void WhoCi1302Task::task()
{
    if (init_hardware() != ESP_OK) {
        xEventGroupSetBits(m_event_group, TASK_STOPPED);
        vTaskDelete(nullptr);
        return;
    }

    while (true) {
        EventBits_t event_bits =
            xEventGroupWaitBits(m_event_group, TASK_PAUSE | TASK_STOP, pdTRUE, pdFALSE, 0);
        if (event_bits & TASK_STOP) {
            break;
        } else if (event_bits & TASK_PAUSE) {
            handle_pause();
            continue;
        }

        process_buttons(esp_timer_get_time());
        if (!m_reset_active) {
            drain_uart_rx();
            drain_uart_tx();
        }
        vTaskDelay(IDLE_DELAY);
    }

    deinit_hardware();
    xEventGroupSetBits(m_event_group, TASK_STOPPED);
    vTaskDelete(nullptr);
}

void WhoCi1302Task::cleanup()
{
    xQueueReset(m_tx_queue);
    memset(m_rx_frame, 0, sizeof(m_rx_frame));
    m_rx_index = 0;
    m_both_pressed_at = 0;
    m_reset_started_at = 0;
    m_reset_triggered = false;
    m_reset_active = false;
}

} // namespace communicate
} // namespace who
