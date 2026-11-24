#pragma once

#include "driver/gpio.h"
#include "driver/i2c_slave.h"
#include "esp_err.h"
#include "who_task.hpp"
#include <functional>
#include <string>
#include <vector>

namespace who {
namespace communicate {

struct WhoI2CSlaveConfig {
    i2c_port_num_t port = I2C_NUM_1;
    gpio_num_t scl_io_num = GPIO_NUM_47;
    gpio_num_t sda_io_num = GPIO_NUM_46;
    uint16_t slave_addr = 0x54;
    size_t buffer_size = 256;
};

class WhoI2CSlaveTask : public task::WhoTask {
public:
    using receive_parser_t = std::function<bool(const uint8_t *data, size_t len)>;

    WhoI2CSlaveTask(const std::string &name, const WhoI2CSlaveConfig &config = {});
    ~WhoI2CSlaveTask();

    void set_receive_parser(const receive_parser_t &parser);
    bool set_tx_data(const uint8_t *data, size_t len);

private:
    enum class Event : uint8_t {
        RX,
        TX,
    };

    static constexpr size_t I2C_PACKET_SIZE = 32;
    static constexpr size_t RX_DATA_MAX_SIZE = 256;
    static constexpr size_t EVENT_QUEUE_SIZE = 16;
    static constexpr TickType_t IDLE_DELAY = pdMS_TO_TICKS(10);

    struct event_t {
        Event type;
        size_t len;
        uint8_t data[RX_DATA_MAX_SIZE];
    };

    void task() override;
    void cleanup() override;
    void handle_pause();
    esp_err_t init_i2c();
    void deinit_i2c();
    void handle_receive_event(const uint8_t *data, size_t len);
    void handle_request_event();
    void clear_tx_data();
    static bool on_receive(i2c_slave_dev_handle_t handle,
                           const i2c_slave_rx_done_event_data_t *event,
                           void *user_data);
    static bool on_request(i2c_slave_dev_handle_t handle,
                           const i2c_slave_request_event_data_t *event,
                           void *user_data);

    WhoI2CSlaveConfig m_config;
    QueueHandle_t m_event_queue;
    SemaphoreHandle_t m_state_mutex;
    i2c_slave_dev_handle_t m_i2c_handle;
    bool m_i2c_inited;
    receive_parser_t m_receive_parser;
    std::vector<uint8_t> m_tx_data;
    size_t m_tx_packet_count;
    size_t m_sent_packet_count;
};

} // namespace communicate
} // namespace who
