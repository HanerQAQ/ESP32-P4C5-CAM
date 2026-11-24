#include "who_i2c_slave_task.hpp"
#include "who_protocol.hpp"
#include "p4c5_cam_board.h"
#include "esp_check.h"
#include "esp_log.h"
#include <cassert>
#include <cstring>
#include <limits>

static const char *TAG = "WhoI2CSlaveTask";

namespace who {
namespace communicate {

WhoI2CSlaveTask::WhoI2CSlaveTask(const std::string &name, const WhoI2CSlaveConfig &config) :
    task::WhoTask(name),
    m_config(config),
    m_event_queue(xQueueCreate(EVENT_QUEUE_SIZE, sizeof(event_t))),
    m_state_mutex(xSemaphoreCreateMutex()),
    m_i2c_handle(nullptr),
    m_i2c_inited(false),
    m_receive_parser(),
    m_tx_data(),
    m_tx_packet_count(0),
    m_sent_packet_count(0)
{
    assert(m_event_queue);
    assert(m_state_mutex);
}

WhoI2CSlaveTask::~WhoI2CSlaveTask()
{
    deinit_i2c();
    vQueueDelete(m_event_queue);
    vSemaphoreDelete(m_state_mutex);
}

void WhoI2CSlaveTask::set_receive_parser(const receive_parser_t &parser)
{
    xSemaphoreTake(m_state_mutex, portMAX_DELAY);
    m_receive_parser = parser;
    xSemaphoreGive(m_state_mutex);
}

bool WhoI2CSlaveTask::set_tx_data(const uint8_t *data, size_t len)
{
    std::vector<uint8_t> packed;
    if (!protocol::pack_data_payload(data,
                                     len,
                                     I2C_PACKET_SIZE,
                                     std::numeric_limits<uint16_t>::max(),
                                     packed)) {
        return false;
    }

    const size_t packet_count = (packed.size() + I2C_PACKET_SIZE - 1) / I2C_PACKET_SIZE;
    packed.resize(packet_count * I2C_PACKET_SIZE, 0);

    xSemaphoreTake(m_state_mutex, portMAX_DELAY);
    m_tx_data = std::move(packed);
    m_tx_packet_count = packet_count;
    m_sent_packet_count = 0;
    xSemaphoreGive(m_state_mutex);
    return true;
}

void WhoI2CSlaveTask::handle_pause()
{
    xEventGroupSetBits(m_event_group, TASK_PAUSED);
    EventBits_t pause_event_bits =
        xEventGroupWaitBits(m_event_group, TASK_RESUME | TASK_STOP, pdTRUE, pdFALSE, portMAX_DELAY);
    if (pause_event_bits & TASK_STOP) {
        xEventGroupSetBits(m_event_group, TASK_STOP);
    }
}

esp_err_t WhoI2CSlaveTask::init_i2c()
{
    if (m_i2c_inited) {
        return ESP_OK;
    }
    ESP_RETURN_ON_FALSE(m_config.buffer_size > 0 && m_config.buffer_size <= RX_DATA_MAX_SIZE,
                        ESP_ERR_INVALID_ARG,
                        TAG,
                        "buffer size must be between 1 and %u",
                        static_cast<unsigned>(RX_DATA_MAX_SIZE));

    i2c_slave_config_t config = {};
    config.i2c_port = m_config.port;
    config.sda_io_num = m_config.sda_io_num;
    config.scl_io_num = m_config.scl_io_num;
    config.clk_source = I2C_CLK_SRC_DEFAULT;
    config.send_buf_depth = m_config.buffer_size;
    config.receive_buf_depth = m_config.buffer_size;
    config.slave_addr = m_config.slave_addr;
    config.addr_bit_len = I2C_ADDR_BIT_LEN_7;
    config.flags.enable_internal_pullup = true;

    i2c_slave_event_callbacks_t callbacks = {};
    callbacks.on_receive = on_receive;
    callbacks.on_request = on_request;
    esp_err_t err = bsp_i2c_slave_init(&config, &callbacks, this, &m_i2c_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "init failed: %s", esp_err_to_name(err));
        m_i2c_handle = nullptr;
        return err;
    }

    m_i2c_inited = true;
    return ESP_OK;
}

void WhoI2CSlaveTask::deinit_i2c()
{
    if (!m_i2c_inited && !m_i2c_handle) {
        return;
    }

    if (m_i2c_handle) {
        if (bsp_i2c_slave_deinit(m_i2c_handle) == ESP_OK) {
            m_i2c_handle = nullptr;
        }
    }
    m_i2c_inited = false;
}

bool WhoI2CSlaveTask::on_receive(i2c_slave_dev_handle_t handle,
                                 const i2c_slave_rx_done_event_data_t *event,
                                 void *user_data)
{
    (void)handle;

    if (!event || event->length == 0) {
        return false;
    }

    auto *self = static_cast<WhoI2CSlaveTask *>(user_data);
    event_t task_event = {};
    task_event.type = Event::RX;
    task_event.len = event->length > RX_DATA_MAX_SIZE ? RX_DATA_MAX_SIZE : event->length;
    memcpy(task_event.data, event->buffer, task_event.len);
    BaseType_t task_woken = pdFALSE;
    xQueueSendFromISR(self->m_event_queue, &task_event, &task_woken);
    return task_woken == pdTRUE;
}

bool WhoI2CSlaveTask::on_request(i2c_slave_dev_handle_t handle,
                                 const i2c_slave_request_event_data_t *event,
                                 void *user_data)
{
    (void)handle;
    (void)event;

    auto *self = static_cast<WhoI2CSlaveTask *>(user_data);
    event_t task_event = {};
    task_event.type = Event::TX;
    BaseType_t task_woken = pdFALSE;
    xQueueSendFromISR(self->m_event_queue, &task_event, &task_woken);
    return task_woken == pdTRUE;
}

void WhoI2CSlaveTask::clear_tx_data()
{
    xSemaphoreTake(m_state_mutex, portMAX_DELAY);
    m_tx_data.clear();
    m_tx_packet_count = 0;
    m_sent_packet_count = 0;
    xSemaphoreGive(m_state_mutex);
}

void WhoI2CSlaveTask::handle_receive_event(const uint8_t *data, size_t len)
{
    esp_err_t err = i2c_slave_reset_tx_fifo(m_i2c_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "reset TX FIFO failed: %s", esp_err_to_name(err));
        return;
    }

    clear_tx_data();

    xSemaphoreTake(m_state_mutex, portMAX_DELAY);
    auto parser = m_receive_parser;
    xSemaphoreGive(m_state_mutex);
    if (!parser) {
        ESP_LOGW(TAG, "received %u bytes without a parser", static_cast<unsigned>(len));
        return;
    }
    if (!parser(data, len)) {
        clear_tx_data();
        ESP_LOGW(TAG, "received data parse failed");
    }
}

void WhoI2CSlaveTask::handle_request_event()
{
    uint8_t packet[I2C_PACKET_SIZE] = {};

    xSemaphoreTake(m_state_mutex, portMAX_DELAY);
    if (m_sent_packet_count < m_tx_packet_count) {
        memcpy(packet, m_tx_data.data() + m_sent_packet_count * I2C_PACKET_SIZE, I2C_PACKET_SIZE);
    }
    m_sent_packet_count++;
    xSemaphoreGive(m_state_mutex);

    uint32_t write_len = 0;
    esp_err_t err = i2c_slave_write(m_i2c_handle, packet, sizeof(packet), &write_len, 1000);
    if (err != ESP_OK || write_len != sizeof(packet)) {
        ESP_LOGW(TAG, "write failed: %s, written=%lu", esp_err_to_name(err), write_len);
    }
}

void WhoI2CSlaveTask::task()
{
    if (init_i2c() != ESP_OK) {
        xEventGroupSetBits(m_event_group, TASK_STOPPED);
        vTaskDelete(NULL);
        return;
    }

    while (true) {
        EventBits_t event_bits = xEventGroupWaitBits(m_event_group, TASK_PAUSE | TASK_STOP, pdTRUE, pdFALSE, 0);
        if (event_bits & TASK_STOP) {
            break;
        } else if (event_bits & TASK_PAUSE) {
            handle_pause();
            continue;
        }

        event_t event = {};
        if (xQueueReceive(m_event_queue, &event, IDLE_DELAY) == pdTRUE) {
            if (event.type == Event::RX) {
                handle_receive_event(event.data, event.len);
            } else {
                handle_request_event();
            }
        }
    }

    deinit_i2c();
    xEventGroupSetBits(m_event_group, TASK_STOPPED);
    vTaskDelete(NULL);
}

void WhoI2CSlaveTask::cleanup()
{
    xQueueReset(m_event_queue);
    clear_tx_data();
}

} // namespace communicate
} // namespace who
