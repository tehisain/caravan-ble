#!/usr/bin/env python3
"""Read PowerQueen LiFePO4 BMS over BLE and emit normalized JSON.

Protocol reverse-engineered by dmytro-tsepilov/pq_bms_bluetooth. This script is a
thin wrapper that speaks the same protocol directly (no dependency on that repo),
normalizes all values to SI units (V, A, W, Ah, C), and prints a single JSON
object on stdout — suitable for home automation ingestion.

Exit codes:
  0 = ok
  2 = connection / BLE error
  3 = checksum mismatch
  4 = timeout
"""

import argparse
import asyncio
import json
import sys
from typing import Any

from bleak import BleakClient, BleakError

BMS_CHAR = "0000ffe1-0000-1000-8000-00805f9b34fb"

CMD_VERSION = bytes.fromhex("0000040116 55AA1A".replace(" ", ""))
CMD_BATTERY = bytes.fromhex("0000040113 55AA17".replace(" ", ""))

BATTERY_STATE = {0: "idle", 1: "charging", 2: "discharging", 4: "full"}


def crc8_sum(data: bytes) -> int:
    return sum(data) & 0xFF


def u16_be_rev(b: bytes) -> int:
    return int.from_bytes(b[::-1], "big", signed=False)


def i16_be_rev(b: bytes) -> int:
    return int.from_bytes(b[::-1], "big", signed=True)


def u32_be_rev(b: bytes) -> int:
    return int.from_bytes(b[::-1], "big", signed=False)


def i32_be_rev(b: bytes) -> int:
    return int.from_bytes(b[::-1], "big", signed=True)


def parse_version(data: bytes) -> dict[str, Any]:
    if crc8_sum(data[:-1]) != data[-1]:
        raise ValueError("version checksum mismatch")
    start = data[8:]
    fw = f"{u16_be_rev(start[0:2])}.{u16_be_rev(start[2:4])}.{u16_be_rev(start[4:6])}"
    mfg = f"{u16_be_rev(start[6:8])}-{start[8]:02d}-{start[9]:02d}"
    hw = "".join(chr(b) for b in start[0::2] if 32 <= b <= 126)
    return {"firmware_version": fw, "manufacture_date": mfg, "hardware_version": hw}


def parse_battery(data: bytes) -> dict[str, Any]:
    if crc8_sum(data[:-1]) != data[-1]:
        raise ValueError("battery checksum mismatch")

    pack_mv = u32_be_rev(data[8:12])
    volt_mv = u32_be_rev(data[12:16])
    current_ma = i32_be_rev(data[48:52])
    cell_temp = i16_be_rev(data[52:54])
    mos_temp = i16_be_rev(data[54:56])
    remain_ah_x100 = u16_be_rev(data[62:64])
    factory_ah_x100 = u16_be_rev(data[64:66])

    cells = {}
    cell_block = data[16:48]
    for i in range(0, len(cell_block), 2):
        cv = int.from_bytes([cell_block[i + 1], cell_block[i]], "big")
        if cv:
            cells[i // 2 + 1] = cv / 1000.0

    heat_hex = data[68:72][::-1].hex()
    protect_hex = data[76:80][::-1].hex()
    failure_bytes = list(data[80:84][::-1])
    equilibrium = u32_be_rev(data[84:88])
    state_code = u16_be_rev(data[88:90])
    soc = u16_be_rev(data[90:92])
    soh = u32_be_rev(data[92:96])
    discharges = u32_be_rev(data[96:100])
    discharges_ah = u32_be_rev(data[100:104])

    voltage_v = volt_mv / 1000.0
    current_a = current_ma / 1000.0
    power_w = round(voltage_v * current_a, 2)

    return {
        "pack_voltage_v": round(pack_mv / 1000.0, 3),  # charger-side voltage
        "voltage_v": round(voltage_v, 3),              # battery terminal voltage
        "current_a": round(current_a, 3),              # + charging / - discharging
        "power_w": power_w,
        "soc_percent": soc,
        "soh_percent": soh,
        "remain_ah": round(remain_ah_x100 / 100.0, 2),
        "capacity_ah": round(factory_ah_x100 / 100.0, 2),
        "cell_temperature_c": cell_temp,
        "mosfet_temperature_c": mos_temp,
        "cells_v": cells,
        "cell_count": len(cells),
        "cell_min_v": round(min(cells.values()), 3) if cells else None,
        "cell_max_v": round(max(cells.values()), 3) if cells else None,
        "cell_delta_mv": round((max(cells.values()) - min(cells.values())) * 1000, 1) if cells else None,
        "balancing": equilibrium > 0,
        "equilibrium_mask": equilibrium,
        "state_code": state_code,
        "state": BATTERY_STATE.get(state_code, f"unknown({state_code})"),
        "discharge_switch_on": int(heat_hex[6]) < 8,  # per upstream library
        "self_heating_on": heat_hex[7] == "2",
        "protect_state_hex": protect_hex,
        "failure_state": failure_bytes,
        "heat_flags_hex": heat_hex,
        "total_discharge_cycles": discharges,
        "total_discharged_ah": discharges_ah,
    }


async def read_bms(mac: str, timeout: float) -> dict[str, Any]:
    result: dict[str, Any] = {}
    responses: list[bytes] = []

    def cb(_s, data: bytearray) -> None:
        responses.append(bytes(data))

    async with BleakClient(mac, timeout=timeout) as client:
        await client.start_notify(BMS_CHAR, cb)

        responses.clear()
        await client.write_gatt_char(BMS_CHAR, CMD_VERSION, response=True)
        await asyncio.sleep(1.0)
        if not responses:
            raise TimeoutError("no response to GET_VERSION")
        result.update(parse_version(responses[-1]))

        responses.clear()
        await client.write_gatt_char(BMS_CHAR, CMD_BATTERY, response=True)
        await asyncio.sleep(1.0)
        if not responses:
            raise TimeoutError("no response to GET_BATTERY_INFO")
        result.update(parse_battery(responses[-1]))

        await client.stop_notify(BMS_CHAR)

    return result


def main() -> int:
    ap = argparse.ArgumentParser(description="Read PowerQueen LiFePO4 BMS via BLE")
    ap.add_argument("mac", help="Battery BLE MAC address")
    ap.add_argument("-t", "--timeout", type=float, default=10.0)
    args = ap.parse_args()

    try:
        data = asyncio.run(read_bms(args.mac, args.timeout))
    except asyncio.TimeoutError:
        print(json.dumps({"error": "timeout"}))
        return 4
    except ValueError as e:
        print(json.dumps({"error": "checksum", "detail": str(e)}))
        return 3
    except BleakError as e:
        print(json.dumps({"error": "ble", "detail": str(e)}))
        return 2
    except Exception as e:
        print(json.dumps({"error": type(e).__name__, "detail": str(e)}))
        return 2

    print(json.dumps(data, indent=2, sort_keys=False))
    return 0


if __name__ == "__main__":
    sys.exit(main())
