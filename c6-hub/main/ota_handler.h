#pragma once

#include "esp_err.h"

// Check GitHub Releases for a newer firmware than what's running.
// In this task: log the result only. Actual download/flash arrives in Task 4.
// Returns ESP_OK if the check completed (regardless of whether an update
// would apply); error code on transport failure.
esp_err_t ota_check_and_update(void);
