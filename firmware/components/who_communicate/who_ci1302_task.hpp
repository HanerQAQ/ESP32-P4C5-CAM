#pragma once

#include "who_task.hpp"

#include <cstddef>
#include <cstdint>
#include <string>

namespace who {
namespace communicate {

/**
 * @brief CI1302 communication and reset-button task.
 *
 * This task owns the CI1302 UART receive/transmit path and monitors the two
 * active-low reset buttons. It follows the common WhoTask lifecycle and is
 * provided as a singleton because the CI1302 UART is a board-wide resource.
 */
class WhoCi1302Task : public task::WhoTask {
public:
    /**
     * @brief Get the board-wide CI1302 task instance.
     *
     * @return Pointer to the CI1302 task instance.
     */
    static WhoCi1302Task *get_instance();

    /**
     * @brief Queue data for transmission to the CI1302.
     *
     * @param data Data to transmit.
     * @param len Number of bytes to transmit.
     * @param timeout Queue wait timeout.
     * @return true if all data was queued, otherwise false.
     */
    bool send(const uint8_t *data, size_t len, TickType_t timeout = 0);

private:
    static constexpr int64_t BUTTON_HOLD_TIME_US = 1000 * 1000;
    static constexpr int64_t RESET_TIME_US = 100 * 1000;
    static constexpr TickType_t IDLE_DELAY = pdMS_TO_TICKS(1);
    static constexpr size_t UART_BUFFER_SIZE = 64;
    static constexpr size_t TX_CHUNK_SIZE = 64;
    static constexpr UBaseType_t TX_QUEUE_SIZE = 8;

    struct tx_msg_t {
        size_t len;
        uint8_t data[TX_CHUNK_SIZE];
    };

    explicit WhoCi1302Task(const std::string &name);
    ~WhoCi1302Task();
    WhoCi1302Task(const WhoCi1302Task &) = delete;
    WhoCi1302Task &operator=(const WhoCi1302Task &) = delete;

    void task() override;
    void cleanup() override;
    void handle_pause();
    esp_err_t init_hardware();
    void deinit_hardware();
    void process_buttons(int64_t now);
    void process_rx_byte(uint8_t byte);
    void drain_uart_rx();
    void drain_uart_tx();

    QueueHandle_t m_tx_queue;
    bool m_hardware_inited;
    uint8_t m_rx_frame[5];
    size_t m_rx_index;
    int64_t m_both_pressed_at;
    int64_t m_reset_started_at;
    bool m_reset_triggered;
    bool m_reset_active;
};

} // namespace communicate
} // namespace who
