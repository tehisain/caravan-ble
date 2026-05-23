# `alerts` — Battery-Critical Alert Rules Design

**Date:** 2026-05-23
**Status:** Approved, ready for plan

## Overview

`power_monitor` produces raw BLE telemetry. `power_state` derives a
coherent enum + flags. This module sits one level up again and
evaluates a small set of **battery-damage-prevention** rules over those
two data sources, raising and clearing alerts with hysteresis. Output
is log-only in v1; future modules (SMS, BLE GATT, T-Encoder) will
consume the same alert state without further changes to this module.

## Goals

- Detect SoC and cell-temperature conditions that can damage a
  LiFePO4 pack, raising a single alert per condition.
- Built-in hysteresis: rules don't bounce on slow-moving readings.
- Stale data freezes alert state — we never *clear* an alert just
  because the BMS went offline.
- Cheap, observable: 1-Hz evaluation, transitions logged, steady state
  silent.

## Non-goals

- Severity tiers (`INFO` / `WARN` / `CRIT`). All v1 alerts are flat.
  We'll add severity when multiple output sinks exist.
- Persistence across reboots. Alert state is in-memory only.
- Output sinks beyond logging.
- Acknowledge / snooze mechanisms.
- Composite or operational alerts (no-charge-when-shore, BMS-stale,
  MOSFET-temp). Deferred to a follow-up bundle once we have a sink
  beyond logging.

## Files

```
c6-hub/main/
├── alerts.h            NEW — public alert_id enum + alert_state_t + API
├── alerts.c            NEW — rules + 1-Hz task + in-memory state
├── CMakeLists.txt      MODIFY — add alerts.c to SRCS
└── main.c              MODIFY — call alerts_init after power_state_init
```

## Public API (`alerts.h`)

```c
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
    int64_t   first_active_ts_ms;   // when current active period began
    int64_t   last_change_ts_ms;
} alert_state_t;

// Spawn the alerts task. Call once, after power_state_init.
esp_err_t alerts_init(void);

// Copy the current state of one alert into `out`. Always succeeds
// (returns ESP_OK) — alert_state_t.active is the meaningful field.
esp_err_t alerts_get(alert_id_t id, alert_state_t *out);

// Human-readable name for logging or future UI display.
const char *alerts_name(alert_id_t id);
```

## Rule Details

Each rule has separate **enter** and **exit** predicates. Once active,
only the exit predicate clears it.

| Alert | Enter | Exit |
|---|---|---|
| `LOW_SOC` | `battery.soc_pct < 20` | `battery.soc_pct > 25` |
| `CRITICAL_SOC` | `battery.soc_pct < 10` | `battery.soc_pct > 15` |
| `OVER_TEMP_CELL` | `battery.cell_temp_c > 50` | `battery.cell_temp_c < 45` |
| `UNDER_TEMP_CHARGE` | `battery.cell_temp_c < 0` AND `power_state.state == CHARGING` | `battery.cell_temp_c > 5` |

### Why these rules

- **LiFePO4 chemistry** can be permanently damaged if charged below
  0 °C. UNDER_TEMP_CHARGE is the most urgent rule of the four; it's
  the one that justifies the whole module.
- **OVER_TEMP_CELL** at 50 °C is the conservative ceiling for cycle
  life on cheap LiFePO4 cells.
- **LOW_SOC** at 20% is the conventional reserve threshold; below this
  we should be conserving load.
- **CRITICAL_SOC** at 10% is "shut down everything non-essential."

### Hysteresis ranges

5 percentage points on SoC, 5 °C on temperatures. Wider than the
expected sample-to-sample noise in the BLE telemetry but narrower than
real-world drift over minutes — so transitions are crisp, not bouncy.

## Operation

### `alerts_init()`

1. Zero the in-memory state array (`s_state[ALERT_COUNT]`).
2. Spawn `alerts_task` with 2.5 KiB stack at priority 4.

### `alerts_task`

```
loop forever:
    sleep 1 s
    if power_monitor_get_battery(&b) != ESP_OK: continue (skip cycle)
    if power_state_get(&ps) != ESP_OK:          continue (skip cycle)

    for id in 0..ALERT_COUNT:
        was_active = s_state[id].active
        if was_active:
            if exit_predicate(id, b, ps):
                clear(id, "CLEAR", b)
        else:
            if enter_predicate(id, b, ps):
                raise(id, "ENTER", b)
```

`raise` and `clear` are tiny helpers that update the state under the
spinlock and emit a single log line.

### Log format

```
al: ENTER  LOW_SOC           SoC=18
al: CLEAR  LOW_SOC           SoC=27
al: ENTER  CRITICAL_SOC      SoC=8
al: ENTER  OVER_TEMP_CELL    T=51
al: CLEAR  OVER_TEMP_CELL    T=44
al: ENTER  UNDER_TEMP_CHARGE T=-2 (charging)
al: CLEAR  UNDER_TEMP_CHARGE T=6
```

The SoC and temperature values are the *current* readings at the
transition moment (not the threshold itself).

### Concurrency

Same `portMUX_TYPE` pattern as `power_state`. Single writer (the task),
multiple readers (getters). Critical sections are pure struct copies.

## Acceptance Criteria

1. **Boot idle** (SoC 100, cell ~11 °C, FLOAT) — no `al:` lines for at
   least 5 minutes.
2. **Simulated low-SoC** — throwaway test: temporarily lower
   `LOW_SOC_ENTER` to `99` in a branch; expect
   `al: ENTER LOW_SOC SoC=100` within 2 s. Revert the change.
3. **Simulated transition** — lower OVER_TEMP_CELL enter to `0` in the
   same throwaway branch, observe `al: ENTER OVER_TEMP_CELL T=11`,
   then revert.
4. **Stale BMS** (front-button BMS off): the task `continue`s each
   cycle, alert states do not change. Verified by observing no
   spurious `CLEAR` lines while BMS is offline.
5. **Heap delta** ≤ 4 KiB after 1 h.

## Risks & Open Questions

- **Stale data freezes alerts.** This is by design: an alert raised
  before a BMS dropout should remain raised until we have fresh data
  confirming it's clear. Tradeoff: if the BMS goes offline *while* a
  condition is improving, the alert remains active until BMS returns.
  Acceptable.
- **Charge-state oscillation.** UNDER_TEMP_CHARGE depends on
  `power_state.state == CHARGING`. If the BMS reports current that
  oscillates around `0.3 A` (the IDLE_THRESHOLD_A in `power_state`),
  the enter-predicate could flap. Mitigation: the 5 °C exit hysteresis
  on the temperature side means the alert won't oscillate; only the
  *initial* enter could flap. Acceptable for v1.
- **Thresholds aren't tunable at runtime.** Compile-time defines. If
  field tuning becomes important, lift to Kconfig later.

## Out of Scope (Future)

- Severity tiers + per-severity output routing.
- Composite rules: No-charge-when-shore (shore_active for >2 min but
  power_state != CHARGING).
- Operational rules: BMS-stale (no readings >2 min), MOSFET-over-temp.
- NVS persistence so a critical alert survives a power-cycle.
- SMS dispatch via SIM7600.
- BLE GATT exposure of active alerts to phone.
- T-Encoder display of active alerts.
- KLIPPBOK siren trigger via Matter on CRITICAL_SOC + offgrid.
