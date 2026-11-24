#pragma once

#include "esp_err.h"

/** Start SoftAP networking, mDNS, and NetBIOS services. */
esp_err_t network_service_start(void);
