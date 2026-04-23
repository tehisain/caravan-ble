# Caravan Automation System

Low-power monitoring and automation system for a towable caravan, built around an ESP32-S3 running OpenThread Border Router with Matter-compatible sensors.

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
│                        ESP32-S3 HUB                             │
│                                                                 │
│  ┌─────────┐ ┌─────────┐ ┌─────────┐ ┌─────────┐ ┌─────────┐  │
│  │ Thread  │ │  BLE    │ │ VE.Dir  │ │  State  │ │  Rules  │  │
│  │  OTBR   │ │ Dual    │ │ Parser  │ │ Machine │ │ Engine  │  │
│  └────┬────┘ └────┬────┘ └────┬────┘ └────┬────┘ └─────────┘  │
│       │           │           │           │                    │
└───────┼───────────┼───────────┼───────────┼────────────────────┘
        │           │           │           │
        ▼           ▼           ▼           ▼
   ┌─────────┐ ┌─────────┐ ┌─────────┐ ┌─────────┐
   │  XIAO   │ │ Power   │ │ Victron │ │  GPIO   │
   │nRF52840 │ │ Queen   │ │  MPPT   │ │ Detect  │
   │ Thread  │ │  BMS    │ │VE.Direct│ │Shore/Car│
   └────┬────┘ └─────────┘ └─────────┘ └─────────┘
        │
        ▼
   ┌─────────────────────────────────────┐
   │      IKEA Thread Sensors            │
   │  Door │ Temp │ Leak │ Motion        │
   └─────────────────────────────────────┘
```

---

## Hardware Components

### Already Owned

| Component | Purpose | Interface |
|-----------|---------|-----------|
| ESP32-S3 DevKit | Main controller | — |
| XIAO nRF52840 Plus | Thread Radio Co-Processor | UART |
| T-Encoder Pro | AMOLED display + rotary encoder | UART (JST) |
| SIM7600SA-MNSE | 4G LTE + GPS | USB |
| MPU6500 | Gyroscope/accelerometer + buzzer | I2C |
| Victron MPPT 75/15 | Solar charge controller | VE.Direct |
| Victron IP65 12V 25A | Shore power charger | — |
| Power Queen 100Ah | LiFePO4 battery with BLE BMS | BLE |
| Dometic SMP 301-01 | 230V AC → 12V DC | — |
| 13-pin Euro connector | Tow vehicle interface | — |
| Water level gauge | 0-190Ω resistive | ADC |
| SimBase data SIM | 4G connectivity | — |

### To Purchase (~€72)

| Component | Purpose | Qty | Est. Cost |
|-----------|---------|-----|-----------|
| INA219 | Current/power monitor | 2 | €8 |
| PC817 | Optocoupler (isolation) | 3 | €2 |
| HLK-PM01 | 230V AC → 5V DC (detection) | 1 | €5 |
| AO3401 | P-channel MOSFET (power gate) | 1 | €1 |
| TXS0102 | Bidirectional level shifter | 1 | €2 |
| DS18B20 | Waterproof temp sensor | 1 | €3 |
| Resistors | 4.7kΩ, 100Ω, 1kΩ, 10kΩ | — | €5 |
| JST-PH connectors | VE.Direct cable, wiring | — | €3 |
| Prototype PCB | Assembly | 1 | €5 |
| IKEA TIMMERFLOTTE | Indoor temp/humidity | 1 | €6 |
| IKEA MYGGBETT | Door/window sensor | 2 | €16 |
| IKEA KLIPPBOK | Water leak sensor | 1 | €8 |
| IKEA MYGGSPRAY | Motion sensor | 1 | €8 |

---

## GPIO Assignments

| GPIO | Function | Direction | Protocol | Notes |
|------|----------|-----------|----------|-------|
| 1 | XIAO nRF52840 TX | Output | UART1 | Thread RCP, 460800 baud |
| 2 | XIAO nRF52840 RX | Input | UART1 | |
| 3 | I2C SDA | Bidir | I2C | INA219 ×2, MPU6500 |
| 4 | I2C SCL | Output | I2C | 400 kHz |
| 5 | Victron MPPT TX | Output | UART2 | Via TXS0102 level shifter |
| 6 | Victron MPPT RX | Input | UART2 | 19200 baud |
| 7 | T-Encoder Pro TX | Output | SW UART | JST connector |
| 8 | T-Encoder Pro RX | Input | SW UART | 115200 baud |
| 17 | Shore power detect | Input | GPIO | Active LOW (PC817) |
| 18 | Car connection detect | Input | GPIO | Active LOW (PC817) |
| 19 | SIM7600 power gate | Output | GPIO | AO3401 P-FET control |
| 20 | Buzzer | Output | PWM | Level/alert audio |
| 21 | DS18B20 data | Bidir | 1-Wire | 4.7kΩ pull-up to 3.3V |
| 33 | Water level ADC | Input | ADC | 100Ω voltage divider |
| USB | SIM7600SA | — | USB Host | AT commands, GPS NMEA |
| BLE | Power Queen + Phone | — | BLE | Central + Peripheral |

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

3.3V ──► 10kΩ pull-up ──┬──► GPIO17
                        │
                   PC817 collector
                        │
                       GND

Logic: Shore ON → GPIO17 LOW | Shore OFF → GPIO17 HIGH
```

### Car Connection Detection (13-Pin Connector)

```
Pin 10 (12V) ──► 10kΩ ──┬──► 10kΩ ──► GND     (voltage divider)
                        │
                        └──► 1kΩ ──► PC817 LED
                                          │
                                         GND

3.3V ──► 10kΩ pull-up ──┬──► GPIO18
                        │
                   PC817 collector
                        │
                       GND

Logic: Car ON → GPIO18 LOW | Car OFF → GPIO18 HIGH
```

### Water Level Sensing

```
3.3V ──► 100Ω ──┬──► GPIO33 (ADC)
                │
           Tank Sensor (0-190Ω)
                │
               GND

Voltage: Empty (0Ω) = 0.00V | Full (190Ω) = 2.16V
ADC: Empty ≈ 0 | Full ≈ 2680 (of 4095)
```

### Outdoor Temperature (DS18B20)

```
3.3V ──► 4.7kΩ ──┬──► GPIO21 (1-Wire data)
                 │
            DS18B20 DATA (yellow)
            DS18B20 VCC (red) ──► 3.3V
            DS18B20 GND (black) ──► GND
```

---

## VE.Direct Connection

Victron MPPT uses 5V TTL; ESP32-S3 uses 3.3V. Level shifter required.

```
Victron JST-PH          TXS0102              ESP32-S3
─────────────          ─────────             ────────
Pin 1: GND ──────────► GND ◄────────────────► GND
Pin 2: RX  ◄───────── HV1 ◄──── LV1 ◄─────── GPIO5 (TX)
Pin 3: TX  ──────────► HV2 ────► LV2 ───────► GPIO6 (RX)
Pin 4: +5V ──────────► HV (ref)
                       LV (ref) ◄───────────► 3.3V
```

---

## State Machine

| GPIO17 (Shore) | GPIO18 (Car) | State | Description |
|----------------|--------------|-------|-------------|
| HIGH | HIGH | **OFFGRID** | Battery only, wild camping |
| LOW | HIGH | **CAMPSITE** | Shore power connected |
| HIGH | LOW | **TOWING** | Car connected, driving |
| LOW | LOW | **ARRIVED** | Both connected, just parked |

### State Behaviors

| Feature | OFFGRID | CAMPSITE | TOWING | ARRIVED |
|---------|---------|----------|--------|---------|
| Deep sleep | Aggressive | Off | Off | Off |
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
| ESP32 ↔ XIAO | UART1 | 460800 | Thread RCP (HDLC frames) |
| ESP32 ↔ Victron | UART2 | 19200 | VE.Direct text protocol |
| ESP32 ↔ T-Encoder | SW UART | 115200 | JSON status/events |
| ESP32 ↔ SIM7600 | USB | — | AT commands, GPS NMEA |
| ESP32 ↔ Power Queen | BLE | — | Proprietary BMS protocol |
| ESP32 ↔ Phone | BLE GATT | — | Status read, config write |
| XIAO ↔ IKEA sensors | Thread 802.15.4 | — | Matter over Thread |

---

## T-Encoder Pro Protocol

### ESP32 → T-Encoder (status updates)

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

### T-Encoder → ESP32 (user input)

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

| Component | Active | Sleep | Duty Cycle | Average |
|-----------|--------|-------|------------|---------|
| ESP32-S3 | 80 mA | 10 μA | 1% | ~1 mA |
| XIAO nRF52840 | 5 mA | 2 μA | 5% | ~0.3 mA |
| INA219 ×2 | 1 mA | — | 100% | 1 mA |
| MPU6500 | 3 mA | — | 10% | ~0.3 mA |
| SIM7600 | 300 mA | OFF | 0.1% | ~0.3 mA |
| T-Encoder | 50 mA | — | 10% | ~5 mA |
| **Total (OFFGRID)** | | | | **~8 mA** |
| **Total (CAMPSITE)** | | | | **~60 mA** |

With 100Ah battery: **~520 days** standby in OFFGRID mode (theoretical).

---

## Repository Layout

```
.
├── README.md                       # This document
│
├── esp32-hub/                      # Main ESP-IDF firmware (ESP32-S3)
│   └── main/
│       ├── main.c                  # Entry point, sleep management
│       ├── state_machine.c         # OFFGRID / CAMPSITE / TOWING / ARRIVED
│       ├── thread_otbr.c           # OpenThread Border Router
│       ├── matter_controller.c     # Matter device handling
│       ├── victron_vedirect.c      # VE.Direct parser
│       ├── power_monitor.c         # INA219 + shore/car detect
│       ├── sensors.c               # DS18B20, water level, MPU6500
│       ├── ble_bms.c               # Power Queen BMS client
│       ├── ble_server.c            # Phone-app GATT server
│       ├── sms_handler.c           # SIM7600 AT commands
│       ├── gps.c                   # NMEA parser
│       ├── display_link.c          # T-Encoder UART protocol
│       ├── alerts.c                # Threshold monitoring
│       ├── rules_engine.c          # Configurable automation
│       └── config.c                # NVS storage for settings
│
├── xiao-rcp/                       # Zephyr-based Thread RCP (XIAO nRF52840)
│
├── t-encoder-display/              # LVGL UI for T-Encoder Pro
│   ├── main.cpp                    # Entry / event loop
│   ├── gauges.cpp                  # Battery, solar, temp gauges
│   ├── level_screen.cpp            # Pitch/roll visualization
│   ├── menu.cpp                    # Settings navigation
│   └── uart_protocol.cpp           # JSON parser
│
├── docs/                           # Architecture & protocol specs
│   ├── architecture.mermaid
│   ├── state_machine.mermaid
│   ├── wiring.mermaid
│   ├── dataflow.mermaid
│   └── protocols/
│       ├── powerqueen_bms.md       # PowerQueen BMS BLE protocol
│       └── victron_instant_readout.md
│
└── tools/
    └── ble-probe/                  # Python reference (Pi-based). Reverse-
        ├── read_battery.py         # engineers the BLE protocols the ESP32
        ├── read_victron.py         # firmware will re-implement in C.
        └── victron.env.example     # Not part of the runtime system.
```

Module source files inside `esp32-hub/main/` and `t-encoder-display/` are
**targets to be implemented** — the directories exist but the files haven't
been created yet.

---

## Key Design Decisions

1. **Eliminated Raspberry Pi** — ESP32-S3 handles all functions directly (OTBR, BLE, sensors), reducing complexity and power consumption.

2. **IKEA Thread sensors over custom nodes** — Off-the-shelf Matter/Thread devices eliminate custom firmware, provide multi-year battery life, and simplify deployment.

3. **XIAO nRF52840 via UART** — Native UART RCP is simpler and more reliable than USB; avoids conflict with SIM7600 on USB port.

4. **T-Encoder Pro via UART** — Stationary display doesn't need wireless; UART is reliable for continuous updates.

5. **Single Victron VE.Direct** — MPPT only; IP65 charger data is redundant given shore power detection and INA219 monitoring.

6. **DS18B20 for outdoor temp** — Wired sensor is more reliable than battery-powered Thread sensor for external mounting.

7. **Power Queen BMS via BLE** — Deferred to software phase due to proprietary protocol; INA219 provides backup current measurement.

8. **Relay support pre-wired** — Free GPIOs reserved for future control without implementing now.

9. **Automation rules in software** — All thresholds and behaviors configurable in NVS, changeable via phone app.

---

## Next Steps

1. **Order components** from shopping list (~€72)
2. **Flash XIAO nRF52840** with OpenThread RCP firmware
3. **Build VE.Direct cable** with TXS0102 level shifter
4. **Wire detection circuits** (shore power, car connection)
5. **Develop ESP32-S3 firmware** (modular, start with state machine)
6. **Pair IKEA sensors** to Thread network
7. **Develop T-Encoder Pro UI** (LVGL gauges and menus)
8. **Reverse engineer Power Queen BLE** protocol
9. **Test and iterate**

---

## References

- [ESP-IDF OpenThread Border Router](https://github.com/espressif/esp-idf/tree/master/examples/openthread/ot_br)
- [Victron VE.Direct Protocol](https://www.victronenergy.com/support-and-downloads/technical-information)
- [Nordic nRF52840 Thread RCP](https://developer.nordicsemi.com/nRF_Connect_SDK/doc/latest/nrf/protocols/thread/overview.html)
- [IKEA DIRIGERA Thread Sensors](https://www.ikea.com/us/en/cat/smart-sensors-47547/)
- [T-Encoder Pro GitHub](https://github.com/Xinyuan-LilyGO/T-Encoder-Pro)
- [Power Queen BMS BLE Protocol](https://github.com/dmytro-tsepilov/pq_bms_bluetooth)

---

*Document version: 1.0*  
*Last updated: April 2026*
