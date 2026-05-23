# `power_state` Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a single small module that consumes the three live `power_monitor` streams, derives a coherent power state (CHARGING/DISCHARGING/FLOAT/UNKNOWN) plus shore/solar flags, and logs on transitions — laying the foundation for alerts and the caravan-mode state machine.

**Architecture:** Two new files in `c6-hub/main/`. A 1-Hz FreeRTOS task pulls fresh readings via `power_monitor_get_*()`, applies the rules from the spec, atomically updates a spinlock-protected cache, and logs single-line transition events. Public getter copies the cache under the same spinlock and refuses stale answers.

**Tech Stack:** ESP-IDF v6.0, FreeRTOS task + portMUX spinlock, ESP_LOG.

**Spec reference:** `docs/superpowers/specs/2026-05-23-power-state-design.md`.

---

## File Structure

```
c6-hub/main/
├── CMakeLists.txt          MODIFY — add power_state.c to SRCS
├── main.c                  MODIFY — call power_state_init after power_monitor_init
├── power_state.h           NEW    — public types + getter declaration
└── power_state.c           NEW    — derivation + task + cache
```

---

## Key Decisions Locked In (from spec)

| Decision | Value |
|---|---|
| Threshold (charging vs idle) | `0.3 A` |
| Derivation cadence | 1 Hz |
| Freshness for getter | 5 s |
| FLOAT criterion | `bms.state == 4` OR (`abs(current) ≤ 0.3` AND `SoC ≥ 95`) |
| Log policy | Only on `state` / `shore_active` / `solar_active` transitions |
| Task stack | 2.5 KiB |
| Task priority | 4 |

---

## Task 1 — `power_state` module + integration

**Files:**
- Create: `c6-hub/main/power_state.h`
- Create: `c6-hub/main/power_state.c`
- Modify: `c6-hub/main/CMakeLists.txt`
- Modify: `c6-hub/main/main.c`

- [ ] **Step 1: Write `power_state.h`**

Create `c6-hub/main/power_state.h`:

```c
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
```

- [ ] **Step 2: Write `power_state.c`**

Create `c6-hub/main/power_state.c`:

```c
#include "power_state.h"

#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "power_monitor.h"

static const char *TAG = "ps";

#define IDLE_THRESHOLD_A       0.3f
#define FLOAT_SOC_THRESHOLD    95
#define BMS_STATE_FULL         4
#define EVAL_INTERVAL_MS       1000
#define FRESHNESS_MS           5000

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static power_state_reading_t s_cache;

static int64_t now_ms(void)
{
    return esp_timer_get_time() / 1000;
}

static const char *state_name(power_state_t st)
{
    switch (st) {
        case POWER_STATE_UNKNOWN:     return "UNKNOWN";
        case POWER_STATE_CHARGING:    return "CHARGING";
        case POWER_STATE_DISCHARGING: return "DISCHARGING";
        case POWER_STATE_FLOAT:       return "FLOAT";
        case POWER_STATE_FAULT:       return "FAULT";
    }
    return "?";
}

static power_state_t derive_state(const battery_reading_t *b)
{
    if (b->current_a > IDLE_THRESHOLD_A)  return POWER_STATE_CHARGING;
    if (b->current_a < -IDLE_THRESHOLD_A) return POWER_STATE_DISCHARGING;
    if (b->state == BMS_STATE_FULL)       return POWER_STATE_FLOAT;
    if (b->soc_pct >= FLOAT_SOC_THRESHOLD) return POWER_STATE_FLOAT;
    return POWER_STATE_FLOAT;
}

static void power_state_task(void *arg)
{
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(EVAL_INTERVAL_MS));

        battery_reading_t b;
        solar_reading_t   s;
        charger_reading_t c;
        bool b_ok = (power_monitor_get_battery(&b) == ESP_OK);
        bool s_ok = (power_monitor_get_solar(&s)   == ESP_OK);
        bool c_ok = (power_monitor_get_charger(&c) == ESP_OK);

        power_state_reading_t now = {
            .valid = true,
            .last_eval_ts_ms = now_ms(),
        };

        if (!b_ok || !s_ok || !c_ok) {
            now.state = POWER_STATE_UNKNOWN;
        } else {
            now.state         = derive_state(&b);
            now.shore_active  = c.output_current_a   > IDLE_THRESHOLD_A;
            now.solar_active  = s.charging_current_a > IDLE_THRESHOLD_A;
            now.net_current_a = b.current_a;
            now.soc_pct       = b.soc_pct;
        }

        power_state_reading_t prev;
        portENTER_CRITICAL(&s_lock);
        prev = s_cache;
        s_cache = now;
        portEXIT_CRITICAL(&s_lock);

        bool changed = !prev.valid
                    || prev.state        != now.state
                    || prev.shore_active != now.shore_active
                    || prev.solar_active != now.solar_active;
        if (changed) {
            ESP_LOGI(TAG, "%s shore=%d solar=%d i=%+.2f SoC=%u",
                     state_name(now.state),
                     (int)now.shore_active, (int)now.solar_active,
                     now.net_current_a, now.soc_pct);
        }
    }
}

esp_err_t power_state_init(void)
{
    BaseType_t rc = xTaskCreate(power_state_task, "power_state",
                                2560, NULL, 4, NULL);
    if (rc != pdPASS) {
        ESP_LOGE(TAG, "task create failed");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t power_state_get(power_state_reading_t *out)
{
    portENTER_CRITICAL(&s_lock);
    *out = s_cache;
    portEXIT_CRITICAL(&s_lock);
    if (!out->valid) return ESP_ERR_NOT_FOUND;
    if (now_ms() - out->last_eval_ts_ms > FRESHNESS_MS) return ESP_ERR_NOT_FOUND;
    return ESP_OK;
}
```

- [ ] **Step 3: Add to CMakeLists**

Edit `c6-hub/main/CMakeLists.txt`. Append `"power_state.c"` to the SRCS list:

```cmake
idf_component_register(
    SRCS "main.c" "wifi_manager.c" "ota_handler.c"
         "powerqueen_bms.c" "victron_iadv.c" "power_monitor.c"
         "power_state.c"
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

Edit `c6-hub/main/main.c`. Add include after the `power_monitor.h` line:

```c
#include "power_monitor.h"
#include "power_state.h"
```

Add the init call right after `power_monitor_init()`:

```c
    power_monitor_init();
    power_state_init();
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

- [ ] **Step 6: Verify on hardware**

Capture 40 s of serial:

```bash
ssh caravan 'python3 << "PYEOF"
import serial, sys, time
s = serial.Serial("/dev/ttyACM0", 115200, timeout=0.3)
s.setDTR(False); s.setRTS(True); time.sleep(0.1); s.setRTS(False)
end = time.time() + 40
while time.time() < end:
    line = s.readline()
    if line: sys.stdout.write(line.decode(errors="replace")); sys.stdout.flush()
PYEOF' 2>&1 | grep -aE "pm: |ps: " | head -10
```

Expected within ~15 s of boot (in order):
- `pm: scan started; solar=1 ip65=1`
- `pm: solar v=...`
- `pm: charger v=...`
- `pm: bms v=... SoC=100 ...`
- `ps: FLOAT shore=0 solar=0 i=+0.00 SoC=100`

The `ps:` line appears one second after the first `pm: bms` (the eval task evaluates 1 s after boot). It only appears once because nothing transitions in idle.

If you see `ps: UNKNOWN shore=0 solar=0 i=+0.00 SoC=0` for many seconds, BMS reads are timing out — check the BMS is awake (front button).

- [ ] **Step 7: Commit**

```bash
cd /Users/maidok/Developer/powerqueen
git add c6-hub/main/power_state.h c6-hub/main/power_state.c \
        c6-hub/main/CMakeLists.txt c6-hub/main/main.c
git commit -m "c6-hub: power_state — derive coherent state from BLE streams"
```

---

## Task 2 — Cut release

**Files:** none — purely operational.

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

The previous release's tag is still in the local repo and would entangle the next build's `git describe`:

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
PYEOF' 2>&1 | grep -aE "App version|ota:|pm: |ps: " | head -25
```

Expected: chip OTAs from `fw-b9621c9` into the new SHA, post-OTA boot shows all four streams alive (`pm: solar`, `pm: charger`, `pm: bms`, `ps: FLOAT …`).

---

## Acceptance Criteria (from spec)

After both tasks complete:

1. ✅ Boot idle → one `ps: FLOAT shore=0 solar=0 …` line, then silence (Task 1 Step 6).
2. Shore plug-in → `ps: FLOAT shore=1 solar=0 …` within 2 s. **Run this test by physically plugging in shore power if convenient; otherwise mark as deferred.**
3. Shore unplug → `ps: FLOAT shore=0 solar=0 …` within 2 s. (Same as above.)
4. BMS BLE button off → `ps: UNKNOWN shore=0 solar=0 …` within ~60 s.
5. Heap delta ≤ 4 KiB after 1 h. Not load-bearing; spot check.

---

## Notes for the engineer

- **Why log on shore/solar flag transitions too?** The state enum often stays FLOAT even when shore or solar starts pushing (because the battery is already full). The flags carry the meaningful event.
- **`current_a` sign convention** from `powerqueen_bms.c`: positive = charging (current flowing INTO battery), negative = discharging.
- **No spinlock contention concerns.** The task writes once per second; the getter is called rarely. The critical sections are pure `memcpy` of a small struct.
- **No persistence.** First boot starts with `cache.valid = false`; first eval populates it. Getters return `NOT_FOUND` until then.
