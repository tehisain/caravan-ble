# `power_monitor` — BLE Telemetry Design

**Date:** 2026-05-23
**Status:** Approved, ready for plan

## Overview

First domain module on the C6. Read live power-system telemetry from
three Bluetooth Low Energy devices already in range inside the caravan:

| Device | MAC | Protocol |
|---|---|---|
| Power Queen 100 Ah LiFePO4 | `C8:47:80:10:C2:DE` | GATT, write+notify on `0xFFE1` |
| Victron SmartSolar MPPT 75/15 | `DA:02:0F:CA:6D:1B` | Encrypted BLE advertising (Instant Readout) |
| Victron Blue Smart IP65 12/25 | `E9:3C:6F:D4:A6:08` | Encrypted BLE advertising (Instant Readout) |

Cache the latest readings in memory, expose them via getter API. No
consumer yet — future modules (`state_machine`, `display_link`,
`ble_server`) will pull values. This iteration's success criterion is
seeing all three streams report sensible numbers on the boot log.

## Goals

- C6 maintains a live in-memory mirror of battery / solar / charger
  state with sub-minute freshness.
- Failure of any single device path is non-fatal; the others keep
  flowing.
- Code is structured so the protocol decoders are testable with
  captured byte arrays (host-side later if needed) — no ESP-IDF
  dependencies in `powerqueen_bms.c` or `victron_iadv.c` beyond
  `<stdint.h>` / `<string.h>` / mbedTLS for AES.

## Non-goals

- Phone-side GATT server (deferred — separate `ble_server.c` module).
- T-Encoder display push (separate `display_link.c`).
- Alerts / SMS thresholds (separate `alerts.c`).
- INA219 ×2 over I2C and shore/car GPIO detect (will be added to this
  same module later; the file is named for what it monitors, not for
  the transport).
- On-device unit-test wiring with Unity.

## Files

```
c6-hub/main/
├── power_monitor.{c,h}   # Public API, FreeRTOS task, GAP callback
├── powerqueen_bms.{c,h}  # Pure-data: encode GET_BATTERY_INFO, decode response
└── victron_iadv.{c,h}    # Pure-data: parse + AES-CTR decrypt + decode records
```

`main.c` loses its current bring-up BLE scanner — `power_monitor_init()`
replaces it.

The protocol files are kept dependency-light so they can be lifted into
a future host-side test harness without touching firmware code.

## Public API (`power_monitor.h`)

```c
typedef struct {
    bool      valid;
    int64_t   last_seen_ts;   // esp_timer_get_time() / 1000 (ms)
    float     pack_voltage_v;
    float     current_a;      // + charging, − discharging
    uint8_t   soc_pct;
    uint8_t   soh_pct;
    int8_t    cell_temp_c;
    int8_t    mosfet_temp_c;
    float     remaining_ah;
    uint16_t  cycles;
    uint8_t   state;          // 0=idle 1=charging 2=discharging 4=full
    uint16_t  cell_mv[16];    // zero = absent
} battery_reading_t;

typedef struct {
    bool      valid;
    int64_t   last_seen_ts;
    float     battery_voltage_v;
    float     charging_current_a;
    uint16_t  solar_power_w;
    uint16_t  yield_today_wh;
    uint8_t   charge_state;
} solar_reading_t;

typedef struct {
    bool      valid;
    int64_t   last_seen_ts;
    float     output_voltage_v;
    float     output_current_a;
    uint8_t   charge_state;
} charger_reading_t;

esp_err_t power_monitor_init(void);
esp_err_t power_monitor_get_battery(battery_reading_t *out);
esp_err_t power_monitor_get_solar(solar_reading_t *out);
esp_err_t power_monitor_get_charger(charger_reading_t *out);
```

Getters return `ESP_OK` when the reading is valid AND
`(now − last_seen_ts) < freshness_ms`, else `ESP_ERR_NOT_FOUND`.
Freshness thresholds: 60 s for BMS (2× poll interval), 10 s for the two
Victron streams (10× expected ADV interval).

## Operation

### Boot sequence (in `main.c`)

1. NimBLE host initialized (existing code).
2. `power_monitor_init()`:
   - Registers a GAP scan callback.
   - Starts a passive scan **without** duplicate filtering (Victron
     payloads change every ~1 s).
   - Spawns a FreeRTOS task `bms_poll_task` with a 6 KiB stack at
     priority 5.

### Victron path (reactive, in the GAP callback)

```
on BLE_GAP_EVENT_DISC:
    if adv.mac == CONFIG_POWER_VICTRON_SOLAR_MAC:
        victron_iadv_decode(adv.mfg_data, solar_key) → solar_reading_t
        atomically copy into module-private cache
    elif adv.mac == CONFIG_POWER_VICTRON_IP65_MAC:
        victron_iadv_decode(adv.mfg_data, ip65_key) → charger_reading_t
        atomically copy into module-private cache
    # else ignored silently — no log spam from neighbors
```

Logs are emitted only when `charge_state` transitions, not on every
ADV. Once per minute a "still receiving" debug line confirms the
stream is alive.

### BMS path (active loop in `bms_poll_task`)

```
loop:
    if ble_gap_connect(bms_mac, timeout=5s) == OK:
        find_service(0xFFE0), find_char(0xFFE1), subscribe to notify
        write(GET_BATTERY_INFO)  # 8-byte frame, ends with CRC
        wait for notify, timeout 3s
        if frame_valid (CRC + opcode):
            powerqueen_bms_decode(frame, len, &battery_reading_t)
            update cache
            log on change
        ble_gap_terminate(conn)
    sleep CONFIG_POWER_BMS_POLL_INTERVAL_MS  # default 30 s
```

When connected, NimBLE keeps the scanner running at the controller
level — Victron ADVs continue to flow. This is the documented
multi-role behavior on ESP32-C6.

### Concurrency

A single `portMUX` (spinlock) protects the three reading structs.
GAP callback runs in the NimBLE host task; `bms_poll_task` runs
separately; getters run in whatever called them. All three contend on
the spinlock for brief writes/reads only.

## Configuration (Kconfig.projbuild additions)

```kconfig
menu "Caravan Power"

config POWER_BMS_MAC
    string "PowerQueen BMS MAC"
    default "C8:47:80:10:C2:DE"

config POWER_VICTRON_SOLAR_MAC
    string "Victron SmartSolar MAC"
    default "DA:02:0F:CA:6D:1B"

config POWER_VICTRON_SOLAR_KEY
    string "Victron SmartSolar Instant-Readout key (32 hex chars)"
    default ""

config POWER_VICTRON_IP65_MAC
    string "Victron IP65 MAC"
    default "E9:3C:6F:D4:A6:08"

config POWER_VICTRON_IP65_KEY
    string "Victron IP65 Instant-Readout key (32 hex chars)"
    default ""

config POWER_BMS_POLL_INTERVAL_MS
    int "BMS poll interval (ms)"
    default 30000

endmenu
```

Empty key string ⇒ that Victron device is silently skipped in the GAP
callback (no log spam, no failure).

## Protocol decoder responsibilities

### `powerqueen_bms.{c,h}`

```c
// Build the 8-byte GET_BATTERY_INFO request frame.
void pq_bms_build_get_battery_info(uint8_t out[8]);

// Validate + decode a GET_BATTERY_INFO response into the caller's struct.
// Returns false on framing/CRC/opcode errors. Pure function.
bool pq_bms_decode_response(const uint8_t *frame, size_t len,
                            battery_reading_t *out);
```

CRC = sum of preceding bytes & 0xFF (per protocol doc). Frame
opcode-response is the request opcode with `0x80` set.

### `victron_iadv.{c,h}`

```c
typedef enum {
    VIADV_SOLAR,
    VIADV_CHARGER,
    VIADV_OTHER,
} viadv_kind_t;

// Decode a raw manufacturer-data payload (already past the 0x02E1 ID)
// using a 16-byte AES key. Determines kind from the payload's device
// type, fills the appropriate reading struct, returns kind.
viadv_kind_t viadv_decode(const uint8_t *mfg_data, size_t len,
                          const uint8_t key[16],
                          solar_reading_t *solar_out,
                          charger_reading_t *charger_out);
```

mbedTLS provides AES-CTR via `mbedtls_aes_crypt_ctr` — already linked
because the OTA cert bundle uses it.

Key format conversion (hex string ⇒ 16 bytes) happens in
`power_monitor.c` at init time; the decoder takes the raw 16 bytes.

## Error & logging behavior

| Event | Level | Rate limit |
|---|---|---|
| BMS connect failure | `WARN` | one log per cycle (the cycle itself rate-limits) |
| BMS read timeout | `WARN` | one log per cycle |
| BMS frame CRC bad | `WARN` | one log per cycle |
| Victron decrypt failure | `WARN` | once per minute per device |
| Victron frame malformed | `WARN` | once per minute per device |
| Reading freshness check | (none — caller's concern) | — |
| Charge-state transition | `INFO` | unrestricted (state changes are rare) |
| Heartbeat "alive" line | `DEBUG` | once per minute |
| Heap snapshot | `INFO` | once per hour |

`ESP_LOGI` defaults stay informative; the noisy paths are kept at
`DEBUG` so they're off by default but flipped on for instrumentation
when needed.

## Acceptance Criteria

1. Boot and within 35 s observe in the log:
   - `pm: solar v=<x>.<y> i=… W=… state=…`
   - `pm: charger v=… i=… state=…`
   - `pm: bms v=… i=… SoC=… SoH=…`
2. With one Victron key intentionally wrong: that device's stream
   doesn't appear; other two are unaffected.
3. With BMS BLE radio off (turn the battery's BLE button off): one
   `WARN` per cycle, Victron streams unchanged.
4. After running for one hour, free heap on the periodic snapshot is
   ≥ 200 KiB (sanity check that the new module + NimBLE central +
   ongoing scan don't leak).
5. Wi-Fi remains functional during BMS connects (verify by power-cycling
   while Wi-Fi-connected and seeing the OTA check complete normally).

## Risks & Open Questions

- **Cell-voltage array size.** Power Queen 12V/100Ah has 4 cells; the
  protocol doc allows up to 16. We allocate 16. Wastes 24 bytes per
  reading; acceptable.
- **Victron payload truncation.** Some Victron devices send shorter
  payloads under low-power conditions. `viadv_decode` must not assume
  fixed length — it reads the device-type byte and then size-checks
  against the device's expected layout.
- **Connect-while-scanning** is officially supported on the ESP32-C6
  but worth confirming on this exact ESP-IDF build. If it fails, the
  fallback is: pause scan ⇒ connect ⇒ poll ⇒ disconnect ⇒ resume scan
  (Victron readings then go stale for ~2 s every 30 s — acceptable).
- **AES-CTR key handling.** Keys end up in the on-disk `sdkconfig`
  file, which is already `.gitignore`d. Compiled into the binary. If
  the firmware is dumped via esptool, keys are recoverable. This is
  acceptable for a single-owner caravan; not for fleet deployments.

## Out of Scope (Future)

- Add INA219 ×2 over I2C (GPIO 4/5) to `power_monitor.c` once the I2C
  driver is in place.
- Add shore-power and car-detect GPIO inputs.
- Push readings to T-Encoder display.
- Expose via a phone-side BLE GATT server.
- Threshold-based alerts (low SoC, over-temp, charger fault).
- On-device unit tests.
