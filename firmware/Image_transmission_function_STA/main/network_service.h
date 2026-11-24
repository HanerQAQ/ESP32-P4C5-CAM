#pragma once

#include "esp_err.h"

/** Initialize the TCP/IP stack and start Wi-Fi (STA steady state, SoftAP on demand). */
esp_err_t network_service_start(void);
