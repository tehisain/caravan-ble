#pragma once

#include "esp_err.h"

// Check GitHub Releases for a newer firmware than what's running. If the
// release tag differs from the running version, download the matching
// asset via esp_https_ota and esp_restart on success (does not return).
// Returns ESP_OK if the check completed without applying an update;
// error code on transport / parse / OTA failure.
esp_err_t ota_check_and_update(void);

// Mark the running image as valid so the bootloader stops considering
// rollback. Safe to call repeatedly. No-op unless the image is in
// PENDING_VERIFY state (i.e. we just OTA'd into it).
void ota_mark_valid(void);
