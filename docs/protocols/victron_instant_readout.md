# Victron BLE Instant Readout

Victron devices broadcast encrypted telemetry as part of their BLE advertisement
manufacturer data (ID `0x02E1`). Decryption is done locally with a per-device
**32-hex-char AES-CTR key**. No pairing or GATT connection is needed.

## Devices found on the caravan Pi

| Device | MAC | Model | Parser |
|---|---|---|---|
| `SmartSolar HQ2216XZQ42` | `DA:02:0F:CA:6D:1B` | SmartSolar Charger MPPT 75/15 | `SolarChargerData` |
| `BSC IP65 12/25 HQ2427EXUK7` | `E9:3C:6F:D4:A6:08` | Blue Smart IP65 Charger 12\|25 | `AcChargerData` |

Per-device Instant Readout keys live on the Pi at `~/victron/victron.env`
(mode `0600`). They don't rotate unless you factory-reset the device.

## Getting the encryption key (one-time, per device)

1. Open **VictronConnect** on your phone, in range of the device.
2. Tap the device to connect.
3. Gear icon (top right) → **Product info**.
4. Scroll to **Instant Readout via Bluetooth** — enable it if it isn't already.
5. Tap **Show** next to *Instant Readout Details* → copy the 32-hex key.
6. Repeat for each device. Store them somewhere safe; they don't change unless
   you factory-reset the device.

## Usage

Keys already populated on the Pi — just source the env file:

```
set -a; . ~/victron/victron.env; set +a
~/victron/venv/bin/python ~/victron/read_victron.py
```

Or pass `MAC=KEY` pairs directly:

```
~/victron/venv/bin/python ~/victron/read_victron.py \
  DA:02:0F:CA:6D:1B=<solar-key> \
  E9:3C:6F:D4:A6:08=<charger-key>
```

Prints a JSON array, one record per device.

### Live sample

```json
[
  {"model_name": "SmartSolar Charger MPPT 75/15", "battery_voltage": 13.55,
   "battery_charging_current": 0.0, "solar_power": 0, "yield_today": 0,
   "charge_state": "FLOAT", "charger_error": "NO_ERROR", "external_device_load": 0.0},
  {"model_name": "Blue Smart IP65 Charger 12|25", "output_voltage1": 13.5,
   "output_current1": 0.0, "charge_state": "STORAGE", "charger_error": "NO_ERROR"}
]
```

## Fields (from the `victron-ble` library)

### SolarChargerData (MPPT)
`battery_voltage`, `battery_charging_current`, `solar_power`, `yield_today`,
`charge_state` (off/bulk/absorption/float/equalize/etc.), `charger_error`,
`external_device_load`, `model_name`.

### AcChargerData (Blue Smart IP65 charger)
`ac_current`, up to three `output_voltage1..3` / `output_current1..3` channels,
`charge_state`, `charger_error`, `temperature`, `model_name`.

### Other device types supported by the same library
`BatteryMonitorData` (SmartShunt/BMV), `BatterySenseData`,
`DcDcConverterData` (Orion Smart), `DcEnergyMeterData`,
`InverterData` (Phoenix Smart), `LynxSmartBMSData`, `MultiRSData`,
`OrionXSData`, `SmartBatteryProtectData`, `SmartLithiumData`, `VEBusData`.

## Protocol notes

- Scanner library: [`victron-ble`](https://github.com/keshavdv/victron-ble)
  (PyPI: `victron-ble`).
- Frames are the "Extra Manufacturer Data Record" format from
  [Victron's published spec](https://communityarchive.victronenergy.com/questions/93919/victron-bluetooth-ble-protocol-publication.html).
- Payload is AES-CTR encrypted; only the holder of the key can decode it.
- Advertisements come in every ~1 s from active devices.

## Integration

Same story as the PowerQueen reader — run on a schedule, pipe JSON into MQTT /
Home Assistant. HA also has a first-class
[victron-ble HACS integration](https://github.com/keshavdv/victron-hass) that
just needs the same keys.
