# Caravan Automation System

Low-power monitoring and automation system for a towable caravan, built around an ESP32-C6 SuperMini running OpenThread Border Router with Matter-compatible sensors.

---

## Project Goals

- **Monitor** battery state, solar production, temperatures, water level, doors, leaks, motion
- **Detect** power source (shore power, car connection, off-grid)
- **Alert** via SMS for critical events (leak, intrusion, low battery)
- **Display** status on local AMOLED screen (T-Encoder Pro)
- **Control** via phone app (BLE) or rotary encoder
- **Level** caravan using gyroscope with audio feedback
- **Track** GPS location while towing
- **Future-proof** for relay control without implementing it now

---

## Architecture Overview

```
┌─────────────────────────────────────────────────────────────────┐
│                    ESP32-C6 SuperMini HUB                       │
│                                                                 │
│  ┌────────┐ ┌────────┐ ┌────────┐ ┌────────┐ ┌────────┐         │
│  │ Thread │ │ Matter │ │ Wi-Fi  │ │  BLE   │ │ State  │         │
│  │  OTBR  │ │  Ctrl  │ │ + OTA  │ │  C+P   │ │Machine │         │
│  └────┬───┘ └────┬───┘ └────────┘ └────┬───┘ └────────┘         │
│       │          │                     │                        │
│  ┌────┴──────────┴───┐  ┌──────────────┴───┐  ┌────────┐        │
│  │ 802.15.4 Radio    │  │ BLE 5.3 Radio    │  │ VE.Dir │        │
│  │ (native)          │  │                  │  │ Parser │        │
│  └────┬──────────────┘  └────┬─────────────┘  └────┬───┘        │
└───────┼──────────────────────┼─────────────────────┼────────────┘
        │                      │                     │
        ▼                      ▼                     ▼
   ┌─────────┐            ┌─────────┐           ┌─────────┐
   │  IKEA   │            │ Power   │           │ Victron │
   │ Thread  │            │ Queen   │           │  MPPT   │
   │ Sensors │            │  BMS    │           │VE.Direct│
   └─────────┘            └─────────┘           └─────────┘
```

---

## Hardware Components

| Component | Purpose | Interface |
|-----------|---------|-----------|
| ESP32-C6 SuperMini | Main controller (Thread + Wi-Fi + BLE + Matter) | — |
| T-Encoder Pro | AMOLED display + rotary encoder + buzzer | UART (JST) |
| SIM7600SA-MNSE | 4G LTE + GPS | UART (AT mode + GPS-via-AT) |
| MPU6500 | Gyroscope/accelerometer | I2C |
| Victron MPPT 75/15 | Solar charge controller | VE.Direct |
| Victron IP65 12V 25A | Shore power charger | — |
| Power Queen 100Ah | LiFePO4 battery with BLE BMS | BLE |
| Dometic SMP 301-01 | 230V AC → 12V DC | — |
| 13-pin Euro connector | Tow vehicle interface | — |
| Water level gauge | 0-190Ω resistive | ADC |
| SimBase data SIM | 4G connectivity | — |
| INA219 ×2 | Current/power monitor (main bus, fridge) | I2C |
| PC817 ×3 | Optocoupler isolation (shore/car detect) | GPIO |
| HLK-PM01 | 230V AC → 5V DC for shore detection | — |
| AO3401 | P-channel MOSFET, SIM7600 power gate | GPIO |
| TXS0102 | Bidirectional level shifter (VE.Direct) | — |
| DS18B20 | Waterproof outdoor temp sensor | 1-Wire |
| IKEA TIMMERFLOTTE | Indoor temp/humidity | Thread |
| IKEA MYGGBETT ×2 | Door/window sensor | Thread |
| IKEA KLIPPBOK | Water leak sensor (built-in siren) | Thread |
| IKEA MYGGSPRAY | Motion sensor | Thread |
| Resistors, JST-PH connectors, prototype PCB | Assembly | — |

---

## GPIO Assignments

| GPIO | Function | Direction | Protocol | Notes |
|------|----------|-----------|----------|-------|
| 0 | Water level | Input | ADC | A0, voltage divider |
| 1 | Shore power detect | Input | GPIO | Active LOW (PC817) |
| 2 | Car connection detect | Input | GPIO | Active LOW (PC817) |
| 3 | DS18B20 outdoor temp | Bidir | 1-Wire | 4.7 kΩ pull-up to 3.3V |
| 4 | I2C SDA | Bidir | I2C | INA219 ×2, MPU6500 |
| 5 | I2C SCL | Output | I2C | 400 kHz |
| 6 | LP_UART RX | Input | UART | SIM7600 → C6 |
| 7 | LP_UART TX | Output | UART | C6 → SIM7600, 115200 baud |
| 8 | SIM7600 power gate | Output | GPIO | AO3401 P-FET (disables onboard RGB LED) |
| 14 | Reserved | — | — | Future relay channel |
| 20 | UART1 RX | Input | UART | VE.Direct from MPPT, 19200 baud |
| 21 | UART1 TX | Output | UART | VE.Direct via TXS0102 level shifter |
| 22 | T-Encoder TX | Output | SW UART | JST connector to display |
| 23 | T-Encoder RX | Input | SW UART | 115200 baud |
| USB | USB-Serial-JTAG | — | USB | Flashing + console + JTAG |
| BLE | Power Queen + Phone | — | BLE 5.3 | Central + Peripheral |
| RF | IKEA sensors | — | 802.15.4 | Native, Matter-over-Thread |

**Reserved / cannot use:** GPIO 9 (BOOT button), 12, 13 (USB D-/D+), 15 (status LED), 18, 19 (internal SPI flash).

---

## I2C Device Addresses

| Address | Device | Data |
|---------|--------|------|
| 0x40 | INA219 #1 | Main bus voltage, current, power |
| 0x41 | INA219 #2 | Fridge circuit voltage, current, power |
| 0x68 | MPU6500 | Pitch, roll, acceleration |

---

## Detection Circuits

### Shore Power Detection (230V AC)

```
230V AC ──► HLK-PM01 ──► 5V ──► 1kΩ ──► PC817 LED
                                              │
                                             GND

3.3V ──► 10kΩ pull-up ──┬──► GPIO1
                        │
                   PC817 collector
                        │
                       GND

Logic: Shore ON → GPIO1 LOW | Shore OFF → GPIO1 HIGH
```

### Car Connection Detection (13-Pin Connector)

```
Pin 10 (12V) ──► 10kΩ ──┬──► 10kΩ ──► GND     (voltage divider)
                        │
                        └──► 1kΩ ──► PC817 LED
                                          │
                                         GND

3.3V ──► 10kΩ pull-up ──┬──► GPIO2
                        │
                   PC817 collector
                        │
                       GND

Logic: Car ON → GPIO2 LOW | Car OFF → GPIO2 HIGH
```

### Water Level Sensing

```
3.3V ──► 100Ω ──┬──► GPIO0 (ADC)
                │
           Tank Sensor (0-190Ω)
                │
               GND

Voltage: Empty (0Ω) = 0.00V | Full (190Ω) = 2.16V
ADC: Empty ≈ 0 | Full ≈ 2680 (of 4095)
```

### Outdoor Temperature (DS18B20)

```
3.3V ──► 4.7kΩ ──┬──► GPIO3 (1-Wire data)
                 │
            DS18B20 DATA (yellow)
            DS18B20 VCC (red) ──► 3.3V
            DS18B20 GND (black) ──► GND
```

---

## VE.Direct Connection

Victron MPPT uses 5V TTL; ESP32-C6 uses 3.3V. Level shifter required.

```
Victron JST-PH          TXS0102              ESP32-C6
─────────────          ─────────             ────────
Pin 1: GND ──────────► GND ◄────────────────► GND
Pin 2: RX  ◄───────── HV1 ◄──── LV1 ◄─────── GPIO21 (TX)
Pin 3: TX  ──────────► HV2 ────► LV2 ───────► GPIO20 (RX)
Pin 4: +5V ──────────► HV (ref)
                       LV (ref) ◄───────────► 3.3V
```

---

## State Machine

| GPIO1 (Shore) | GPIO2 (Car) | State | Description |
|----------------|--------------|-------|-------------|
| HIGH | HIGH | **OFFGRID** | Battery only, wild camping |
| LOW | HIGH | **CAMPSITE** | Shore power connected |
| HIGH | LOW | **TOWING** | Car connected, driving |
| LOW | LOW | **ARRIVED** | Both connected, just parked |

### State Behaviors

| Feature | OFFGRID | CAMPSITE | TOWING | ARRIVED |
|---------|---------|----------|--------|---------|
| Sleep mode | Light sleep | Off | Off | Off |
| GPS tracking | On request | Off | Continuous | Off |
| Motion alerts | On | Optional | Suppressed | Off |
| Sway detection | Off | Off | Active | Off |
| Leveling mode | Off | On request | Off | Active |
| Display | On-demand | Always on | Speed/stability | Always on |
| SIM7600 | Power-gated | Power-gated | On | Power-gated |

---

## Data Points

### Thread Sensors (IKEA Matter-over-Thread)

| Sensor | Model | Data | Location |
|--------|-------|------|----------|
| Indoor climate | TIMMERFLOTTE | Temperature, humidity | Living area |
| Main door | MYGGBETT | Open/closed | Entry door |
| Storage door | MYGGBETT | Open/closed | External storage |
| Water leak | KLIPPBOK | Leak detected, built-in siren | Under sink |
| Motion | MYGGSPRAY | Motion detected | Living area |

### Wired Sensors

| Sensor | Interface | Data |
|--------|-----------|------|
| Outdoor temp | DS18B20 (1-Wire) | Temperature °C |
| Water tank | Resistive (ADC) | Level 0-100% |
| Main current | INA219 #1 (I2C) | Voltage, current, power |
| Fridge current | INA219 #2 (I2C) | Voltage, current, power |
| Pitch/Roll | MPU6500 (I2C) | Orientation, acceleration |

### BLE Sensors

| Sensor | Protocol | Data |
|--------|----------|------|
| Power Queen BMS | BLE (proprietary) | SoC, voltage, current, cell voltages, temp, cycles |

### Serial Sensors

| Sensor | Interface | Data |
|--------|-----------|------|
| Victron MPPT | VE.Direct (UART) | Panel V/I, battery V/I, yield, charge state |
| SIM7600 GPS | USB (NMEA) | Latitude, longitude, speed, heading |

---

## Communication Protocols

| Link | Protocol | Speed | Purpose |
|------|----------|-------|---------|
| C6 ↔ Victron MPPT | UART1 | 19200 | VE.Direct text protocol |
| C6 ↔ T-Encoder Pro | SW UART | 115200 | JSON status + buzzer commands |
| C6 ↔ SIM7600 | LP_UART | 115200 | AT commands, SMS, GPS-via-AT |
| C6 ↔ Power Queen | BLE | — | Proprietary BMS protocol |
| C6 ↔ Phone | BLE GATT | — | Status read, config write |
| C6 ↔ IKEA sensors | 802.15.4 (Matter) | — | Native, no RCP |
| C6 ↔ Wi-Fi AP | Wi-Fi 6 STA | — | OTA + home backhaul |

---

## T-Encoder Pro Protocol

### C6 → T-Encoder (status updates + buzzer)

```json
{
  "ts": 1711700000,
  "state": "CAMPSITE",
  "battery": {"soc": 78, "voltage": 13.2, "current": 2.5, "power": 33},
  "solar": {"voltage": 18.5, "power": 45, "daily": 320},
  "temp": {"indoor": 22.5, "outdoor": 18.2, "humidity": 55},
  "water": {"level": 65},
  "doors": {"main": "closed", "storage": "closed"},
  "level": {"pitch": 1.2, "roll": -0.5}
}
```

**Buzzer commands** (sent in addition to status updates — display fires its onboard buzzer):

```json
{"buzzer": {"pattern": "level_ok"}}
{"buzzer": {"pattern": "level_warning"}}
{"buzzer": {"pattern": "alert"}}
{"buzzer": {"freq": 2000, "duration_ms": 100}}
```

### T-Encoder → C6 (user input)

```json
{"event": "encoder", "direction": "cw", "clicks": 3}
{"event": "button", "action": "press"}
{"event": "button", "action": "long_press"}
{"event": "touch", "x": 120, "y": 80}
{"event": "menu", "select": "level_mode"}
```

---

## SMS Commands

| Command | Response |
|---------|----------|
| `STATUS` | Battery SoC, voltage, solar power, temperatures, GPS |
| `POWER` | Detailed power info (main bus, fridge, charging) |
| `GPS` | Coordinates + Google Maps link |
| `HELP` | List available commands |

---

## Alert Conditions (Configurable)

| Condition | Default Threshold | Action |
|-----------|-------------------|--------|
| Low battery | SoC < 20% | SMS alert |
| Water leak | Detected | SMS alert + buzzer |
| Door open (OFFGRID) | Any door | SMS alert |
| Motion (OFFGRID) | Detected | SMS alert |
| Excessive sway | > 0.3g lateral | Buzzer warning |
| High temperature | > 35°C indoor | SMS alert |
| Freezing risk | < 5°C indoor | SMS alert |

---

## Power Budget

| State | Mode | Estimated avg current | Notes |
|-------|------|----------------------|-------|
| OFFGRID | Light sleep, Thread router awake | 30–50 mA | Conservative; instrument early in firmware |
| CAMPSITE | Active, Wi-Fi connected | 80–120 mA | Display always on |
| TOWING | Active, GPS continuous | 100–150 mA | SMS standby |
| ARRIVED | Active, similar to CAMPSITE | 80–120 mA | |

With a 100 Ah battery: roughly **80–140 days** OFFGRID standby. The previous architecture's ~520 day figure assumed the host MCU could deep-sleep, which is incompatible with running a Thread border router. These numbers will be measured and refined during firmware bring-up.

---

## Repository Layout

```
.
├── README.md                       # This document
│
├── c6-hub/                         # ESP-IDF v6.0 firmware (ESP32-C6)
│   ├── CMakeLists.txt
│   ├── partitions.csv              # Single-app slot for 4 MB; switches to dual-OTA on 16 MB
│   ├── sdkconfig.defaults
│   ├── build.sh                    # Wraps idf.py
│   ├── flash.sh                    # Wraps idf.py flash + monitor
│   └── main/
│       ├── CMakeLists.txt
│       ├── main.c                  # Entry point — currently boot info + heartbeat
│       ├── state_machine.c         # OFFGRID / CAMPSITE / TOWING / ARRIVED  (planned)
│       ├── thread_otbr.c           # OpenThread Border Router  (planned)
│       ├── matter_controller.c     # Matter device pairing + reads  (planned)
│       ├── wifi_manager.c          # Wi-Fi STA + OTA  (planned)
│       ├── ota_handler.c           # esp_https_ota driver  (planned)
│       ├── victron_vedirect.c      # VE.Direct parser  (planned)
│       ├── power_monitor.c         # INA219 + shore/car detect  (planned)
│       ├── sensors.c               # DS18B20, water level, MPU6500  (planned)
│       ├── ble_bms.c               # Power Queen BMS client  (planned)
│       ├── ble_server.c            # Phone-app GATT server  (planned)
│       ├── sms_handler.c           # SIM7600 AT commands  (planned)
│       ├── gps.c                   # GPS-via-AT polling  (planned)
│       ├── display_link.c          # T-Encoder UART protocol + buzzer  (planned)
│       ├── alerts.c                # Threshold monitoring  (planned)
│       ├── rules_engine.c          # Configurable automation  (planned)
│       └── config.c                # NVS storage for settings  (planned)
│
├── t-encoder-display/              # LVGL UI for T-Encoder Pro  (planned)
│   ├── main.cpp
│   ├── gauges.cpp
│   ├── level_screen.cpp
│   ├── menu.cpp
│   └── uart_protocol.cpp
│
├── docs/                           # Architecture & protocol specs
│   ├── architecture.mermaid
│   ├── state_machine.mermaid
│   ├── wiring.mermaid
│   ├── dataflow.mermaid
│   ├── protocols/
│   │   ├── powerqueen_bms.md
│   │   └── victron_instant_readout.md
│   └── superpowers/                # Specs and plans for major changes
│       ├── specs/
│       └── plans/
│
└── tools/
    └── ble-probe/                  # Python reference (Pi-based) — reverse-engineers
        ├── read_battery.py         # the BLE protocols the firmware re-implements in C.
        ├── read_victron.py         # Not part of the runtime system.
        └── victron.env.example
```

Module source files inside `c6-hub/main/` (other than `main.c`) and
`t-encoder-display/` are **targets to be implemented** — `main.c` is a
hello-world that prints chip info; the rest will be built up module by
module, each with its own spec under `docs/superpowers/specs/`.

---

## Key Design Decisions

1. **Single ESP32-C6 SuperMini** — One chip handles Thread (native 802.15.4), Wi-Fi 6, BLE 5.3, and the application logic. Replaces the earlier ESP32-S3 + XIAO nRF52840 RCP design; see [`docs/superpowers/specs/2026-04-25-c6-single-mcu-design.md`](docs/superpowers/specs/2026-04-25-c6-single-mcu-design.md) for the rationale.

2. **IKEA Thread sensors over custom nodes** — Off-the-shelf Matter-over-Thread devices eliminate custom firmware, provide multi-year battery life, and simplify deployment.

3. **SIM7600 over UART (AT mode + GPS-via-AT)** — The C6 has no USB host. AT-mode polling at ~1 Hz is sufficient for the caravan use case (SMS commands, periodic GPS); avoids a second UART for raw NMEA streaming.

4. **T-Encoder Pro for display and audio** — The display module includes its own ESP32-S3, AMOLED screen, rotary encoder, and a buzzer on its GPIO17. The C6 sends JSON over UART; the display drives its own UI and sounds its own buzzer. No discrete buzzer needed on the C6.

5. **Single Victron VE.Direct** — MPPT only; IP65 charger data is redundant given shore power detection and INA219 monitoring.

6. **DS18B20 for outdoor temp** — Wired sensor is more reliable than battery-powered Thread sensor for external mounting.

7. **Power Queen BMS via BLE** — Deferred to software phase due to proprietary protocol; INA219 provides backup current measurement.

8. **Relay support pre-wired** — GPIO 14 reserved for future relay control without implementing now.

9. **Automation rules in software** — All thresholds and behaviors configurable in NVS, changeable via phone app or Wi-Fi.

10. **Wi-Fi kept active** — Used for OTA updates and home-network connectivity when parked. Drops to backhaul-only or disabled if SRAM headroom becomes a problem.

---

## Next Steps

1. ~~Flash XIAO nRF52840 with OpenThread RCP firmware~~ — **obsolete after C6 pivot.**
2. **Bring up `c6-hub` hello-world firmware on the 4 MB ESP32-C6FH4** — confirm USB-Serial-JTAG flash + console + IEEE 802.15.4 MAC visible. ✓ Done 2026-04-25.
3. **Build VE.Direct cable** with TXS0102 level shifter.
4. **Wire detection circuits** (shore power, car connection).
5. **Develop ESP32-C6 firmware modules** — each with its own spec/plan under `docs/superpowers/`. Suggested order: state_machine → power_monitor → sensors → display_link → ble_bms → thread_otbr → matter_controller → wifi_manager → sms_handler → ota_handler → alerts/rules.
6. **Pair IKEA sensors** to the Thread network (after Matter controller works).
7. **Develop T-Encoder Pro UI** (LVGL gauges, menus, buzzer patterns).
8. **Port Power Queen BLE protocol** to C (spec in [`docs/protocols/powerqueen_bms.md`](docs/protocols/powerqueen_bms.md)).
9. **Migrate to 16 MB SuperMini** when it arrives — switch `partitions.csv` to dual-OTA layout.
10. **Test and iterate.**

---

## References

- [ESP-IDF v6.0 Programming Guide (ESP32-C6)](https://docs.espressif.com/projects/esp-idf/en/stable/esp32c6/)
- [ESP Thread Border Router SDK](https://github.com/espressif/esp-thread-br)
- [ESP-Matter](https://github.com/espressif/esp-matter)
- [Victron VE.Direct Protocol](https://www.victronenergy.com/support-and-downloads/technical-information)
- [IKEA DIRIGERA Thread Sensors](https://www.ikea.com/us/en/cat/smart-sensors-47547/)
- [T-Encoder Pro GitHub](https://github.com/Xinyuan-LilyGO/T-Encoder-Pro)
- [Power Queen BMS BLE Protocol](https://github.com/dmytro-tsepilov/pq_bms_bluetooth)

---

*Document version: 2.0 — single-MCU ESP32-C6 architecture*  
*Last updated: April 2026*
