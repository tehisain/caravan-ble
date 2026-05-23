# `power_state` — Derived Power State Design

**Date:** 2026-05-23
**Status:** Approved, ready for plan

## Overview

`power_monitor` produces raw telemetry from three BLE devices. This
module sits one level up and turns those streams into a small,
opinionated state that downstream consumers (alerts, T-Encoder display,
phone GATT, future caravan-mode state machine) can branch on without
re-deriving the same conditions everywhere.

This is **not** the caravan-mode state machine that produces
OFFGRID / CAMPSITE / TOWING / ARRIVED — that bigger state machine will
consume `power_state` as one of several inputs once shore-detect GPIO,
car-detect GPIO, GPS, and pitch/roll are wired up.

## Goals

- Single source of truth for "what's the power system doing right now".
- Cheap to query — readers don't pay the cost of re-deriving from the
  raw streams.
- Conservative — when inputs are stale, the answer is honestly
  `UNKNOWN` rather than a stale value.

## Non-goals

- Threshold-based alerts (low SoC, over-temp, no-charge-when-shore).
- FAULT detection: we don't yet expose Victron `charger_error`. The
  enum has a `POWER_STATE_FAULT` value reserved but it isn't raised
  in v1.
- Persistence across reboots. The state is recomputed fresh each boot.

## Files

```
c6-hub/main/
├── power_state.h           NEW — public types + getter
├── power_state.c           NEW — derivation logic + background task
├── CMakeLists.txt          MODIFY — add SRC
└── main.c                  MODIFY — call power_state_init after power_monitor_init
```

## Public API (`power_state.h`)

```c
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

typedef enum {
    POWER_STATE_UNKNOWN,        // any input reading is stale
    POWER_STATE_CHARGING,       // net positive current into battery
    POWER_STATE_DISCHARGING,    // net negative current from battery
    POWER_STATE_FLOAT,          // SoC near 100% AND current near zero
    POWER_STATE_FAULT,          // (reserved — never raised in v1)
} power_state_t;

typedef struct {
    bool          valid;            // false until first derivation
    int64_t       last_eval_ts_ms;
    power_state_t state;
    bool          shore_active;     // charger.output_current > 0.3 A
    bool          solar_active;     // solar.charging_current > 0.3 A
    float         net_current_a;    // signed; mirrors bms.current_a
    uint8_t       soc_pct;          // mirrors bms.soc_pct
} power_state_reading_t;

// Spawn the background derivation task. Call once, after
// power_monitor_init.
esp_err_t power_state_init(void);

// Copy the latest derivation into `out`. Returns ESP_OK when the
// cached reading exists AND its last_eval_ts_ms is within 5 s.
esp_err_t power_state_get(power_state_reading_t *out);
```

## Derivation rules

Evaluated once per second by the background task.

### Enum (`state`)

| Condition | → state |
|---|---|
| Any of `power_monitor_get_battery/solar/charger` returns `NOT_FOUND` | `POWER_STATE_UNKNOWN` |
| `bms.current_a > +0.3 A` | `POWER_STATE_CHARGING` |
| `bms.current_a < −0.3 A` | `POWER_STATE_DISCHARGING` |
| `bms.state == 4` (BMS reports "full") OR (`abs(bms.current_a) ≤ 0.3 A` AND `bms.soc_pct ≥ 95`) | `POWER_STATE_FLOAT` |
| Otherwise (idle but not near full — uncommon transient) | `POWER_STATE_FLOAT` |

### Flags (independent of state)

| Flag | Rule |
|---|---|
| `shore_active` | `charger.output_current > 0.3 A` |
| `solar_active` | `solar.charging_current > 0.3 A` |

### Threshold rationale

`0.3 A` is empirically the noise floor we observed in the live readings
(both INA-side rounding and Victron's reported precision). Anything
below that should be treated as idle.

## Operation

### `power_state_init()`

1. Validate that `power_monitor` has been initialized (best-effort: call
   `power_monitor_get_battery()` once and ignore the return — it just
   confirms the module is linked).
2. Allocate the internal cache + spinlock.
3. Spawn `power_state_task` with a 2.5 KiB stack at priority 4.

### `power_state_task`

```
loop forever:
    sleep 1 s
    battery_reading_t b; if get_battery(&b) != OK → mark stale
    solar_reading_t s;   if get_solar(&s) != OK   → mark stale
    charger_reading_t c; if get_charger(&c) != OK → mark stale

    if any stale:
        new_state = UNKNOWN
        shore_active = solar_active = false
        net_current_a = 0; soc_pct = 0
    else:
        net_current_a = b.current_a
        soc_pct       = b.soc_pct
        shore_active  = c.output_current_a > 0.3
        solar_active  = s.charging_current_a > 0.3
        new_state     = derive (see table above)

    portENTER_CRITICAL
        prev = cache
        cache = { valid: true, last_eval_ts: now, state: new_state, ... }
    portEXIT_CRITICAL

    if prev.state != new_state OR prev.shore_active != shore_active
       OR prev.solar_active != solar_active:
        ESP_LOGI("ps", "<STATE> shore=<0/1> solar=<0/1> i=±X.XX SoC=YY")
```

Log only on transitions of `state`, `shore_active`, or `solar_active`.
SoC and current are part of the log line for context but don't trigger
logs by themselves (they'd drift constantly).

### `power_state_get()`

Spinlock-protected copy. Returns `ESP_ERR_NOT_FOUND` if
`!cache.valid || (now − last_eval_ts_ms > 5000)`.

## Acceptance Criteria

1. **Boot idle** — caravan on float, all three BLE streams alive:
   Within ~3 s of `power_state_init()`, log shows:
   `ps: FLOAT shore=0 solar=0 i=+0.00 SoC=100`
   and stays silent (no transition spam over 5 minutes).
2. **Shore plug-in** — within 2 s after the IP65 starts pushing:
   `ps: FLOAT shore=1 solar=0 i=+X.XX SoC=YY`
   (`state` may still be FLOAT if SoC stays high — but the flag flips
   and that's what triggers the log).
3. **Shore unplug** — within 2 s:
   `ps: FLOAT shore=0 solar=0 …`.
4. **BMS BLE off** (front button) — within ~60 s (BMS reading freshness
   window) — `ps: UNKNOWN shore=0 solar=0 …`.
5. **Heap delta** — after 1 h of running with this module, free heap
   ≤ 4 KiB lower than at `power_state_init` return.

## Risks & Open Questions

- **`bms.state == 4` definition.** The PowerQueen protocol doc says
  `4 = full`. We rely on this for the FLOAT rule. If the BMS firmware
  ever changes the enum, we fall through to the `SoC ≥ 95 && idle`
  branch — still correct.
- **Solar charging at very low currents.** A `solar.charging_current`
  of 0.2 A on a cloudy morning would not flip `solar_active`. That's
  fine — at that flow the panel isn't meaningfully contributing.
  Consumers wanting "panel reports any production" can read
  `solar.solar_power_w > 0` directly from `power_monitor`.

## Out of Scope / Future

- FAULT raising once `power_monitor` exposes Victron `charger_error`.
- Threshold-based alerts (low SoC, no-charge-when-shore-active,
  over-temperature) — a separate `alerts.c` module.
- Caravan-level state machine (OFFGRID/CAMPSITE/TOWING/ARRIVED) that
  consumes this module's output plus other inputs.
- Phone GATT exposure of `power_state` — separate `ble_server.c`.
