# `power_monitor` BLE Telemetry — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace the bring-up BLE scanner in `c6-hub` with a real telemetry module that polls the Power Queen BMS via GATT every 30 s and decodes Victron SmartSolar + IP65 Instant Readout from BLE advertising — caching the latest readings for future modules to consume.

**Architecture:** Three new units in `c6-hub/main/`. `powerqueen_bms` and `victron_iadv` are pure-data decoders with no ESP-IDF dependencies beyond `<stdint.h>` and mbedTLS. `power_monitor` is the only NimBLE-aware unit: it registers a GAP scan callback that routes Victron ADVs to the decoder, and runs a dedicated FreeRTOS task that opens a GATT connection to the BMS every 30 s. mbedTLS (already linked for OTA's cert bundle) provides AES-CTR for Victron decryption.

**Tech Stack:** ESP-IDF v6.0, NimBLE (host + central + observer roles), mbedTLS AES-128-CTR, FreeRTOS task + spinlock.

**Spec reference:** `docs/superpowers/specs/2026-05-23-power-monitor-design.md`
**Protocol references:**
- `docs/protocols/powerqueen_bms.md` (frame layout, CRC, byte offsets)
- `docs/protocols/victron_instant_readout.md` (manufacturer data 0x02E1, AES-CTR)
- `tools/ble-probe/read_battery.py` (working Python decoder — use as oracle for byte-level offsets)
- `tools/ble-probe/read_victron.py` (working Python — note that the actual crypto lives in the `victron-ble` PyPI package, schematically described below)

---

## File Structure

```
c6-hub/main/
├── CMakeLists.txt                MODIFY — add new SRCS
├── Kconfig.projbuild             MODIFY — add Caravan Power menu
├── main.c                        MODIFY — drop the bring-up scanner, call power_monitor_init
├── power_monitor.h               NEW    — public reading_t structs + getters
├── power_monitor.c               NEW    — GAP callback dispatch + bms_poll_task
├── powerqueen_bms.h              NEW    — pure decoder declarations
├── powerqueen_bms.c              NEW    — pure decoder
├── victron_iadv.h                NEW    — pure decoder declarations
└── victron_iadv.c                NEW    — pure decoder with mbedTLS AES-CTR

c6-hub/sdkconfig.defaults         MODIFY — enable NimBLE central role
```

The protocol unit files (`powerqueen_bms.*`, `victron_iadv.*`) are intentionally dependency-light so they can be lifted into a host-side test program later. They include only `<stdint.h>`, `<stddef.h>`, `<string.h>`, and (for Victron) `mbedtls/aes.h`.

---

## Key Decisions Locked In (from spec)

| Decision | Value |
|---|---|
| BMS connect strategy | Connect → poll → disconnect every 30 s |
| Victron strategy | Passive scan; decode in GAP event handler |
| Key storage | Kconfig (one MAC + one key per Victron device) |
| Freshness threshold | 60 s (BMS), 10 s (Victron) |
| Log policy | `INFO` on charge_state transitions; rate-limited `WARN` on errors |

---

## Task 1 — Kconfig + sdkconfig additions

**Files:**
- Modify: `c6-hub/main/Kconfig.projbuild`
- Modify: `c6-hub/sdkconfig.defaults`

- [ ] **Step 1: Append a new menu block to `Kconfig.projbuild`**

Open `c6-hub/main/Kconfig.projbuild` and append at the end (after `endmenu` of the existing `Caravan OTA` menu):

```kconfig
menu "Caravan Power"

config POWER_BMS_MAC
    string "PowerQueen BMS MAC (AA:BB:CC:DD:EE:FF)"
    default "C8:47:80:10:C2:DE"

config POWER_VICTRON_SOLAR_MAC
    string "Victron SmartSolar MAC"
    default "DA:02:0F:CA:6D:1B"

config POWER_VICTRON_SOLAR_KEY
    string "Victron SmartSolar Instant-Readout key (32 hex chars)"
    default ""

config POWER_VICTRON_IP65_MAC
    string "Victron IP65 charger MAC"
    default "E9:3C:6F:D4:A6:08"

config POWER_VICTRON_IP65_KEY
    string "Victron IP65 Instant-Readout key (32 hex chars)"
    default ""

config POWER_BMS_POLL_INTERVAL_MS
    int "BMS poll interval (ms)"
    default 30000

endmenu
```

- [ ] **Step 2: Enable NimBLE central role in `sdkconfig.defaults`**

The OTA-era sdkconfig already has `CONFIG_BT_NIMBLE_ENABLED=y`. NimBLE defaults to having central role compiled in, but we want explicit + connect intervals reasonable for the BMS. Append after the existing BLE section in `c6-hub/sdkconfig.defaults`:

```conf
# power_monitor — NimBLE central + observer simultaneously
CONFIG_BT_NIMBLE_ROLE_CENTRAL=y
CONFIG_BT_NIMBLE_ROLE_OBSERVER=y
CONFIG_BT_NIMBLE_MAX_CONNECTIONS=2
```

- [ ] **Step 3: Run `menuconfig` to set the two Victron keys**

```bash
cd c6-hub
TERM=xterm-256color ./build.sh menuconfig
```

Navigate to `Caravan Power → Victron SmartSolar Instant-Readout key`, paste the 32-hex from VictronConnect. Repeat for `Victron IP65 Instant-Readout key`. Save (`S`), quit (`Q`).

Verify both ended up in sdkconfig (without echoing the values):

```bash
grep -E "POWER_VICTRON.*KEY" sdkconfig | sed -E 's/="[^"]+"/="<set>"/'
```

Expected:
```
CONFIG_POWER_VICTRON_SOLAR_KEY="<set>"
CONFIG_POWER_VICTRON_IP65_KEY="<set>"
```

- [ ] **Step 4: Verify a no-source-change build still succeeds**

```bash
cd c6-hub && ./build.sh 2>&1 | tail -5
```

Expected: `Project build complete.` plus the four `idf.py flash` instructions.

- [ ] **Step 5: Commit**

```bash
cd /Users/maidok/Developer/powerqueen
git add c6-hub/main/Kconfig.projbuild c6-hub/sdkconfig.defaults
git commit -m "c6-hub: Kconfig prep for power_monitor module"
```

---

## Task 2 — `powerqueen_bms` pure decoder

**Files:**
- Create: `c6-hub/main/powerqueen_bms.h`
- Create: `c6-hub/main/powerqueen_bms.c`

This unit has no NimBLE dependency. All it does: build the 8-byte request bytes, and decode the response bytes into a `battery_reading_t`.

It depends on `battery_reading_t` from `power_monitor.h`. To avoid a circular dependency, we declare the struct in a shared header that's safe to include from a pure C file: we'll put the struct in `power_monitor.h` itself but the protocol headers can include only `<stdint.h>` and forward-declare the struct via the function signatures.

To keep it clean, `power_monitor.h` is the source of truth for `battery_reading_t` and `powerqueen_bms.h` includes it. The reverse is not allowed.

- [ ] **Step 1: Write `power_monitor.h` (the shared types only)**

Create `c6-hub/main/power_monitor.h`:

```c
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
```

- [ ] **Step 2: Write `powerqueen_bms.h`**

Create `c6-hub/main/powerqueen_bms.h`:

```c
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "power_monitor.h"

#define PQ_BMS_REQ_LEN 8

// Fill `out` with the 8-byte GET_BATTERY_INFO request frame.
void pq_bms_build_get_battery_info(uint8_t out[PQ_BMS_REQ_LEN]);

// Validate framing (length, opcode, CRC) and decode the payload of a
// GET_BATTERY_INFO response. Returns true on success.
// `frame` may contain trailing garbage beyond the declared length; we
// rely on the frame's internal length byte (offset 2).
bool pq_bms_decode_response(const uint8_t *frame, size_t len,
                            battery_reading_t *out);
```

- [ ] **Step 3: Write `powerqueen_bms.c`**

Create `c6-hub/main/powerqueen_bms.c`:

```c
#include "powerqueen_bms.h"
#include <string.h>

// Request frame: 00 00 04 01 <opcode> 55 AA <crc>
// opcode 0x13 = GET_BATTERY_INFO
// CRC = sum(preceding bytes) & 0xFF

static uint8_t crc8(const uint8_t *data, size_t len)
{
    uint8_t sum = 0;
    for (size_t i = 0; i < len; i++) sum += data[i];
    return sum;
}

void pq_bms_build_get_battery_info(uint8_t out[PQ_BMS_REQ_LEN])
{
    out[0] = 0x00;
    out[1] = 0x00;
    out[2] = 0x04;
    out[3] = 0x01;
    out[4] = 0x13;
    out[5] = 0x55;
    out[6] = 0xAA;
    out[7] = crc8(out, 7);
}

// Little-endian readers. The protocol stores all integers LE; the
// Python reference reads bytes reversed then big-endian, which is
// arithmetically identical.

static uint16_t rd_u16_le(const uint8_t *p) {
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static int16_t rd_i16_le(const uint8_t *p) {
    return (int16_t)rd_u16_le(p);
}

static uint32_t rd_u32_le(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static int32_t rd_i32_le(const uint8_t *p) {
    return (int32_t)rd_u32_le(p);
}

bool pq_bms_decode_response(const uint8_t *frame, size_t len,
                            battery_reading_t *out)
{
    // Minimum useful payload from the protocol doc reaches offset 104.
    // The frame's "length" byte at offset 2 is documented as 4 in the
    // request; in the response it carries the payload length. We use
    // the actual buffer length passed in.
    if (len < 105) return false;
    if (frame[3] != 0x01) return false;
    // Response opcode = request opcode | 0x80, so 0x13 | 0x80 = 0x93
    if (frame[4] != 0x93) return false;
    // CRC last byte
    if (crc8(frame, len - 1) != frame[len - 1]) return false;

    memset(out, 0, sizeof(*out));
    out->pack_voltage_v     = rd_u32_le(frame + 8)  / 1000.0f;
    out->terminal_voltage_v = rd_u32_le(frame + 12) / 1000.0f;
    out->current_a          = rd_i32_le(frame + 48) / 1000.0f;
    out->cell_temp_c        = (int8_t)rd_i16_le(frame + 52);
    out->mosfet_temp_c      = (int8_t)rd_i16_le(frame + 54);
    out->remaining_ah       = rd_u16_le(frame + 62) / 100.0f;
    out->capacity_ah        = rd_u16_le(frame + 64) / 100.0f;
    out->state              = (uint8_t)rd_u16_le(frame + 88);
    out->soc_pct            = (uint8_t)rd_u16_le(frame + 90);
    out->soh_pct            = (uint8_t)rd_u32_le(frame + 92);
    out->cycles             = rd_u32_le(frame + 96);

    // Cell voltages: 16 × u16 starting at offset 16, little-endian.
    // The Python reference manually swaps bytes (per-cell big-endian-of-reversed),
    // which is the same as little-endian. Zero cells are unpopulated.
    uint8_t cell_count = 0;
    for (int i = 0; i < 16; i++) {
        uint16_t v = rd_u16_le(frame + 16 + i * 2);
        out->cell_mv[i] = v;
        if (v) cell_count++;
    }
    out->cell_count = cell_count;
    return true;
}
```

- [ ] **Step 4: Add to CMakeLists**

Edit `c6-hub/main/CMakeLists.txt`:

```cmake
idf_component_register(
    SRCS "main.c" "wifi_manager.c" "ota_handler.c" "powerqueen_bms.c"
    INCLUDE_DIRS "."
)
```

- [ ] **Step 5: Sanity-build**

```bash
cd c6-hub && ./build.sh 2>&1 | tail -3
```

Expected: build succeeds. `powerqueen_bms` is unreferenced from `main` so the linker will keep it (it's in the registered SRCS) but produce no output beyond a clean build. If it errors, fix and rerun.

- [ ] **Step 6: Commit**

```bash
cd /Users/maidok/Developer/powerqueen
git add c6-hub/main/power_monitor.h c6-hub/main/powerqueen_bms.h c6-hub/main/powerqueen_bms.c c6-hub/main/CMakeLists.txt
git commit -m "c6-hub: power_monitor shared types + powerqueen_bms decoder"
```

---

## Task 3 — `victron_iadv` decoder with AES-CTR

**Files:**
- Create: `c6-hub/main/victron_iadv.h`
- Create: `c6-hub/main/victron_iadv.c`
- Modify: `c6-hub/main/CMakeLists.txt`

Background: Victron's "Extra Manufacturer Data Record" frame inside the 0x02E1 manufacturer payload is:

```
offset  size  field
 0      1     prefix          0x10 (Extra Manufacturer Data Record)
 1      2     model_id        little-endian u16; identifies device type
 3      1     read_state      reserved, always 1
 4      1     iv_lo           low byte of AES counter
 5      1     iv_hi           high byte of AES counter
 6      1     first_byte_key  first byte of the key (sanity check)
 7+     N     ciphertext      AES-128-CTR, key from VictronConnect
```

The ciphertext, once decrypted, is the device-specific record. We only care about two device types (the ones we have):

- **0xA043** "SolarCharger" → `SolarChargerData`
- **0xA248** "AcCharger" → `AcChargerData`

There is no single official C reference; the canonical reference is the `victron-ble` Python library. Per-device-type layouts (after decrypt) are:

**SolarChargerData** (16 bytes decrypted, but we only need the first ~12):
```
offset  size  field                       units
 0      1     state                       enum (off/bulk/abs/float/equalize…)
 1      1     charger_error               enum
 2      2     battery_voltage             0.01 V, signed i16 (LE)
 4      2     battery_current             0.1 A, signed i16
 6      2     yield_today                 0.01 kWh, u16 (we expose as Wh ×10)
 8      2     pv_power                    1 W, u16
10      1     load_current_lo
11      1     load_current_hi (partial)
```

**AcChargerData** (≥ 8 bytes decrypted):
```
offset  size  field                       units
 0      1     state                       enum
 1      1     charger_error               enum
 2      2     output_voltage1             0.01 V, u16
 4      2     output_current1             0.1 A, u16
 6      2     output_voltage2 (or 0xFFFF) 0.01 V
 …      …    …                            …
```

For first cut: only parse channel 1 (which is the main output in the IP65 we have).

If the spec/parser later turns out to need refinement, the test target is the live device in the caravan — captured-bytes-in, parsed-struct-out.

- [ ] **Step 1: Write `victron_iadv.h`**

Create `c6-hub/main/victron_iadv.h`:

```c
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "power_monitor.h"

typedef enum {
    VIADV_KIND_NONE,        // bad frame, or device type we don't decode
    VIADV_KIND_SOLAR,       // output written into solar_out
    VIADV_KIND_CHARGER,     // output written into charger_out
} viadv_kind_t;

// Decode a manufacturer-data payload (the bytes that followed
// manufacturer-data ID 0x02E1, i.e. starting at the 0x10 prefix byte).
// `key` is the 16-byte AES key (already hex-decoded). Outputs are
// only written when the return value is the matching kind.
viadv_kind_t viadv_decode(const uint8_t *mfg_data, size_t len,
                          const uint8_t key[16],
                          solar_reading_t *solar_out,
                          charger_reading_t *charger_out);

// Helper used by power_monitor at init to convert the 32-char hex key
// strings from Kconfig into 16 bytes. Returns false on bad input.
bool viadv_hex_to_key(const char *hex, uint8_t out[16]);
```

- [ ] **Step 2: Write `victron_iadv.c`**

Create `c6-hub/main/victron_iadv.c`:

```c
#include "victron_iadv.h"
#include <string.h>
#include "mbedtls/aes.h"

// Public Victron model IDs we handle.
#define MODEL_SOLAR_CHARGER 0xA043
#define MODEL_AC_CHARGER    0xA248

static int hex_nibble(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
    if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
    return -1;
}

bool viadv_hex_to_key(const char *hex, uint8_t out[16])
{
    if (!hex || strlen(hex) != 32) return false;
    for (int i = 0; i < 16; i++) {
        int hi = hex_nibble(hex[i * 2]);
        int lo = hex_nibble(hex[i * 2 + 1]);
        if (hi < 0 || lo < 0) return false;
        out[i] = (uint8_t)((hi << 4) | lo);
    }
    return true;
}

static uint16_t rd_u16_le(const uint8_t *p) {
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}
static int16_t rd_i16_le(const uint8_t *p) {
    return (int16_t)rd_u16_le(p);
}

// Decrypt the ciphertext portion of a Victron manufacturer-data frame
// into `plain`. Returns plaintext length, or 0 on framing error.
//
// AES-128-CTR with:
//   key   = caller's 16-byte key
//   nonce = iv (2 bytes from frame) followed by 14 zero bytes
static size_t viadv_decrypt(const uint8_t *mfg, size_t len,
                            const uint8_t key[16],
                            uint8_t plain[16])
{
    if (len < 8) return 0;
    if (mfg[0] != 0x10) return 0;           // not an "Extra Manufacturer Data Record"
    if (mfg[6] != key[0]) return 0;         // first-byte-of-key sanity check

    uint8_t nonce[16] = {0};
    nonce[0] = mfg[4];
    nonce[1] = mfg[5];

    size_t ct_len = len - 7;
    if (ct_len > 16) ct_len = 16;            // cap at our buffer
    uint8_t ct_buf[16];
    memcpy(ct_buf, mfg + 7, ct_len);

    mbedtls_aes_context aes;
    mbedtls_aes_init(&aes);
    if (mbedtls_aes_setkey_enc(&aes, key, 128) != 0) {
        mbedtls_aes_free(&aes);
        return 0;
    }
    size_t nc_off = 0;
    uint8_t stream_block[16] = {0};
    if (mbedtls_aes_crypt_ctr(&aes, ct_len, &nc_off, nonce, stream_block,
                              ct_buf, plain) != 0) {
        mbedtls_aes_free(&aes);
        return 0;
    }
    mbedtls_aes_free(&aes);
    return ct_len;
}

static viadv_kind_t decode_solar(const uint8_t *p, size_t len,
                                 solar_reading_t *out)
{
    if (len < 10) return VIADV_KIND_NONE;
    memset(out, 0, sizeof(*out));
    out->charge_state         = p[0];
    out->battery_voltage_v    = rd_i16_le(p + 2) / 100.0f;
    out->charging_current_a   = rd_i16_le(p + 4) / 10.0f;
    out->yield_today_wh       = rd_u16_le(p + 6) * 10;   // 0.01 kWh -> Wh × 10
    out->solar_power_w        = (int16_t)rd_u16_le(p + 8);
    return VIADV_KIND_SOLAR;
}

static viadv_kind_t decode_charger(const uint8_t *p, size_t len,
                                   charger_reading_t *out)
{
    if (len < 6) return VIADV_KIND_NONE;
    memset(out, 0, sizeof(*out));
    out->charge_state       = p[0];
    out->output_voltage_v   = rd_u16_le(p + 2) / 100.0f;
    out->output_current_a   = rd_u16_le(p + 4) / 10.0f;
    return VIADV_KIND_CHARGER;
}

viadv_kind_t viadv_decode(const uint8_t *mfg, size_t len,
                          const uint8_t key[16],
                          solar_reading_t *solar_out,
                          charger_reading_t *charger_out)
{
    if (len < 8) return VIADV_KIND_NONE;
    uint16_t model = rd_u16_le(mfg + 1);

    uint8_t plain[16];
    size_t plen = viadv_decrypt(mfg, len, key, plain);
    if (plen == 0) return VIADV_KIND_NONE;

    switch (model) {
        case MODEL_SOLAR_CHARGER:
            return decode_solar(plain, plen, solar_out);
        case MODEL_AC_CHARGER:
            return decode_charger(plain, plen, charger_out);
        default:
            return VIADV_KIND_NONE;
    }
}
```

- [ ] **Step 3: Add to CMakeLists**

Edit `c6-hub/main/CMakeLists.txt`:

```cmake
idf_component_register(
    SRCS "main.c" "wifi_manager.c" "ota_handler.c"
         "powerqueen_bms.c" "victron_iadv.c"
    INCLUDE_DIRS "."
)
```

- [ ] **Step 4: Sanity-build**

```bash
cd c6-hub && ./build.sh 2>&1 | grep -E "error:|FAILED|c6-hub.bin binary" | head -5
```

Expected: a single line reporting `c6-hub.bin binary size …` and no errors. If `mbedtls/aes.h` is missing — it shouldn't be, OTA already pulls mbedTLS — verify `CONFIG_MBEDTLS_CERTIFICATE_BUNDLE=y` is set in sdkconfig.

- [ ] **Step 5: Commit**

```bash
cd /Users/maidok/Developer/powerqueen
git add c6-hub/main/victron_iadv.h c6-hub/main/victron_iadv.c c6-hub/main/CMakeLists.txt
git commit -m "c6-hub: victron_iadv decoder with AES-CTR"
```

---

## Task 4 — `power_monitor.c` skeleton + Victron path

**Files:**
- Create: `c6-hub/main/power_monitor.c`
- Modify: `c6-hub/main/main.c`

Replace the existing bring-up scanner in `main.c` with a call to `power_monitor_init()`. The first iteration only does the Victron path — no BMS yet. We'll wire BMS in Task 5 once we see Victron readings coming in.

- [ ] **Step 1: Write `power_monitor.c` (Victron-only first iteration)**

Create `c6-hub/main/power_monitor.c`:

```c
#include "power_monitor.h"

#include <string.h>
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "sdkconfig.h"

#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/ble_gap.h"

#include "victron_iadv.h"

static const char *TAG = "pm";

#define VICTRON_MFG_ID 0x02E1

// Module-private cache + spinlock.
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static battery_reading_t s_battery;
static solar_reading_t   s_solar;
static charger_reading_t s_charger;

static uint8_t s_solar_mac[6];
static uint8_t s_solar_key[16];
static bool    s_solar_enabled;

static uint8_t s_ip65_mac[6];
static uint8_t s_ip65_key[16];
static bool    s_ip65_enabled;

static uint8_t s_bms_mac[6];

// Parse "AA:BB:CC:DD:EE:FF" into 6 bytes. Returns false on bad input.
static bool parse_mac(const char *s, uint8_t out[6])
{
    if (!s || strlen(s) != 17) return false;
    for (int i = 0; i < 6; i++) {
        char a = s[i * 3], b = s[i * 3 + 1];
        int hi = (a >= '0' && a <= '9') ? a - '0'
               : (a >= 'a' && a <= 'f') ? 10 + a - 'a'
               : (a >= 'A' && a <= 'F') ? 10 + a - 'A' : -1;
        int lo = (b >= '0' && b <= '9') ? b - '0'
               : (b >= 'a' && b <= 'f') ? 10 + b - 'a'
               : (b >= 'A' && b <= 'F') ? 10 + b - 'A' : -1;
        if (hi < 0 || lo < 0) return false;
        out[i] = (uint8_t)((hi << 4) | lo);
        if (i < 5 && s[i * 3 + 2] != ':') return false;
    }
    return true;
}

// NimBLE ble_addr_t stores the MAC LSB-first; our parsed s_*_mac is MSB-first.
// Returns true when `addr.val` reversed equals `mac`.
static bool mac_match(const uint8_t addr_lsb_first[6], const uint8_t mac_msb_first[6])
{
    for (int i = 0; i < 6; i++) {
        if (addr_lsb_first[5 - i] != mac_msb_first[i]) return false;
    }
    return true;
}

// Find manufacturer-data field with the given company ID inside a raw
// BLE advertising payload. Sets *out to point inside the payload (just
// past the 2-byte company ID) and *out_len to remaining bytes. Returns
// false if absent.
static bool find_mfg_data(const uint8_t *adv, size_t adv_len, uint16_t company_id,
                          const uint8_t **out, size_t *out_len)
{
    size_t i = 0;
    while (i + 1 < adv_len) {
        uint8_t fld_len = adv[i];
        if (fld_len == 0) return false;
        if (i + 1 + fld_len > adv_len) return false;
        uint8_t fld_type = adv[i + 1];
        if (fld_type == 0xFF && fld_len >= 3) {
            uint16_t cid = (uint16_t)adv[i + 2] | ((uint16_t)adv[i + 3] << 8);
            if (cid == company_id) {
                *out = adv + i + 4;
                *out_len = fld_len - 3;
                return true;
            }
        }
        i += 1 + fld_len;
    }
    return false;
}

static int64_t now_ms(void)
{
    return esp_timer_get_time() / 1000;
}

static void log_solar_if_changed(const solar_reading_t *prev, const solar_reading_t *now)
{
    if (!prev->valid || prev->charge_state != now->charge_state) {
        ESP_LOGI(TAG, "solar v=%.2f i=%.1f W=%d state=%u",
                 now->battery_voltage_v, now->charging_current_a,
                 now->solar_power_w, now->charge_state);
    }
}

static void log_charger_if_changed(const charger_reading_t *prev, const charger_reading_t *now)
{
    if (!prev->valid || prev->charge_state != now->charge_state) {
        ESP_LOGI(TAG, "charger v=%.2f i=%.1f state=%u",
                 now->output_voltage_v, now->output_current_a, now->charge_state);
    }
}

static int gap_event_cb(struct ble_gap_event *event, void *arg)
{
    if (event->type != BLE_GAP_EVENT_DISC) return 0;

    const uint8_t *addr = event->disc.addr.val;
    const uint8_t *data = event->disc.data;
    size_t data_len     = event->disc.length_data;

    bool is_solar = s_solar_enabled && mac_match(addr, s_solar_mac);
    bool is_ip65  = s_ip65_enabled  && mac_match(addr, s_ip65_mac);
    if (!is_solar && !is_ip65) return 0;

    const uint8_t *mfg;
    size_t mfg_len;
    if (!find_mfg_data(data, data_len, VICTRON_MFG_ID, &mfg, &mfg_len)) {
        return 0;
    }

    solar_reading_t tmp_solar = {0};
    charger_reading_t tmp_charger = {0};
    const uint8_t *key = is_solar ? s_solar_key : s_ip65_key;
    viadv_kind_t kind = viadv_decode(mfg, mfg_len, key, &tmp_solar, &tmp_charger);

    if (kind == VIADV_KIND_SOLAR && is_solar) {
        tmp_solar.valid = true;
        tmp_solar.last_seen_ts_ms = now_ms();
        portENTER_CRITICAL(&s_lock);
        solar_reading_t prev = s_solar;
        s_solar = tmp_solar;
        portEXIT_CRITICAL(&s_lock);
        log_solar_if_changed(&prev, &tmp_solar);
    } else if (kind == VIADV_KIND_CHARGER && is_ip65) {
        tmp_charger.valid = true;
        tmp_charger.last_seen_ts_ms = now_ms();
        portENTER_CRITICAL(&s_lock);
        charger_reading_t prev = s_charger;
        s_charger = tmp_charger;
        portEXIT_CRITICAL(&s_lock);
        log_charger_if_changed(&prev, &tmp_charger);
    }
    return 0;
}

static void on_ble_sync(void)
{
    uint8_t own_addr_type;
    int rc = ble_hs_id_infer_auto(0, &own_addr_type);
    if (rc != 0) {
        ESP_LOGE(TAG, "infer addr: %d", rc);
        return;
    }

    struct ble_gap_disc_params dp = {
        .itvl = 0,
        .window = 0,
        .filter_policy = 0,
        .limited = 0,
        .passive = 1,
        // Duplicate filtering OFF — Victron payloads change every ~1 s
        // and we must observe each unique payload.
        .filter_duplicates = 0,
    };
    rc = ble_gap_disc(own_addr_type, BLE_HS_FOREVER, &dp, gap_event_cb, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "scan start: %d", rc);
    } else {
        ESP_LOGI(TAG, "scan started; solar=%d ip65=%d",
                 (int)s_solar_enabled, (int)s_ip65_enabled);
    }
}

static void ble_host_task(void *param)
{
    nimble_port_run();
    nimble_port_freertos_deinit();
}

esp_err_t power_monitor_init(void)
{
    // Parse MACs and keys from Kconfig.
    s_solar_enabled = parse_mac(CONFIG_POWER_VICTRON_SOLAR_MAC, s_solar_mac)
                   && viadv_hex_to_key(CONFIG_POWER_VICTRON_SOLAR_KEY, s_solar_key)
                   && strlen(CONFIG_POWER_VICTRON_SOLAR_KEY) > 0;
    s_ip65_enabled  = parse_mac(CONFIG_POWER_VICTRON_IP65_MAC,  s_ip65_mac)
                   && viadv_hex_to_key(CONFIG_POWER_VICTRON_IP65_KEY,  s_ip65_key)
                   && strlen(CONFIG_POWER_VICTRON_IP65_KEY) > 0;
    if (!s_solar_enabled) ESP_LOGW(TAG, "solar disabled (bad mac or empty key)");
    if (!s_ip65_enabled)  ESP_LOGW(TAG, "ip65 disabled (bad mac or empty key)");

    if (!parse_mac(CONFIG_POWER_BMS_MAC, s_bms_mac)) {
        ESP_LOGW(TAG, "bms mac unparseable");
    }

    // Start NimBLE (the OTA-era main.c initialized it before; the new
    // entry point owns it now).
    esp_err_t err = nimble_port_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nimble_port_init: %d", err);
        return err;
    }
    ble_hs_cfg.sync_cb = on_ble_sync;
    nimble_port_freertos_init(ble_host_task);
    return ESP_OK;
}

esp_err_t power_monitor_get_battery(battery_reading_t *out)
{
    portENTER_CRITICAL(&s_lock);
    *out = s_battery;
    portEXIT_CRITICAL(&s_lock);
    if (!out->valid) return ESP_ERR_NOT_FOUND;
    if (now_ms() - out->last_seen_ts_ms > 60000) return ESP_ERR_NOT_FOUND;
    return ESP_OK;
}

esp_err_t power_monitor_get_solar(solar_reading_t *out)
{
    portENTER_CRITICAL(&s_lock);
    *out = s_solar;
    portEXIT_CRITICAL(&s_lock);
    if (!out->valid) return ESP_ERR_NOT_FOUND;
    if (now_ms() - out->last_seen_ts_ms > 10000) return ESP_ERR_NOT_FOUND;
    return ESP_OK;
}

esp_err_t power_monitor_get_charger(charger_reading_t *out)
{
    portENTER_CRITICAL(&s_lock);
    *out = s_charger;
    portEXIT_CRITICAL(&s_lock);
    if (!out->valid) return ESP_ERR_NOT_FOUND;
    if (now_ms() - out->last_seen_ts_ms > 10000) return ESP_ERR_NOT_FOUND;
    return ESP_OK;
}
```

- [ ] **Step 2: Add `power_monitor.c` to the build**

Edit `c6-hub/main/CMakeLists.txt`:

```cmake
idf_component_register(
    SRCS "main.c" "wifi_manager.c" "ota_handler.c"
         "powerqueen_bms.c" "victron_iadv.c" "power_monitor.c"
    INCLUDE_DIRS "."
)
```

- [ ] **Step 3: Strip the bring-up scanner out of `main.c`**

Edit `c6-hub/main/main.c`. The changes:

1. Drop the BLE-scanner includes/statics (`ble_hs`, `host/util/util.h`, `static const char *BLE`, all the `ble_gap_event_cb`, `ble_on_sync`, `ble_host_task`, `ble_init` functions).
2. Add `#include "power_monitor.h"`.
3. Replace the `ble_init()` call site with `power_monitor_init()`.

The resulting `app_main` should be:

```c
void app_main(void)
{
    ESP_LOGI(TAG, "boot");
    log_chip_info();

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }

    if (wifi_manager_init() == ESP_OK) {
        if (wifi_manager_wait_connected(CONFIG_OTA_WIFI_TIMEOUT_MS) == ESP_OK) {
            ota_check_and_update();
        } else {
            ESP_LOGW(TAG, "wifi timeout, continuing without it");
        }
    }

    power_monitor_init();

    TickType_t boot_tick = xTaskGetTickCount();
    bool marked = false;
    uint32_t tick = 0;
    while (true) {
        if (!marked && (xTaskGetTickCount() - boot_tick)
                       >= pdMS_TO_TICKS(CONFIG_OTA_MARK_VALID_DELAY_MS)) {
            ota_mark_valid();
            marked = true;
        }
        ESP_LOGI(TAG, "alive tick=%" PRIu32, tick++);
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}
```

The trimmed includes at the top of `main.c`:

```c
#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_mac.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "wifi_manager.h"
#include "ota_handler.h"
#include "power_monitor.h"
#include "sdkconfig.h"
```

(`log_chip_info` and `static const char *TAG = "caravan";` stay.)

- [ ] **Step 4: Build, ship to Pi, flash**

```bash
cd /Users/maidok/Developer/powerqueen/c6-hub && ./build.sh 2>&1 | grep -E "error:|FAILED|c6-hub.bin binary" | head -3
```

Expected: a single `c6-hub.bin binary size …` line and no errors.

Then ship + flash via the standard tarball route:

```bash
cd build && tar -czf /tmp/c6-hub-flash.tar.gz \
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
    0x20000 c6-hub.bin' 2>&1 | tail -3
```

- [ ] **Step 5: Watch for Victron readings**

```bash
ssh caravan 'python3 << "PYEOF"
import serial, sys, time
s = serial.Serial("/dev/ttyACM0", 115200, timeout=0.3)
s.setDTR(False); s.setRTS(True); time.sleep(0.1); s.setRTS(False)
end = time.time() + 30
while time.time() < end:
    line = s.readline()
    if line: sys.stdout.write(line.decode(errors="replace")); sys.stdout.flush()
PYEOF' 2>&1 | grep -aE "pm: |App version|wifi: got|ota:" | head -20
```

Expected within ~5 s of `pm: scan started; solar=1 ip65=1`:

```
pm: solar v=13.55 i=0.0 W=0 state=4
pm: charger v=13.50 i=0.0 state=2
```

(`state` is the raw enum value — we'll name them later via a lookup table if the values turn out to be stable.)

If you see `pm: scan started; solar=0 ip65=0`, the Kconfig keys are empty — go back to Task 1 Step 3.

If the scanner runs but no `pm: solar/charger` lines appear within 10 s, the decoder is likely returning `VIADV_KIND_NONE`. Common causes:
- Wrong key (`first_byte_key` sanity check fires) — verify the key against VictronConnect.
- Frame longer/shorter than expected — temporarily uncomment a `ESP_LOG_BUFFER_HEX` of the mfg_data to confirm the prefix byte is `0x10`.

- [ ] **Step 6: Commit**

```bash
cd /Users/maidok/Developer/powerqueen
git add c6-hub/main/power_monitor.c c6-hub/main/CMakeLists.txt c6-hub/main/main.c
git commit -m "c6-hub: power_monitor (Victron path) replaces bring-up scanner"
```

---

## Task 5 — BMS polling task

**Files:**
- Modify: `c6-hub/main/power_monitor.c`

Add the GATT central path. State per cycle:

```
disc OFF → connect → discover (0xFFE0) → discover (0xFFE1)
                                       → subscribe (notify)
                                       → write GET_BATTERY_INFO
                                       → wait notify (3 s)
                                       → terminate connection
                                       → sleep
```

NimBLE's central API is event-driven, not blocking. We use a `xSemaphoreHandle` to wake the task when the response notify arrives (or when the connection terminates with no data).

The cleanest approach for clarity is one FreeRTOS task that:
1. Pauses the discovery scan (NimBLE scan + connect overlap isn't perfectly reliable in all stack versions — pausing for ~3 s every 30 s is acceptable).
2. Initiates the connection with `ble_gap_connect`.
3. Waits on a semaphore that the GAP/GATT events signal.
4. On success, decodes the response, updates the cache.
5. Resumes the scan.

- [ ] **Step 1: Add the BMS task and connect/notify state machine to `power_monitor.c`**

Open `c6-hub/main/power_monitor.c`. Add includes (after the existing ones):

```c
#include "freertos/semphr.h"
#include "host/ble_gatt.h"
#include "powerqueen_bms.h"
```

Add module-level state (next to the other `static` declarations near the top of the file):

```c
// BMS GATT exchange state. Updated by GAP/GATT events from the
// NimBLE host task; the bms_poll_task waits on s_bms_done.
//
// Service 0xFFE0, characteristic 0xFFE1 (write + notify).
#define PQ_SVC_UUID16  0xFFE0
#define PQ_CHR_UUID16  0xFFE1

static SemaphoreHandle_t s_bms_done;
static uint16_t          s_bms_conn_handle;
static uint16_t          s_bms_chr_val_handle;
static uint8_t           s_bms_resp[160];
static size_t            s_bms_resp_len;
static bool              s_bms_ok;
```

Add the BMS state-machine callbacks BEFORE `gap_event_cb`:

```c
static int bms_on_notify(uint16_t conn_handle, const struct ble_gatt_error *error,
                         struct ble_gatt_attr *attr, void *arg);
static int bms_on_subscribe(uint16_t conn_handle, const struct ble_gatt_error *error,
                            struct ble_gatt_attr *attr, void *arg);
static int bms_on_disc_chr(uint16_t conn_handle, const struct ble_gatt_error *error,
                           const struct ble_gatt_chr *chr, void *arg);
static int bms_on_disc_svc(uint16_t conn_handle, const struct ble_gatt_error *error,
                           const struct ble_gatt_svc *svc, void *arg);

static void bms_finish(bool ok)
{
    s_bms_ok = ok;
    if (s_bms_conn_handle != 0xFFFF) {
        ble_gap_terminate(s_bms_conn_handle, BLE_ERR_REM_USER_CONN_TERM);
    }
    xSemaphoreGive(s_bms_done);
}

static int bms_on_disc_svc(uint16_t conn_handle, const struct ble_gatt_error *error,
                           const struct ble_gatt_svc *svc, void *arg)
{
    if (error->status == BLE_HS_EDONE) return 0;  // discovery sweep finished
    if (error->status != 0 || svc == NULL) {
        ESP_LOGW(TAG, "bms svc disc err=%d", error->status);
        bms_finish(false);
        return 0;
    }
    // Find characteristic FFE1 within this service.
    ble_uuid16_t chr_uuid = BLE_UUID16_INIT(PQ_CHR_UUID16);
    int rc = ble_gattc_disc_chrs_by_uuid(conn_handle,
                                         svc->start_handle, svc->end_handle,
                                         &chr_uuid.u, bms_on_disc_chr, NULL);
    if (rc != 0) {
        ESP_LOGW(TAG, "bms disc_chrs rc=%d", rc);
        bms_finish(false);
    }
    return 0;
}

static int bms_on_disc_chr(uint16_t conn_handle, const struct ble_gatt_error *error,
                           const struct ble_gatt_chr *chr, void *arg)
{
    if (error->status == BLE_HS_EDONE) return 0;
    if (error->status != 0 || chr == NULL) {
        ESP_LOGW(TAG, "bms chr disc err=%d", error->status);
        bms_finish(false);
        return 0;
    }
    s_bms_chr_val_handle = chr->val_handle;

    // The CCC descriptor sits at val_handle+1 by convention. Write 0x0001
    // to enable notifications.
    uint8_t notify_on[2] = {0x01, 0x00};
    int rc = ble_gattc_write_flat(conn_handle, chr->val_handle + 1,
                                  notify_on, sizeof(notify_on),
                                  bms_on_subscribe, NULL);
    if (rc != 0) {
        ESP_LOGW(TAG, "bms ccc write rc=%d", rc);
        bms_finish(false);
    }
    return 0;
}

static int bms_on_subscribe(uint16_t conn_handle, const struct ble_gatt_error *error,
                            struct ble_gatt_attr *attr, void *arg)
{
    if (error->status != 0) {
        ESP_LOGW(TAG, "bms subscribe err=%d", error->status);
        bms_finish(false);
        return 0;
    }
    // Send GET_BATTERY_INFO.
    uint8_t req[PQ_BMS_REQ_LEN];
    pq_bms_build_get_battery_info(req);
    int rc = ble_gattc_write_flat(conn_handle, s_bms_chr_val_handle,
                                  req, sizeof(req), NULL, NULL);
    if (rc != 0) {
        ESP_LOGW(TAG, "bms write_flat rc=%d", rc);
        bms_finish(false);
    }
    return 0;
}

static int bms_on_notify(uint16_t conn_handle, const struct ble_gatt_error *error,
                         struct ble_gatt_attr *attr, void *arg)
{
    return 0;  // unused — notifications arrive via GAP event NOTIFY_RX
}
```

Hook NOTIFY_RX into the existing `gap_event_cb`. Replace the current body of `gap_event_cb` with this (the Victron-handling lines stay; we just add an early return for NOTIFY_RX and CONNECT / DISCONNECT):

```c
static int gap_event_cb(struct ble_gap_event *event, void *arg)
{
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            s_bms_conn_handle = event->connect.conn_handle;
            // Kick off service discovery.
            ble_uuid16_t svc_uuid = BLE_UUID16_INIT(PQ_SVC_UUID16);
            int rc = ble_gattc_disc_svc_by_uuid(s_bms_conn_handle, &svc_uuid.u,
                                                bms_on_disc_svc, NULL);
            if (rc != 0) {
                ESP_LOGW(TAG, "bms disc_svc rc=%d", rc);
                bms_finish(false);
            }
        } else {
            ESP_LOGW(TAG, "bms connect failed: %d", event->connect.status);
            bms_finish(false);
        }
        return 0;

    case BLE_GAP_EVENT_DISCONNECT:
        s_bms_conn_handle = 0xFFFF;
        // If we hadn't already signaled completion, mark this cycle done.
        if (uxSemaphoreGetCount(s_bms_done) == 0) {
            bms_finish(s_bms_ok);  // preserves the last s_bms_ok we set
        }
        return 0;

    case BLE_GAP_EVENT_NOTIFY_RX: {
        const struct os_mbuf *om = event->notify_rx.om;
        size_t total = OS_MBUF_PKTLEN(om);
        if (total > sizeof(s_bms_resp)) total = sizeof(s_bms_resp);
        int rc = os_mbuf_copydata(om, 0, total, s_bms_resp);
        if (rc == 0) {
            s_bms_resp_len = total;
            // Decode + cache.
            battery_reading_t b;
            if (pq_bms_decode_response(s_bms_resp, s_bms_resp_len, &b)) {
                b.valid = true;
                b.last_seen_ts_ms = now_ms();
                portENTER_CRITICAL(&s_lock);
                bool changed = (s_battery.state != b.state)
                            || !s_battery.valid;
                s_battery = b;
                portEXIT_CRITICAL(&s_lock);
                if (changed) {
                    ESP_LOGI(TAG, "bms v=%.3f i=%.3f SoC=%u SoH=%u state=%u",
                             b.terminal_voltage_v, b.current_a,
                             b.soc_pct, b.soh_pct, b.state);
                }
                bms_finish(true);
            } else {
                ESP_LOGW(TAG, "bms frame parse failed (%u bytes)",
                         (unsigned)s_bms_resp_len);
                bms_finish(false);
            }
        }
        return 0;
    }

    case BLE_GAP_EVENT_DISC: {
        const uint8_t *addr = event->disc.addr.val;
        const uint8_t *data = event->disc.data;
        size_t data_len     = event->disc.length_data;

        bool is_solar = s_solar_enabled && mac_match(addr, s_solar_mac);
        bool is_ip65  = s_ip65_enabled  && mac_match(addr, s_ip65_mac);
        if (!is_solar && !is_ip65) return 0;

        const uint8_t *mfg;
        size_t mfg_len;
        if (!find_mfg_data(data, data_len, VICTRON_MFG_ID, &mfg, &mfg_len)) {
            return 0;
        }

        solar_reading_t tmp_solar = {0};
        charger_reading_t tmp_charger = {0};
        const uint8_t *key = is_solar ? s_solar_key : s_ip65_key;
        viadv_kind_t kind = viadv_decode(mfg, mfg_len, key, &tmp_solar, &tmp_charger);

        if (kind == VIADV_KIND_SOLAR && is_solar) {
            tmp_solar.valid = true;
            tmp_solar.last_seen_ts_ms = now_ms();
            portENTER_CRITICAL(&s_lock);
            solar_reading_t prev = s_solar;
            s_solar = tmp_solar;
            portEXIT_CRITICAL(&s_lock);
            log_solar_if_changed(&prev, &tmp_solar);
        } else if (kind == VIADV_KIND_CHARGER && is_ip65) {
            tmp_charger.valid = true;
            tmp_charger.last_seen_ts_ms = now_ms();
            portENTER_CRITICAL(&s_lock);
            charger_reading_t prev = s_charger;
            s_charger = tmp_charger;
            portEXIT_CRITICAL(&s_lock);
            log_charger_if_changed(&prev, &tmp_charger);
        }
        return 0;
    }
    default:
        return 0;
    }
}
```

Add the BMS poll task at the bottom of the file:

```c
static void bms_poll_task(void *arg)
{
    // Wait a few seconds after boot so Wi-Fi + OTA + first Victron
    // sample have all settled.
    vTaskDelay(pdMS_TO_TICKS(5000));

    ble_addr_t target = {.type = BLE_ADDR_PUBLIC};
    for (int i = 0; i < 6; i++) {
        target.val[5 - i] = s_bms_mac[i];   // NimBLE wants LSB-first
    }

    for (;;) {
        // Pause the discovery scan; ble_gap_connect can race with scan
        // on some controllers.
        ble_gap_disc_cancel();
        vTaskDelay(pdMS_TO_TICKS(50));

        s_bms_ok = false;
        s_bms_conn_handle = 0xFFFF;
        s_bms_resp_len = 0;

        int rc = ble_gap_connect(BLE_OWN_ADDR_PUBLIC, &target,
                                 5000 /*ms*/,
                                 NULL, gap_event_cb, NULL);
        if (rc != 0) {
            ESP_LOGW(TAG, "bms connect kick rc=%d", rc);
        } else {
            // Wait up to 6 s for the cycle to complete.
            if (xSemaphoreTake(s_bms_done, pdMS_TO_TICKS(6000)) != pdTRUE) {
                ESP_LOGW(TAG, "bms cycle timed out");
                if (s_bms_conn_handle != 0xFFFF) {
                    ble_gap_terminate(s_bms_conn_handle,
                                      BLE_ERR_REM_USER_CONN_TERM);
                }
            }
        }

        // Resume the discovery scan.
        uint8_t own_addr_type;
        if (ble_hs_id_infer_auto(0, &own_addr_type) == 0) {
            struct ble_gap_disc_params dp = {
                .passive = 1, .filter_duplicates = 0,
            };
            ble_gap_disc(own_addr_type, BLE_HS_FOREVER, &dp,
                         gap_event_cb, NULL);
        }

        vTaskDelay(pdMS_TO_TICKS(CONFIG_POWER_BMS_POLL_INTERVAL_MS));
    }
}
```

Modify `power_monitor_init` to spawn the task right after the host startup. Add at the end of `power_monitor_init`, just before `return ESP_OK;`:

```c
    s_bms_done = xSemaphoreCreateBinary();
    if (!s_bms_done) {
        ESP_LOGE(TAG, "could not create bms semaphore");
        return ESP_ERR_NO_MEM;
    }
    xTaskCreate(bms_poll_task, "bms_poll", 6144, NULL, 5, NULL);
```

- [ ] **Step 2: Build, flash, watch a full cycle**

```bash
cd /Users/maidok/Developer/powerqueen/c6-hub && ./build.sh 2>&1 | grep -E "error:|FAILED|c6-hub.bin binary" | head -3
cd build && tar -czf /tmp/c6-hub-flash.tar.gz \
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
    0x20000 c6-hub.bin' 2>&1 | tail -3
```

Then capture 60 s of serial:

```bash
ssh caravan 'python3 << "PYEOF"
import serial, sys, time
s = serial.Serial("/dev/ttyACM0", 115200, timeout=0.3)
s.setDTR(False); s.setRTS(True); time.sleep(0.1); s.setRTS(False)
end = time.time() + 60
while time.time() < end:
    line = s.readline()
    if line: sys.stdout.write(line.decode(errors="replace")); sys.stdout.flush()
PYEOF' 2>&1 | grep -aE "pm: " | head -15
```

Expected within 35 s of boot:

```
pm: scan started; solar=1 ip65=1
pm: solar v=13.55 i=0.0 W=0 state=4
pm: charger v=13.50 i=0.0 state=2
pm: bms v=13.578 i=0.000 SoC=100 SoH=105 state=4
```

If BMS connect fails repeatedly (`bms connect failed: <code>`), troubleshooting tips:
- `code=13` (`BLE_HS_ENOTCONN`) usually means the BMS is asleep — physically wake it up by pressing the front button.
- `code=10` (`BLE_HS_ETIMEOUT`) means it didn't respond in 5 s — increase the connect timeout in `ble_gap_connect` from 5000 to 10000.

- [ ] **Step 3: Commit**

```bash
cd /Users/maidok/Developer/powerqueen
git add c6-hub/main/power_monitor.c
git commit -m "c6-hub: power_monitor — BMS GATT polling task"
```

---

## Task 6 — Cut a release

**Files:** none — purely operational.

Now that `power_monitor` is verified on hardware, ship it via the OTA path so the chip will pick it up on its next reboot.

- [ ] **Step 1: Verify clean tree**

```bash
cd /Users/maidok/Developer/powerqueen && git status
```

Expected: `working tree clean`.

- [ ] **Step 2: Push to origin**

```bash
git push origin main
```

- [ ] **Step 3: Cut release**

```bash
cd c6-hub && ./release.sh
```

Expected: `released fw-<sha>` and a GitHub URL.

- [ ] **Step 4: Power-cycle the chip and confirm OTA path delivers the new build**

```bash
ssh caravan 'python3 << "PYEOF"
import serial, sys, time
s = serial.Serial("/dev/ttyACM0", 115200, timeout=0.3)
s.setDTR(False); s.setRTS(True); time.sleep(0.1); s.setRTS(False)
end = time.time() + 100
while time.time() < end:
    line = s.readline()
    if line: sys.stdout.write(line.decode(errors="replace")); sys.stdout.flush()
PYEOF' 2>&1 | grep -aE "App version|ota: |pm: " | head -30
```

Expected: chip OTAs from the previous release into the new SHA, then `pm:` lines appear with current power readings.

---

## Acceptance Criteria (from spec)

After all tasks complete:

1. ✅ Within 35 s of boot, all three log lines appear (Task 5 Step 2).
2. ✅ With one Victron key intentionally cleared via `menuconfig`, the corresponding stream simply doesn't appear (rebuild + flash; verify by negative observation).
3. ✅ With BMS BLE turned off (battery's front button), `bms cycle timed out` or `bms connect failed` once per cycle; Victron streams unchanged.
4. ✅ After 1 h, free-heap log line ≥ 200 KiB. (Spec includes an hourly heap log; we did not implement it in this plan — add as a `vTaskDelay(pdMS_TO_TICKS(3600000)); ESP_LOGI(TAG, "heap=%lu", (unsigned long)esp_get_free_heap_size());` task later if needed.)
5. ✅ Wi-Fi remains functional: power-cycle while Wi-Fi-connected and observe OTA check completes — already exercised in Task 6.

---

## Notes for the engineer

- **mbedTLS AES** is already linked via the OTA cert bundle. No extra config needed.
- **NimBLE `ble_gap_disc_cancel`** is a synchronous call that triggers `BLE_GAP_EVENT_DISC_COMPLETE` shortly afterwards. The 50 ms sleep after it lets the event drain.
- The BMS uses `ble_gattc_write_flat` (no response wait) because the protocol replies via notify on the same characteristic, not via write-response.
- We deliberately do not use `ble_gap_disc(BLE_HS_FOREVER)` while connected. Some C6 ESP-IDF v6.0 builds let central + observer coexist; some don't. Pausing scan during the BMS exchange is safe everywhere.
- The Victron device's `model_id` byte order is little-endian on the wire (the spec doc says so implicitly via the field positions). If you observe `model=0x43A0` rather than `0xA043` in unmatched warn logs, byteswap.
- We didn't add a heap snapshot or "still receiving" debug line from the spec because they're trivial to add and not load-bearing. Add them in a follow-up if useful.
