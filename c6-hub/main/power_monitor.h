#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"

typedef struct {
    bool      valid;
    int64_t   last_seen_ts_ms;
    float     pack_voltage_v;
    float     terminal_voltage_v;
    float     current_a;
    uint8_t   soc_pct;
    uint8_t   soh_pct;
    int8_t    cell_temp_c;
    int8_t    mosfet_temp_c;
    float     remaining_ah;
    float     capacity_ah;
    uint32_t  cycles;
    uint8_t   state;          // 0=idle 1=charging 2=discharging 4=full
    uint8_t   cell_count;
    uint16_t  cell_mv[16];    // unpopulated cells are zero
} battery_reading_t;

typedef struct {
    bool      valid;
    int64_t   last_seen_ts_ms;
    float     battery_voltage_v;
    float     charging_current_a;
    int16_t   solar_power_w;
    uint16_t  yield_today_wh;
    uint8_t   charge_state;
} solar_reading_t;

typedef struct {
    bool      valid;
    int64_t   last_seen_ts_ms;
    float     output_voltage_v;
    float     output_current_a;
    uint8_t   charge_state;
} charger_reading_t;

esp_err_t power_monitor_init(void);
esp_err_t power_monitor_get_battery(battery_reading_t *out);
esp_err_t power_monitor_get_solar(solar_reading_t *out);
esp_err_t power_monitor_get_charger(charger_reading_t *out);
