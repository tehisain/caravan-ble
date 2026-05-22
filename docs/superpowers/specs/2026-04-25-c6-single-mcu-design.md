# Single-MCU Caravan Hub on ESP32-C6 SuperMini — Design Spec

**Status:** Draft for user review
**Author:** Claude + Ain
**Date:** 2026-04-25
**Supersedes:** The two-MCU architecture (ESP32-S3 + XIAO nRF52840 RCP) merged on 2026-04-24.

## Summary

Replace the existing ESP32-S3 + XIAO nRF52840 architecture with a single ESP32-C6 SuperMini. The C6's native 802.15.4 radio eliminates the dedicated RCP, removing one MCU, one UART link, and the entire `xiao-rcp/` subsystem. SIM7600 cellular moves from USB host (which the C6 lacks) to the C6's LP_UART using AT-mode commands and GPS-via-AT. Wi-Fi is kept active for OTA and home-network backhaul.

## Goals

1. One-MCU bill of materials, fewer wires, simpler firmware build.
2. Espressif-supported "C6 as all-in-one OTBR" pattern — better long-term ESP-IDF support than the unusual S3+nRF52840-via-UART setup.
3. No regression in the documented feature set (state machine, sensors, IKEA Thread sensors via Matter, BLE BMS, BLE phone control, SMS/GPS).

## Non-Goals

- Sub-second GPS updates (poll-based GPS-via-AT at ~1 Hz is sufficient for caravan use).
- Aggressive deep-sleep — incompatible with C6 acting as the Thread router.
- Wi-Fi captive-portal provisioning (BLE GATT and USB-CDC are sufficient).

## Hardware

### Bill of Materials Changes

| Item | Before | After |
|---|---|---|
| Main MCU | ESP32-S3 DevKit | ESP32-C6 SuperMini (target: 16 MB or 8 MB flash variant) |
| Thread radio | XIAO nRF52840 Plus running RCP firmware | Built into C6 |
| UART RCP cable | Required (D6/D7 ↔ GPIO1/2) | **Removed** |
| Buzzer | Discrete on GPIO 20 | **Removed** — T-Encoder Pro's onboard buzzer used over the existing UART |
| SIM7600 link | USB host on S3 | LP_UART on C6 (AT mode + GPS-via-AT) |

The development board for initial bring-up is the existing 4 MB **ESP32-C6FH4** SuperMini already in hand. A 16 MB variant is on order to enable dual-OTA partitions in the production flash layout. App code is portable between the two — only the partition table differs.

### GPIO Map (ESP32-C6 SuperMini)

| GPIO | Function | Direction | Protocol | Notes |
|------|----------|-----------|----------|-------|
| 0 | Water level | Input | ADC1_CH0 | A0, voltage divider |
| 1 | Shore power detect | Input | GPIO | Active LOW (PC817) |
| 2 | Car connection detect | Input | GPIO | Active LOW (PC817) |
| 3 | DS18B20 outdoor temp | Bidir | 1-Wire | 4.7 kΩ pull-up |
| 4 | I2C SDA | Bidir | I2C | INA219 ×2, MPU6500. Sacrifices hardware JTAG (MTMS) |
| 5 | I2C SCL | Output | I2C | 400 kHz. Sacrifices hardware JTAG (MTDI) |
| 6 | LP_UART RX | Input | UART | SIM7600 → C6 |
| 7 | LP_UART TX | Output | UART | C6 → SIM7600, 115200 baud |
| 8 | SIM7600 power gate | Output | GPIO | AO3401 P-FET. Disables onboard RGB LED + ROM-message print |
| 14 | (free / future relay) | — | — | Reserved |
| 20 | UART1 RX | Input | UART | VE.Direct from MPPT, 19200 baud |
| 21 | UART1 TX | Output | UART | VE.Direct to MPPT (via TXS0102 level shifter) |
| 22 | T-Encoder TX | Output | SW UART | JST connector to display |
| 23 | T-Encoder RX | Input | SW UART | 115200 baud |

**Reserved / cannot use:** GPIO 9 (BOOT), 12, 13 (USB-Serial-JTAG), 15 (status LED), 18, 19 (internal SPI flash).

13 GPIOs used out of 15 usable; GPIO 14 reserved for a future relay channel.

### Wiring Tradeoffs Accepted

- **Hardware JTAG gone** — USB-Serial-JTAG over the USB-C port still works, which covers normal flashing, console, and debugging. Cannot connect external J-Link.
- **Onboard RGB LED gone** — status comes from T-Encoder display.
- **ROM-message print disabled at boot** — unimportant; ESP-IDF logging over USB-Serial covers everything.

## Software Architecture

### Concurrent Stacks on the C6

| Stack | Role | Notes |
|---|---|---|
| OpenThread Border Router | Thread network for IKEA sensors | C6 acts as router, not sleepy |
| Matter (Thread + BLE commissioning) | Drives IKEA Matter-over-Thread devices | Required — IKEA sensors are Matter |
| Wi-Fi 6 (STA mode) | OTA, home-network backhaul when parked | Idle most of the time |
| BLE central | Power Queen BMS readout | Periodic poll |
| BLE peripheral (GATT) | Phone control + status read | Always advertising |

ESP-IDF supports BLE + 802.15.4 + Wi-Fi simultaneously on the C6 via radio time-multiplexing. Thin SRAM headroom — see Risks.

### UART Allocation

| UART | Role | Pins | Baud |
|---|---|---|---|
| UART0 | USB-Serial-JTAG console | (internal) | 115 200 |
| UART1 | VE.Direct to Victron MPPT | GPIO 21 (TX), 20 (RX) | 19 200 |
| LP_UART | SIM7600 AT + GPS-via-AT | GPIO 7 (TX), 6 (RX) | 115 200 |
| Software UART | T-Encoder Pro (status JSON + buzzer commands) | GPIO 22 (TX), 23 (RX) | 115 200 |

The LP_UART choice for SIM7600 gives us optional power savings — AT polling can continue during light-sleep windows without waking the main UART peripheral.

### Module Layout (ESP-IDF Components)

The existing `esp32-hub/main/` directory is renamed `c6-hub/main/` and the module list updates:

```
c6-hub/main/
├── main.c                  # Entry point, sleep/run management
├── state_machine.c         # OFFGRID / CAMPSITE / TOWING / ARRIVED
├── thread_otbr.c           # Native C6 OpenThread Border Router
├── matter_controller.c     # ESP-Matter device pairing + attribute reads
├── wifi_manager.c          # STA connect, OTA trigger, backhaul state
├── ota_handler.c           # HTTPS-pull OTA via esp_https_ota
├── victron_vedirect.c      # VE.Direct text protocol parser (UART1)
├── power_monitor.c         # INA219 ×2 + shore/car detect
├── sensors.c               # DS18B20, water level (ADC), MPU6500
├── ble_bms.c               # Power Queen BMS client (BLE central)
├── ble_server.c            # Phone-app GATT server (BLE peripheral)
├── sms_handler.c           # SIM7600 AT commands over LP_UART
├── gps.c                   # GPS-via-AT polling (CGNSPWR/CGNSINF)
├── display_link.c          # T-Encoder Pro UART protocol (status + buzzer)
├── alerts.c                # Threshold monitoring
├── rules_engine.c          # Configurable automation
└── config.c                # NVS storage for settings
```

### Updated Communication Protocols

| Link | Protocol | Speed | Purpose |
|---|---|---|---|
| C6 ↔ Victron MPPT | UART1 | 19 200 | VE.Direct text |
| C6 ↔ T-Encoder Pro | SW UART | 115 200 | JSON status + buzzer commands |
| C6 ↔ SIM7600 | LP_UART | 115 200 | AT commands, SMS, GPS-via-AT |
| C6 ↔ Power Queen | BLE | — | BMS readout |
| C6 ↔ Phone | BLE GATT | — | Status read, config write |
| C6 ↔ IKEA sensors | 802.15.4 (Matter) | — | Native, no RCP |
| C6 ↔ Wi-Fi AP | Wi-Fi 6 STA | — | OTA + backhaul (when home) |

### T-Encoder Protocol Additions

Existing JSON status push from C6 → T-Encoder is unchanged. **New addition: buzzer commands.**

```json
{"buzzer": {"pattern": "level_ok"}}
{"buzzer": {"pattern": "level_warning"}}
{"buzzer": {"pattern": "alert"}}
{"buzzer": {"freq": 2000, "duration_ms": 100}}
```

The display's onboard ESP32-S3 firmware drives its local buzzer (GPIO17 of the T-Encoder module). When the display is asleep (OFFGRID), buzzer requests are no-ops — critical alerts are still covered by:
- IKEA KLIPPBOK leak sensor (built-in siren)
- SMS alerts via SIM7600 for low battery, motion, doors, freezing risk

## Power Profile (Honest Revision)

The original "1% duty cycle, ~8 mA OFFGRID average" was unrealistic for a Thread-router-host. The C6 is now the 802.15.4 router and must stay reachable for the IKEA sensors at all times.

| State | Mode | Estimated avg current | Notes |
|---|---|---|---|
| OFFGRID | Light sleep with Thread router awake | 30–50 mA | Conservative; instrumented in Task TBD |
| CAMPSITE | Active | 80–120 mA | Wi-Fi connected, display always on |
| TOWING | Active | 100–150 mA | GPS continuous, SMS standby |
| ARRIVED | Active | 80–120 mA | Same as CAMPSITE |

100 Ah battery in OFFGRID still gives **~80–140 days** standby — far less than the original document's 520 days, but honest and acceptable for the use case.

## Repository Changes

1. **Delete** `xiao-rcp/` directory and all references to it.
2. **Rename** `esp32-hub/` to `c6-hub/`. Module list above.
3. **Rewrite** root `README.md` sections: Hardware Components, GPIO Assignments, I2C addresses (unchanged content but pin numbers in narrative may shift), Communication Protocols, Architecture Overview ASCII, Repository Layout, Power Budget, Key Design Decisions, Next Steps, References.
4. **Update** `docs/architecture.mermaid`, `docs/dataflow.mermaid`, `docs/wiring.mermaid` to remove the XIAO node and the ESP32↔XIAO UART link.
5. **Keep** `docs/state_machine.mermaid` (state logic unchanged).
6. **Keep** `tools/ble-probe/` (Python BLE reference scripts — unchanged).
7. **Add** entry to `docs/architecture-history/` (or just rely on git log) noting the architecture pivot.

## Risks and Open Questions

1. **4 MB flash on the bring-up board** — Matter + OTBR + Wi-Fi + app + dual-OTA does not fit in 4 MB. Mitigation: bring up using single-app-slot layout on the current board; switch to dual-OTA partitions when the 16 MB SuperMini arrives. App code is portable; only the partition table differs.

2. **SRAM headroom** — 512 KB SRAM with all stacks active is officially supported by Espressif but thin. We will instrument heap on every boot and log peak usage. First feature to drop if we run out: BLE peripheral (move phone control to a Wi-Fi/HTTP API instead). Matter is non-negotiable because of IKEA sensor support.

3. **Honest power numbers** — old README's 8 mA OFFGRID was a fiction. Real numbers will come from a measurement task during implementation.

4. **No external JTAG** — USB-Serial-JTAG over the USB-C port still works for normal debugging.

5. **Counterfeit AliExpress flash sizes** — verify the incoming 16 MB board with `esptool.py flash_id` before trusting the listing.

## Out of Scope

- VE.Direct cable build (still needed; same TXS0102 level shifter).
- Detection circuit wiring (PC817 optos, HLK-PM01, voltage divider — unchanged).
- T-Encoder Pro firmware development (same display, same UART protocol with the buzzer addition).
- Power Queen BLE protocol porting (same BLE protocol).

## Implementation Findings (2026-05-22 update)

Captured below are deltas discovered during the OTA bring-up. Treat the
rest of the spec as the original design and these as the actual state.

### Hardware on hand

- The "16 MB ESP32-C6 SuperMini" we ordered turned up with a **Boya 16 MB
  embedded flash** (vendor ID `0x68`, device `0x4018`). esptool detected
  the chip's Boya driver was not enabled by default and printed a soft
  warning. Final sdkconfig has `CONFIG_SPI_FLASH_SUPPORT_BOYA_CHIP=y`.
- The original 4 MB ESP32-C6FH4 bring-up board has been retired in
  favor of the 16 MB SuperMini for the in-caravan install. Both boards
  share the same app, only the partition table differs.

### Additional BLE devices discovered on-site

A passive BLE scan from inside the caravan turned up two devices the
spec didn't anticipate:

| MAC | Name | Source | Implication |
|---|---|---|---|
| `da:02:0f:ca:6d:1b` | `SmartSolar HQ2216XZQ42` | Victron MPPT | Provides solar telemetry over BLE in addition to VE.Direct UART |
| `e9:3c:6f:d4:a6:08` | `BSC IP65 12/25 HQ2427EXUK7` | Victron IP65 shore charger | **Only** telemetry path — no VE.Direct port on the IP65 |

Both broadcast Victron's "Instant Readout" in manufacturer data (no
GATT connection required; encrypted with a per-device key from
VictronConnect). Consequences:

- VE.Direct cable build moves from "required" to "optional, redundant"
  if BLE turns out to be sufficient for MPPT readings.
- A new BLE module will need access to all three power-related
  advertisers (Power Queen BMS + SmartSolar + IP65).

### OTA implementation realities

The OTA-self-update spec at `2026-05-16-ota-self-update-design.md`
captures the design. Two implementation realities the design doc didn't
mention but that ended up mattering:

- **`esp_https_ota` cannot follow GitHub's release-asset 302 itself.**
  The download URL `github.com/.../releases/download/<tag>/<asset>`
  302-redirects to a signed `release-assets.githubusercontent.com` URL.
  The OTA library's connection setup doesn't handle that hop reliably,
  so `ota_handler.c` does a manual HEAD via `esp_http_client`, captures
  `Location` via the HTTP event callback, and hands the resolved URL to
  `esp_https_ota`. URL buffer is sized 2 KiB; the resolved URL is
  ~900–1500 bytes (long JWT + Azure SAS params).
- **Default HTTP buffers (512 bytes) overflow on the GET.** The
  request line alone exceeds 512 bytes once the signed URL is in it.
  Final sdkconfig uses `buffer_size = 4096`, `buffer_size_tx = 4096` in
  the OTA `esp_http_client_config_t`.
- **Default main-task stack (3.5 KB) overflows during mbedTLS
  handshake** with `CONFIG_MBEDTLS_CERTIFICATE_BUNDLE_DEFAULT_FULL=y`.
  Bumped to `CONFIG_ESP_MAIN_TASK_STACK_SIZE=12288`.
- **`build.sh` originally called `idf.py set-target esp32c6` on every
  build**, which silently wiped `sdkconfig` (including menuconfig-set
  Wi-Fi credentials). Now only invoked when `sdkconfig` is absent.

### Version-comparison anchor (resolved 2026-05-22)

The original OTA design used `tag != "fw-" + running` (string
inequality). This allowed silent **downgrades** to whichever release
GitHub's `/latest` endpoint currently named, observed during rollback
testing when /latest briefly fell back to an older release.

Resolution: `ota_handler.c` now parses each release's `published_at`
ISO8601 timestamp, stores it in NVS (ns=`ota`, key=`last_pub_ts`) when
the chip observes that the running build matches /latest, and refuses
to OTA into anything whose `published_at` is ≤ the stored anchor. The
anchor only moves forward in time. First-boot behaviour (anchor=0) is
identical to the old design — any release is accepted, so chips
freshly flashed via esptool will pick up the latest release on first
Wi-Fi connection.

Also collapsed: `PROJECT_VER` was being polluted by historical `fw-*`
release tags (git describe falls back to nearest tag). The 10 test
tags from OTA bring-up have been deleted; future tag-pollution is
unlikely because production tags only appear when `release.sh` cuts
them, and there will be relatively few.
