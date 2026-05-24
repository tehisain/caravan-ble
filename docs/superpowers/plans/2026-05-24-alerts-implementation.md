# `alerts` Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add the four battery-critical alert rules with hysteresis as a single small module, wired into the existing power_monitor + power_state stack and shipped via OTA.

**Architecture:** Two files. A 1-Hz FreeRTOS task evaluates four hysteretic rules against fresh battery + power_state readings; stale data freezes alert state. Public API exposes per-alert get + name lookup. Single writer (task), readers via spinlock-protected copy — same pattern as `power_state`.

**Tech Stack:** ESP-IDF v6.0, FreeRTOS, portMUX spinlock, ESP_LOG.

**Spec reference:** `docs/superpowers/specs/2026-05-23-alerts-design.md`.

---

## File Structure

```
c6-hub/main/
├── CMakeLists.txt      MODIFY — add alerts.c to SRCS
├── main.c              MODIFY — call alerts_init after power_state_init
├── alerts.h            NEW    — alert_id_t enum + alert_state_t + API
└── alerts.c            NEW    — rules + 1-Hz task + cache
```

---

## Key Decisions Locked In (from spec)

| Decision | Value |
|---|---|
| Rule set | LOW_SOC, CRITICAL_SOC, OVER_TEMP_CELL, UNDER_TEMP_CHARGE |
| Hysteresis SoC | 5 pp (enter < N, exit > N+5) |
| Hysteresis temp | 5 °C |
| Cadence | 1 Hz |
| Stale handling | Skip cycle — never auto-clear |
| Log policy | Single line on ENTER and CLEAR transitions |
| Thresholds | Compile-time `#define` (not Kconfig in v1) |

---

## Task 1 — `alerts` module + integration

**Files:**
- Create: `c6-hub/main/alerts.h`
- Create: `c6-hub/main/alerts.c`
- Modify: `c6-hub/main/CMakeLists.txt`
- Modify: `c6-hub/main/main.c`

- [ ] **Step 1: Write `alerts.h`**

Create `c6-hub/main/alerts.h`:

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
    int64_t   first_active_ts_ms;
    int64_t   last_change_ts_ms;
} alert_state_t;

// Spawn the alerts task. Call once, after power_state_init.
esp_err_t alerts_init(void);

// Copy the current state of one alert into `out`. Always succeeds.
esp_err_t alerts_get(alert_id_t id, alert_state_t *out);

// Human-readable name for logging.
const char *alerts_name(alert_id_t id);
```

- [ ] **Step 2: Write `alerts.c`**

Create `c6-hub/main/alerts.c`:

```c
#include "alerts.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "power_monitor.h"
#include "power_state.h"

static const char *TAG = "al";

// Hysteresis thresholds.
#define LOW_SOC_ENTER             20
#define LOW_SOC_EXIT              25
#define CRITICAL_SOC_ENTER        10
#define CRITICAL_SOC_EXIT         15
#define OVER_TEMP_CELL_ENTER      50
#define OVER_TEMP_CELL_EXIT       45
#define UNDER_TEMP_CHARGE_ENTER   0
#define UNDER_TEMP_CHARGE_EXIT    5

#define EVAL_INTERVAL_MS          1000

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static alert_state_t s_state[ALERT_COUNT];

static const char *s_names[ALERT_COUNT] = {
    [ALERT_LOW_SOC]            = "LOW_SOC",
    [ALERT_CRITICAL_SOC]       = "CRITICAL_SOC",
    [ALERT_OVER_TEMP_CELL]     = "OVER_TEMP_CELL",
    [ALERT_UNDER_TEMP_CHARGE]  = "UNDER_TEMP_CHARGE",
};

static int64_t now_ms(void)
{
    return esp_timer_get_time() / 1000;
}

static bool enter_predicate(alert_id_t id,
                            const battery_reading_t *b,
                            const power_state_reading_t *ps)
{
    switch (id) {
        case ALERT_LOW_SOC:
            return b->soc_pct < LOW_SOC_ENTER;
        case ALERT_CRITICAL_SOC:
            return b->soc_pct < CRITICAL_SOC_ENTER;
        case ALERT_OVER_TEMP_CELL:
            return b->cell_temp_c > OVER_TEMP_CELL_ENTER;
        case ALERT_UNDER_TEMP_CHARGE:
            return b->cell_temp_c < UNDER_TEMP_CHARGE_ENTER
                && ps->state == POWER_STATE_CHARGING;
        case ALERT_COUNT:
            return false;
    }
    return false;
}

static bool exit_predicate(alert_id_t id,
                           const battery_reading_t *b,
                           const power_state_reading_t *ps)
{
    (void)ps;
    switch (id) {
        case ALERT_LOW_SOC:
            return b->soc_pct > LOW_SOC_EXIT;
        case ALERT_CRITICAL_SOC:
            return b->soc_pct > CRITICAL_SOC_EXIT;
        case ALERT_OVER_TEMP_CELL:
            return b->cell_temp_c < OVER_TEMP_CELL_EXIT;
        case ALERT_UNDER_TEMP_CHARGE:
            return b->cell_temp_c > UNDER_TEMP_CHARGE_EXIT;
        case ALERT_COUNT:
            return false;
    }
    return false;
}

static void log_transition(const char *verb, alert_id_t id,
                           const battery_reading_t *b)
{
    if (id == ALERT_OVER_TEMP_CELL || id == ALERT_UNDER_TEMP_CHARGE) {
        ESP_LOGI(TAG, "%-5s %-17s T=%d", verb, s_names[id], b->cell_temp_c);
    } else {
        ESP_LOGI(TAG, "%-5s %-17s SoC=%u", verb, s_names[id], b->soc_pct);
    }
}

static void raise_alert(alert_id_t id, const battery_reading_t *b)
{
    int64_t t = now_ms();
    portENTER_CRITICAL(&s_lock);
    s_state[id].active = true;
    s_state[id].first_active_ts_ms = t;
    s_state[id].last_change_ts_ms = t;
    portEXIT_CRITICAL(&s_lock);
    log_transition("ENTER", id, b);
}

static void clear_alert(alert_id_t id, const battery_reading_t *b)
{
    int64_t t = now_ms();
    portENTER_CRITICAL(&s_lock);
    s_state[id].active = false;
    s_state[id].last_change_ts_ms = t;
    portEXIT_CRITICAL(&s_lock);
    log_transition("CLEAR", id, b);
}

static void alerts_task(void *arg)
{
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(EVAL_INTERVAL_MS));

        battery_reading_t b;
        power_state_reading_t ps;
        if (power_monitor_get_battery(&b) != ESP_OK) continue;
        if (power_state_get(&ps) != ESP_OK) continue;

        for (int id = 0; id < ALERT_COUNT; id++) {
            bool was_active;
            portENTER_CRITICAL(&s_lock);
            was_active = s_state[id].active;
            portEXIT_CRITICAL(&s_lock);

            if (was_active) {
                if (exit_predicate(id, &b, &ps)) clear_alert(id, &b);
            } else {
                if (enter_predicate(id, &b, &ps)) raise_alert(id, &b);
            }
        }
    }
}

esp_err_t alerts_init(void)
{
    portENTER_CRITICAL(&s_lock);
    for (int i = 0; i < ALERT_COUNT; i++) {
        s_state[i] = (alert_state_t){0};
    }
    portEXIT_CRITICAL(&s_lock);

    BaseType_t rc = xTaskCreate(alerts_task, "alerts", 2560, NULL, 4, NULL);
    if (rc != pdPASS) {
        ESP_LOGE(TAG, "task create failed");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t alerts_get(alert_id_t id, alert_state_t *out)
{
    if (id >= ALERT_COUNT) return ESP_ERR_INVALID_ARG;
    portENTER_CRITICAL(&s_lock);
    *out = s_state[id];
    portEXIT_CRITICAL(&s_lock);
    return ESP_OK;
}

const char *alerts_name(alert_id_t id)
{
    if (id >= ALERT_COUNT) return "?";
    return s_names[id];
}
```

- [ ] **Step 3: Add to CMakeLists**

Edit `c6-hub/main/CMakeLists.txt`. Append `"alerts.c"` to the SRCS list:

```cmake
idf_component_register(
    SRCS "main.c" "wifi_manager.c" "ota_handler.c"
         "powerqueen_bms.c" "victron_iadv.c" "power_monitor.c"
         "power_state.c" "alerts.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES
        bt
        nvs_flash
        esp_wifi
        esp_event
        esp_netif
        esp_http_client
        esp_https_ota
        app_update
        spi_flash
        esp_app_format
        esp_partition
        esp_timer
        mbedtls
)
```

- [ ] **Step 4: Hook into `main.c`**

Edit `c6-hub/main/main.c`. Add include after `power_state.h`:

```c
#include "power_state.h"
#include "alerts.h"
```

Add the init call right after `power_state_init()`:

```c
    power_monitor_init();
    power_state_init();
    alerts_init();
```

- [ ] **Step 5: Build, ship, flash**

```bash
cd /Users/maidok/Developer/powerqueen/c6-hub
./build.sh 2>&1 | grep -E "error:|FAILED|c6-hub.bin binary" | head -3
cd build
tar -czf /tmp/c6-hub-flash.tar.gz \
    bootloader/bootloader.bin partition_table/partition-table.bin \
    ota_data_initial.bin c6-hub.bin
scp -q /tmp/c6-hub-flash.tar.gz caravan:~/c6-hub-flash/
ssh caravan 'cd ~/c6-hub-flash && tar -xzf c6-hub-flash.tar.gz && \
    ~/.local/esptool-venv/bin/esptool.py --chip esp32c6 -p /dev/ttyACM0 \
    --before default-reset --after hard-reset write-flash \
    --flash-mode dio --flash-size 16MB --flash-freq 80m \
    0x0 bootloader/bootloader.bin \
    0x8000 partition_table/partition-table.bin \
    0xf000 ota_data_initial.bin \
    0x20000 c6-hub.bin'
```

- [ ] **Step 6: Verify silence on healthy battery**

Capture 60 s of serial:

```bash
ssh caravan 'python3 << "PYEOF"
import serial, sys, time
s = serial.Serial("/dev/ttyACM0", 115200, timeout=0.3)
s.setDTR(False); s.setRTS(True); time.sleep(0.1); s.setRTS(False)
end = time.time() + 60
while time.time() < end:
    line = s.readline()
    if line: sys.stdout.write(line.decode(errors="replace")); sys.stdout.flush()
PYEOF' 2>&1 | grep -aE "pm: |ps: |al: " | head -10
```

Expected:
- `pm: solar v=...`, `pm: charger v=...`, `pm: bms v=...`
- `ps: UNKNOWN ...` then `ps: FLOAT shore=0 solar=0 i=+0.00 SoC=100`
- **No `al:` lines.** Battery is at SoC 100, cell ~11°C — all four rules are quiet.

If a spurious `al: ENTER` line appears, that's a real bug in a threshold — investigate before proceeding.

- [ ] **Step 7: Verify a rule fires (throwaway test)**

Lower `LOW_SOC_ENTER` from `20` to `200` temporarily so it's always true:

```c
#define LOW_SOC_ENTER             200
```

Build + flash via the same Step 5 commands. Capture 10 s of serial:

```bash
ssh caravan 'python3 << "PYEOF"
import serial, sys, time
s = serial.Serial("/dev/ttyACM0", 115200, timeout=0.3)
s.setDTR(False); s.setRTS(True); time.sleep(0.1); s.setRTS(False)
end = time.time() + 25
while time.time() < end:
    line = s.readline()
    if line: sys.stdout.write(line.decode(errors="replace")); sys.stdout.flush()
PYEOF' 2>&1 | grep -aE "al: " | head -5
```

Expected within ~15 s of boot:

```
al: ENTER LOW_SOC           SoC=100
```

Then revert `LOW_SOC_ENTER` back to `20`. Build + flash one more time to confirm the alert clears:

```
al: CLEAR LOW_SOC           SoC=100
```

(The clear fires because `100 > LOW_SOC_EXIT (25)`.)

- [ ] **Step 8: Commit**

```bash
cd /Users/maidok/Developer/powerqueen
git add c6-hub/main/alerts.h c6-hub/main/alerts.c \
        c6-hub/main/CMakeLists.txt c6-hub/main/main.c
git commit -m "c6-hub: alerts — four battery-critical hysteretic rules"
```

---

## Task 2 — Cut release

- [ ] **Step 1: Verify clean tree**

```bash
cd /Users/maidok/Developer/powerqueen && git status
```

Expected: `working tree clean`.

- [ ] **Step 2: Push to origin**

```bash
git push origin main 2>&1 | tail -3
```

- [ ] **Step 3: Delete the stale local fw-* tag**

```bash
git tag -d $(git tag --list 'fw-*') 2>&1 | tail -3
```

- [ ] **Step 4: Cut release**

```bash
cd c6-hub && ./release.sh 2>&1 | tail -3
```

Expected: `released fw-<sha>` and a GitHub URL.

- [ ] **Step 5: Power-cycle the chip + verify OTA delivers**

```bash
ssh caravan 'python3 << "PYEOF"
import serial, sys, time
s = serial.Serial("/dev/ttyACM0", 115200, timeout=0.3)
s.setDTR(False); s.setRTS(True); time.sleep(0.1); s.setRTS(False)
end = time.time() + 100
while time.time() < end:
    line = s.readline()
    if line: sys.stdout.write(line.decode(errors="replace")); sys.stdout.flush()
PYEOF' 2>&1 | grep -aE "App version|ota:|pm: |ps: |al: " | head -25
```

Expected: chip OTAs into the new SHA, post-OTA shows `pm:` streams, `ps: FLOAT`, and no `al:` lines.

---

## Acceptance Criteria (from spec)

After both tasks complete:

1. ✅ Boot idle → no `al:` lines over the first 60 s (Task 1 Step 6).
2. ✅ Throwaway-lowered threshold → `al: ENTER LOW_SOC SoC=100` within seconds (Task 1 Step 7).
3. ✅ Threshold reverted → `al: CLEAR LOW_SOC SoC=100` after re-flash (Task 1 Step 7).
4. Stale BMS test — defer; verify by physical button-off if convenient.
5. Heap delta ≤ 4 KiB after 1 h — defer; not load-bearing.

---

## Notes for the engineer

- **`b->cell_temp_c` is `int8_t`** (range −128..127). The thresholds 0, 5, 45, 50 are well inside that range. No casting concerns.
- **`UNDER_TEMP_CHARGE` reads `power_state`.** This adds an ordering dependency: `power_state_init()` must run before `alerts_init()`. The plan already wires that in Step 4.
- **`ALERT_COUNT` is the sentinel.** Switch statements include a `case ALERT_COUNT: return false;` so `-Wswitch` is happy.
- **No NVS, no Kconfig.** First-cut alerts are intentionally simple; lift to Kconfig only if field tuning becomes important.
