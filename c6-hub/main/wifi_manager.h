#pragma once

#include <stdint.h>
#include "esp_err.h"

// Initialize netif/event-loop/Wi-Fi, start STA association with the SSID
// from Kconfig. Returns ESP_ERR_INVALID_STATE if no SSID is configured
// (in which case OTA should be skipped entirely).
esp_err_t wifi_manager_init(void);

// Block until associated (got IP) or the timeout elapses. Returns ESP_OK
// on success, ESP_ERR_TIMEOUT otherwise.
esp_err_t wifi_manager_wait_connected(uint32_t timeout_ms);
