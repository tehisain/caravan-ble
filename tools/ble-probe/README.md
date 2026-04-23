# BLE probe (Python reference)

Python scripts that reverse-engineer the caravan's BLE devices from a Raspberry
Pi running in range. **Reference only** — not part of the runtime system. The
ESP32-S3 hub (see [`../../esp32-hub/`](../../esp32-hub/)) will re-implement
these protocols in C against the specs in
[`../../docs/protocols/`](../../docs/protocols/).

Each script emits normalized JSON on stdout so the same output format can be
reproduced on the ESP32 side and compared byte-for-byte during porting.

- `read_battery.py` — PowerQueen LiFePO4 BMS
  → [protocol spec](../../docs/protocols/powerqueen_bms.md)
- `read_victron.py` — Victron Instant Readout (SmartSolar MPPT, Blue Smart
  Charger, …) → [protocol spec](../../docs/protocols/victron_instant_readout.md)

## PowerQueen battery

- Device: `PQ-12100BE-A00278` — MAC `C8:47:80:10:C2:DE`
- Upstream reference: [pq_bms_bluetooth](https://github.com/dmytro-tsepilov/pq_bms_bluetooth)

## Run on the Pi

```
python3 -m venv ~/powerqueen/venv
~/powerqueen/venv/bin/pip install 'bleak==0.22.*' 'victron-ble'
~/powerqueen/venv/bin/python read_battery.py C8:47:80:10:C2:DE
```

Exit codes: `0` ok, `2` BLE error, `3` checksum, `4` timeout.

## Victron keys

Per-device Instant Readout AES keys go in `victron.env` (see
`victron.env.example`, mode `0600`). They don't rotate unless you factory-reset
the device.

## Output fields

All values are SI. `cells_v` is a 1-indexed map. `current_a` is signed
(+ charging, − discharging). `state` is one of `idle|charging|discharging|full`.
`balancing` is a boolean; `equilibrium_mask` exposes the raw bitmask.
