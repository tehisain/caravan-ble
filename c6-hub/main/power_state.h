#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

typedef enum {
    POWER_STATE_UNKNOWN,        // any input reading is stale
    POWER_STATE_CHARGING,       // net positive into battery
    POWER_STATE_DISCHARGING,    // net negative from battery
    POWER_STATE_FLOAT,          // SoC near 100% AND current near zero
    POWER_STATE_FAULT,          // reserved — never raised in v1
} power_state_t;

typedef struct {
    bool          valid;
    int64_t       last_eval_ts_ms;
    power_state_t state;
    bool          shore_active;
    bool          solar_active;
    float         net_current_a;
    uint8_t       soc_pct;
} power_state_reading_t;

// Spawn the background derivation task. Call once, after power_monitor_init.
esp_err_t power_state_init(void);

// Copy the latest derivation into `out`. Returns ESP_OK when the cached
// reading exists AND its last_eval_ts_ms is within 5 s.
esp_err_t power_state_get(power_state_reading_t *out);
