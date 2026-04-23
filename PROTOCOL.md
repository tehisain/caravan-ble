# PowerQueen LiFePO4 BMS — BLE Protocol

Reverse-engineered by [dmytro-tsepilov/pq_bms_bluetooth](https://github.com/dmytro-tsepilov/pq_bms_bluetooth).
Verified against device `PQ-12100BE-A00278` (12 V 100 Ah, firmware 1.3.0).

## Link layer

- BLE GATT, BlueZ pairing **not** required.
- Advertised name format: `PQ-<model>-<serial>` (e.g. `PQ-12100BE-A00278`).
- Underlying chipset: BEKEN SAS `BK-BLE-1.0` (module FW 6.1.2 / SW 6.3.0).

## Characteristic

| Purpose | UUID | Properties |
|---|---|---|
| BMS request/response | `0000ffe1-0000-1000-8000-00805f9b34fb` | write + notify |
| Serial number (unused) | `0000ffe2-0000-1000-8000-00805f9b34fb` | unimplemented in FW |

All requests are written to `ffe1`; responses arrive as notifications on the same characteristic.

## Commands

Every frame ends with a 1-byte CRC = `sum(preceding bytes) & 0xFF`.

| Name | Bytes (hex) |
|---|---|
| `GET_VERSION` | `00 00 04 01 16 55 AA 1A` |
| `GET_BATTERY_INFO` | `00 00 04 01 13 55 AA 17` |
| `SERIAL_NUMBER` (no-op) | `00 00 04 01 10 55 AA 14` |

Header anatomy: `00 00 04 01 <opcode> 55 AA <crc>` — the `55 AA` is a magic footer before the CRC.

## Response framing

All responses share `00 00 <len> 01 <opcode|0x80> 55 AA` as the first 7 bytes, followed by payload, with the final byte being CRC.
Multi-byte integers are stored **little-endian** (the upstream lib reverses each slice then decodes big-endian — same result).

### `GET_BATTERY_INFO` payload offsets

| Offset | Size | Field | Units / Notes |
|---:|---:|---|---|
| 8  | 4  | pack voltage         | mV, u32 (charger-side / pack) |
| 12 | 4  | terminal voltage     | mV, u32 |
| 16 | 32 | cell voltages        | up to 16 × u16 LE, mV; zero = cell absent |
| 48 | 4  | current              | mA, **signed** i32 (+ charging, − discharging) |
| 52 | 2  | cell temperature     | °C, i16 |
| 54 | 2  | MOSFET temperature   | °C, i16 |
| 62 | 2  | remaining capacity   | u16 × 0.01 Ah |
| 64 | 2  | factory capacity     | u16 × 0.01 Ah |
| 68 | 4  | heat/flags (hex)     | nibble 6 → discharge switch; nibble 7 → self-heating (`2`=on) |
| 76 | 4  | protect state (hex)  | bitmap |
| 80 | 4  | failure state        | 4 × u8 |
| 84 | 4  | equilibrium / balance | u32 bitmask — non-zero means balancing |
| 88 | 2  | battery state        | 0=idle, 1=charging, 2=discharging, 4=full |
| 90 | 2  | SOC                  | % |
| 92 | 4  | SOH                  | % |
| 96 | 4  | discharge cycle count | u32 |
| 100| 4  | cumulative discharged | u32 Ah |

### `GET_VERSION` payload offsets

| Offset (from byte 8) | Size | Field |
|---:|---:|---|
| 0 | 2 | firmware major |
| 2 | 2 | firmware minor |
| 4 | 2 | firmware patch |
| 6 | 2 | manufacture year |
| 8 | 1 | manufacture month |
| 9 | 1 | manufacture day |
| 10+ | — | ASCII hardware string (every second byte is printable, e.g. `T12100-V1.3`) |

## Watts

Power is not reported — compute as `voltage_v * current_a`.

## Observed live sample

```
PQ-12100BE-A00278 → 13.578 V, 0.0 A, SOC 100 %, SOH 105 %,
4 cells 3.377/3.395/3.399/3.407 V (Δ 30 mV, balancing bit set),
cellT 11 °C, mosT 9 °C, remain 91.9/cap 91.9 Ah,
state=full, 7 cycles, 716 Ah lifetime discharged, FW 1.3.0, HW 2023-12-20.
```
