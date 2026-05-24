#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

typedef enum {
    ALERT_LOW_SOC,            // SoC < 20%
    ALERT_CRITICAL_SOC,       // SoC < 10%
    ALERT_OVER_TEMP_CELL,     // cell_temp > 50°C
    ALERT_UNDER_TEMP_CHARGE,  // cell_temp < 0°C while CHARGING
    ALERT_COUNT,              // sentinel — array size, not an alert
} alert_id_t;

typedef struct {
    bool      active;
    int64_t   first_active_ts_ms;
    int64_t   last_change_ts_ms;
} alert_state_t;

// Spawn the alerts task. Call once, after power_state_init.
esp_err_t alerts_init(void);

// Copy the current state of one alert into `out`. Always succeeds.
esp_err_t alerts_get(alert_id_t id, alert_state_t *out);

// Human-readable name for logging.
const char *alerts_name(alert_id_t id);
