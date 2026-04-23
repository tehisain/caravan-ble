# Caravan BLE readers

Readers that pull telemetry from the Bluetooth devices in the caravan and
print normalized JSON on stdout — intended for the caravan Pi
(`192.168.89.150`, hci0) and a home-automation scraper.

- `read_battery.py` — PowerQueen LiFePO4 BMS, see [PROTOCOL.md](./PROTOCOL.md)
- `read_victron.py` — Victron Instant Readout (SmartSolar MPPT, Blue Smart
  Charger, …), see [VICTRON.md](./VICTRON.md)

## PowerQueen battery

- Device: `PQ-12100BE-A00278` — MAC `C8:47:80:10:C2:DE`
- Protocol: see [PROTOCOL.md](./PROTOCOL.md)
- Reference implementation: [pq_bms_bluetooth](https://github.com/dmytro-tsepilov/pq_bms_bluetooth)

## Run on the Pi

```
python3 -m venv ~/powerqueen/venv
~/powerqueen/venv/bin/pip install 'bleak==0.22.*'
~/powerqueen/venv/bin/python read_battery.py C8:47:80:10:C2:DE
```

Exit codes: `0` ok, `2` BLE error, `3` checksum, `4` timeout.

## Output fields

All values are SI. `cells_v` is a 1-indexed map. `current_a` is signed
(+ charging, − discharging). `state` is one of `idle|charging|discharging|full`.
`balancing` is a boolean; `equilibrium_mask` exposes the raw bitmask.

## Integration ideas

- **Poll + MQTT**: cron every 30–60 s, publish the JSON to
  `caravan/battery/state`; Home Assistant picks it up via an MQTT sensor.
- **Prometheus**: wrap the JSON in a tiny text-exposition script behind
  `node_exporter --collector.textfile.directory`.
- **Home Assistant direct**: use the
  [BLE BMS integration](https://github.com/patman15/BMS_BLE-HA) — it ships a
  PowerQueen driver. Running it means the Pi's hci0 must be exposed to HA
  (passthrough, proxy, or run HA on the same Pi).
