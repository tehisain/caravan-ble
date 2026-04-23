#!/usr/bin/env python3
"""Read Victron Instant Readout BLE advertisements and emit normalized JSON.

Victron broadcasts encrypted telemetry in manufacturer data 0x02E1. Each device
has a 32-hex-char AES key displayed in VictronConnect:
    Settings -> Product info -> Instant Readout via Bluetooth -> Show.

Decryption + schema are provided by the `victron-ble` library.

Pass one or more device IDs (MAC, colons optional) with keys, either as CLI:
    read_victron.py DA:02:0F:CA:6D:1B=<key> E9:3C:6F:D4:A6:08=<key>
or via $VICTRON_DEVICES="MAC=KEY,MAC=KEY".

Prints a JSON array of the next advertisement from each device, then exits.
"""

import argparse
import asyncio
import json
import os
import sys
import time
from dataclasses import is_dataclass, asdict
from enum import Enum

from bleak import BleakScanner
from victron_ble.devices import detect_device_type


def _clean_mac(s: str) -> str:
    return s.replace(":", "").replace("-", "").lower()


def _parse_pairs(items: list[str]) -> dict[str, str]:
    out: dict[str, str] = {}
    for it in items:
        if "=" not in it:
            raise SystemExit(f"expected MAC=KEY, got {it!r}")
        mac, key = it.split("=", 1)
        out[_clean_mac(mac)] = key.strip()
    return out


def _to_plain(obj):
    if isinstance(obj, Enum):
        return obj.name
    if is_dataclass(obj):
        return {k: _to_plain(v) for k, v in asdict(obj).items()}
    if hasattr(obj, "_asdict"):
        return {k: _to_plain(v) for k, v in obj._asdict().items()}
    if isinstance(obj, dict):
        return {k: _to_plain(v) for k, v in obj.items()}
    if isinstance(obj, (list, tuple)):
        return [_to_plain(v) for v in obj]
    return obj


def _device_to_dict(parser, raw: bytes) -> dict:
    data = parser.parse(raw)
    # victron-ble returns a *Data subclass with typed getters; iterate them.
    out: dict = {}
    for attr in dir(data):
        if attr.startswith("_") or attr in {"parse"}:
            continue
        if not attr.startswith("get_"):
            continue
        try:
            val = getattr(data, attr)()
        except Exception:
            continue
        key = attr[4:]
        out[key] = _to_plain(val)
    out["model_name"] = data.get_model_name()
    return out


async def read_once(devices: dict[str, str], timeout: float) -> list[dict]:
    remaining = dict(devices)
    results: list[dict] = []
    done = asyncio.Event()

    def cb(device, adv):
        key_hex = remaining.get(_clean_mac(device.address))
        if not key_hex:
            return
        md = adv.manufacturer_data.get(0x02E1)
        if not md:
            return
        try:
            parser = detect_device_type(md)
            if parser is None:
                return
            parser = parser(key_hex)
            record = _device_to_dict(parser, md)
        except Exception as e:
            record = {"error": f"{type(e).__name__}: {e}"}
        record["address"] = device.address
        record["name"] = device.name
        record["rssi"] = adv.rssi
        record["ts"] = time.time()
        results.append(record)
        remaining.pop(_clean_mac(device.address), None)
        if not remaining:
            done.set()

    scanner = BleakScanner(detection_callback=cb)
    await scanner.start()
    try:
        await asyncio.wait_for(done.wait(), timeout=timeout)
    except asyncio.TimeoutError:
        pass
    finally:
        await scanner.stop()

    for mac, _ in remaining.items():
        results.append({"address": mac, "error": "no advertisement within timeout"})
    return results


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument(
        "devices",
        nargs="*",
        help="MAC=KEY pairs (key is 32-hex from VictronConnect). "
        "Env $VICTRON_DEVICES also accepted as comma-separated list.",
    )
    ap.add_argument("-t", "--timeout", type=float, default=15.0)
    args = ap.parse_args()

    pairs = list(args.devices)
    env = os.environ.get("VICTRON_DEVICES", "").strip()
    if env:
        pairs.extend(p for p in env.split(",") if p.strip())
    if not pairs:
        print("error: no devices given (MAC=KEY)", file=sys.stderr)
        return 2

    devices = _parse_pairs(pairs)
    out = asyncio.run(read_once(devices, args.timeout))
    print(json.dumps(out, indent=2, sort_keys=False))
    return 0 if all("error" not in d for d in out) else 2


if __name__ == "__main__":
    sys.exit(main())
