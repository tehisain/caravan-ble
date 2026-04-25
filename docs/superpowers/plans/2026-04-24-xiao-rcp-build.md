# XIAO nRF52840 Plus OpenThread RCP Firmware — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build and flash OpenThread Radio Co-Processor (RCP) firmware onto the Seeed XIAO nRF52840 Plus so the ESP32-S3 hub can drive its 802.15.4 radio over UART.

**Architecture:** Build the stock nRF Connect SDK `samples/openthread/coprocessor` sample (Zephyr-based) targeting the `xiao_ble/nrf52840` board. Convert the resulting Intel HEX to a UF2 file with the Adafruit family ID (`0xADA52840`). User flashes via the XIAO's built-in UF2 bootloader (drag-and-drop onto the `XIAO-SENSE` mass-storage volume after double-tap reset). No J-Link required.

**Tech Stack:** nRF Connect SDK v3.3.0, Zephyr RTOS, OpenThread, `west` build system, `nrfutil` toolchain manager, `uf2conv.py` for UF2 conversion.

---

## Key Decisions Locked In

| Decision | Value | Rationale |
|---|---|---|
| ncs version | **v3.3.0** | Current stable (Apr 2026). Produces modern RCP API version matching ESP-IDF OTBR. |
| Board target | **`xiao_ble/nrf52840`** | Mainline Zephyr board. XIAO Plus shares the 14 through-hole pinout with the base XIAO; we don't use the Plus-only castellated extras. No custom board file needed. |
| Transport | **UART0** | Matches project's ESP32↔XIAO wiring (GPIO1/GPIO2). |
| UART pins | **P1.11 (TX) / P1.12 (RX)** = XIAO **D6 / D7** | Zephyr `xiao_ble` default — no pinctrl overlay needed. |
| Baud rate | **1 000 000 bit/s** (sample default) | Matches Nordic's tested config. The project README currently specifies 460 800 — we will update `README.md` after flashing to match what we shipped. Short caravan wiring handles 1 Mbit easily. |
| Bootloader / flash method | **UF2 drag-and-drop** | XIAO ships with Adafruit nRF52 bootloader. No J-Link hardware needed. |
| UF2 family ID | **`0xADA52840`** | Required by Adafruit bootloader; foreign UF2s are rejected. |

## File Structure

Under `xiao-rcp/`:
- `README.md` — updated with build & flash instructions (this replaces the current placeholder).
- `boards/xiao_ble_nrf52840.overlay` — empty stub (kept for future pin/baud overrides; sample defaults are correct today).
- `prj.conf` — extra Kconfig fragment layered on top of the sample's prj.conf. Currently empty; kept as the place for project-specific tweaks.
- `build.sh` — convenience wrapper that invokes `west build` with pinned board target, overlay, and conf. Reproducible single-command builds.
- `.gitignore` — ignores `build/` output.

Outside `xiao-rcp/`:
- `~/ncs/` (user home, not in repo) — nRF Connect SDK workspace created by `nrfutil`. ~3 GB. Shared across all Nordic projects; lives outside the repo.

The overlay and prj.conf files are created intentionally empty (or nearly so) because the sample's defaults already fit our wiring. Keeping the files in place means future tweaks (baud override, pin remap, extra Kconfig) are a one-line change, not a new file.

---

## Task 1: Install `nrfutil` and the nRF Connect SDK toolchain

**Files:** None (system install).

- [ ] **Step 1: Download the `nrfutil` single-binary CLI for macOS**

Run:
```bash
curl -L -o /tmp/nrfutil https://files.nordicsemi.com/artifactory/swtools/external/nrfutil/executables/x86_64-apple-darwin/nrfutil
chmod +x /tmp/nrfutil
sudo mv /tmp/nrfutil /usr/local/bin/nrfutil
nrfutil --version
```
Expected: version string printed (e.g. `nrfutil 8.x.x`).

- [ ] **Step 2: Install the toolchain-manager plugin**

Run:
```bash
nrfutil install toolchain-manager
nrfutil toolchain-manager --help
```
Expected: help text for toolchain-manager subcommands.

- [ ] **Step 3: Install the ncs v3.3.0 toolchain**

Run:
```bash
nrfutil toolchain-manager install --ncs-version v3.3.0
```
Expected: progress bar, ~1 GB download, completes with "Installed v3.3.0".

- [ ] **Step 4: Verify the toolchain works**

Run:
```bash
nrfutil toolchain-manager list
```
Expected: `v3.3.0` appears in the installed list.

- [ ] **Step 5: Commit nothing**

This task installs system-level tooling, no repo changes. Skip commit.

---

## Task 2: Initialize the ncs workspace

**Files:** Creates `~/ncs/v3.3.0/` (outside repo, not committed).

- [ ] **Step 1: Create workspace directory**

Run:
```bash
mkdir -p ~/ncs && cd ~/ncs
```

- [ ] **Step 2: Launch a toolchain-manager shell and initialize the workspace**

Run:
```bash
nrfutil toolchain-manager launch --ncs-version v3.3.0 --shell
```
Expected: a new shell prompt where `west`, `cmake`, `ninja`, and the ARM toolchain are on PATH.

Inside that shell:
```bash
cd ~/ncs
west init -m https://github.com/nrfconnect/sdk-nrf --mr v3.3.0 v3.3.0
cd v3.3.0
west update
```
Expected: `west update` clones ~15 repos (Zephyr, OpenThread, Mbed TLS, etc.). Takes 5–15 minutes depending on network. Ends with all modules at their pinned revisions.

- [ ] **Step 3: Sanity-check: build an OpenThread sample with the default board**

Still inside the toolchain shell:
```bash
cd ~/ncs/v3.3.0/nrf
west build -b nrf52840dk/nrf52840 -p always samples/openthread/coprocessor
```
Expected: build completes, produces `build/coprocessor/zephyr/zephyr.hex`. This proves the toolchain is sound before we target XIAO.

- [ ] **Step 4: Clean up the sanity build**

Run:
```bash
rm -rf ~/ncs/v3.3.0/nrf/build
```

- [ ] **Step 5: Commit nothing**

Workspace lives outside the repo. No commit.

---

## Task 3: Create the `xiao-rcp` build scaffold

**Files:**
- Create: `xiao-rcp/boards/xiao_ble_nrf52840.overlay`
- Create: `xiao-rcp/prj.conf`
- Create: `xiao-rcp/build.sh`
- Create: `xiao-rcp/.gitignore`
- Modify: `xiao-rcp/README.md` (replace placeholder content)

- [ ] **Step 1: Create the empty devicetree overlay**

Create `xiao-rcp/boards/xiao_ble_nrf52840.overlay` with content:
```dts
/*
 * XIAO nRF52840 Plus overlay for OpenThread RCP.
 *
 * Intentionally empty: the xiao_ble Zephyr board file already maps UART0
 * to P1.11 (TX, header D6) / P1.12 (RX, header D7), which matches the
 * ESP32-S3 wiring in the project README. Add pin remaps or current-speed
 * overrides here if the wiring changes.
 */
```

- [ ] **Step 2: Create the project Kconfig fragment**

Create `xiao-rcp/prj.conf` with content:
```conf
# Project-specific Kconfig overrides for the XIAO nRF52840 RCP build.
# Layered on top of samples/openthread/coprocessor/prj.conf via -DEXTRA_CONF_FILE.
# Intentionally empty: the upstream sample's defaults are correct for our use case.
```

- [ ] **Step 3: Create the build script**

Create `xiao-rcp/build.sh` with content:
```bash
#!/usr/bin/env bash
# Build the OpenThread RCP firmware for XIAO nRF52840 Plus.
#
# Usage:
#   ./build.sh               # build only
#   ./build.sh --uf2         # build and produce xiao-rcp.uf2 in repo root
#
# Must be invoked from inside an `nrfutil toolchain-manager launch --shell` session.

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
NCS_DIR="${NCS_DIR:-$HOME/ncs/v3.3.0}"
SAMPLE="$NCS_DIR/nrf/samples/openthread/coprocessor"
BUILD_DIR="$REPO_ROOT/xiao-rcp/build"

if ! command -v west >/dev/null 2>&1; then
  echo "error: west not on PATH. Run: nrfutil toolchain-manager launch --ncs-version v3.3.0 --shell" >&2
  exit 1
fi

if [ ! -d "$SAMPLE" ]; then
  echo "error: ncs sample not found at $SAMPLE. Did you run 'west update'?" >&2
  exit 1
fi

west build \
  -b xiao_ble/nrf52840 \
  -p always \
  -d "$BUILD_DIR" \
  "$SAMPLE" \
  -- \
  -DEXTRA_DTC_OVERLAY_FILE="$REPO_ROOT/xiao-rcp/boards/xiao_ble_nrf52840.overlay" \
  -DEXTRA_CONF_FILE="$REPO_ROOT/xiao-rcp/prj.conf"

HEX="$BUILD_DIR/coprocessor/zephyr/zephyr.hex"
if [ ! -f "$HEX" ]; then
  echo "error: build succeeded but $HEX not produced" >&2
  exit 1
fi

echo "build ok: $HEX"

if [ "${1:-}" = "--uf2" ]; then
  UF2_OUT="$REPO_ROOT/xiao-rcp/xiao-rcp.uf2"
  python3 "$REPO_ROOT/xiao-rcp/uf2conv.py" \
    --family 0xADA52840 \
    --convert \
    --output "$UF2_OUT" \
    "$HEX"
  echo "uf2 ok: $UF2_OUT"
fi
```

Then make it executable:
```bash
chmod +x xiao-rcp/build.sh
```

- [ ] **Step 4: Create `.gitignore` for the build output**

Create `xiao-rcp/.gitignore` with content:
```
build/
xiao-rcp.uf2
uf2conv.py
uf2families.json
```

- [ ] **Step 5: Replace `xiao-rcp/README.md` with the real build instructions**

Overwrite `xiao-rcp/README.md` with content:
````markdown
# xiao-rcp

OpenThread Radio Co-Processor firmware for the Seeed XIAO nRF52840 Plus.

Exposes the nRF52840's 802.15.4 radio to the ESP32-S3 hub over UART1 so the
hub can run OpenThread Border Router without its own radio silicon.

**Framework:** Zephyr + nRF Connect SDK v3.3.0, building the upstream
`samples/openthread/coprocessor` sample unchanged.

## Wiring

| XIAO pin | nRF52840 GPIO | Signal | → ESP32-S3 |
|----------|---------------|--------|------------|
| D6       | P1.11         | UART TX | GPIO2 (RX) |
| D7       | P1.12         | UART RX | GPIO1 (TX) |
| GND      | —             | GND    | GND        |

Baud rate: **1 000 000 bit/s** (sample default).

## Build

1. Install the toolchain (one time):
   ```bash
   curl -L -o /tmp/nrfutil https://files.nordicsemi.com/artifactory/swtools/external/nrfutil/executables/x86_64-apple-darwin/nrfutil
   chmod +x /tmp/nrfutil && sudo mv /tmp/nrfutil /usr/local/bin/nrfutil
   nrfutil install toolchain-manager
   nrfutil toolchain-manager install --ncs-version v3.3.0
   ```

2. Initialize the ncs workspace (one time, ~3 GB):
   ```bash
   mkdir -p ~/ncs && cd ~/ncs
   nrfutil toolchain-manager launch --ncs-version v3.3.0 --shell
   # inside the launched shell:
   west init -m https://github.com/nrfconnect/sdk-nrf --mr v3.3.0 v3.3.0
   cd v3.3.0 && west update
   ```

3. Build (inside the toolchain shell):
   ```bash
   cd /path/to/powerqueen/xiao-rcp
   ./build.sh --uf2
   ```
   Produces `xiao-rcp/xiao-rcp.uf2`.

## Flash

1. Double-tap the XIAO's reset button. The board mounts as `XIAO-SENSE`.
2. Drag `xiao-rcp.uf2` onto that volume.
3. The board reboots automatically into the new firmware.

## Verify

After flashing, the XIAO appears as a USB-CDC serial device but the RCP
does **not** respond to CLI input — it speaks the binary Spinel protocol
over UART pins D6/D7, not over USB. To verify:

- Connect XIAO D6/D7 to ESP32-S3 GPIO2/GPIO1 per the wiring table.
- Build the ESP-IDF `ot_br` example on the ESP32-S3 and check the boot log:
  it should report "RCP API Version: 6" and start an OpenThread network.
````

- [ ] **Step 6: Verify scaffold is in place**

Run:
```bash
ls -la xiao-rcp/
```
Expected: shows `README.md`, `build.sh` (executable), `prj.conf`, `boards/xiao_ble_nrf52840.overlay`, `.gitignore`.

- [ ] **Step 7: Commit the scaffold**

Run:
```bash
git add xiao-rcp/
git commit -m "xiao-rcp: scaffold Zephyr RCP build for XIAO nRF52840 Plus"
```

---

## Task 4: Download `uf2conv.py` into the repo

**Files:** Creates `xiao-rcp/uf2conv.py` and `xiao-rcp/uf2families.json` (both gitignored — these are Microsoft's reference tool, not our code).

- [ ] **Step 1: Download the canonical Microsoft UF2 converter**

Run:
```bash
curl -L -o xiao-rcp/uf2conv.py https://raw.githubusercontent.com/microsoft/uf2/master/utils/uf2conv.py
curl -L -o xiao-rcp/uf2families.json https://raw.githubusercontent.com/microsoft/uf2/master/utils/uf2families.json
chmod +x xiao-rcp/uf2conv.py
```

- [ ] **Step 2: Verify the converter runs**

Run:
```bash
python3 xiao-rcp/uf2conv.py --help
```
Expected: usage message listing `--family`, `--convert`, `--output` etc.

- [ ] **Step 3: Verify the Adafruit nRF52840 family ID is known**

Run:
```bash
python3 xiao-rcp/uf2conv.py --list-families | grep -i ada52840
```
Expected: a line showing `0xADA52840` associated with `ADA52840` or similar. If this fails, `uf2families.json` may be outdated — pass the family ID numerically via `--family 0xADA52840` (the build script already does this).

- [ ] **Step 4: Commit nothing**

`uf2conv.py` and `uf2families.json` are listed in `.gitignore` — they're vendor tooling, re-downloadable. Skip commit.

---

## Task 5: First build of the RCP firmware

**Files:** Produces `xiao-rcp/build/coprocessor/zephyr/zephyr.hex` and `xiao-rcp/xiao-rcp.uf2` (both gitignored).

- [ ] **Step 1: Launch the ncs toolchain shell**

Run:
```bash
nrfutil toolchain-manager launch --ncs-version v3.3.0 --shell
```
Expected: new shell prompt with `west` etc. on PATH.

- [ ] **Step 2: Run the build**

Inside the toolchain shell:
```bash
cd /Users/maidok/Developer/powerqueen/xiao-rcp
./build.sh --uf2
```
Expected:
- CMake configuration output (~30 s).
- Ninja build (~2–3 minutes on first build).
- `build ok: .../coprocessor/zephyr/zephyr.hex` printed.
- `uf2 ok: .../xiao-rcp.uf2` printed.

- [ ] **Step 3: Verify UF2 file looks sane**

Run:
```bash
ls -la xiao-rcp/xiao-rcp.uf2
file xiao-rcp/xiao-rcp.uf2
```
Expected:
- Size between 200 KB and 1.5 MB (RCP firmware is ~300–500 KB).
- `file` reports `data` (UF2 has no magic `file(1)` recognizes by default — size check is enough).

- [ ] **Step 4: Sanity-check the OpenThread config that was compiled in**

Run (inside toolchain shell):
```bash
grep -E "CONFIG_OPENTHREAD_(COPROCESSOR|NORDIC|FTD|MTD)" xiao-rcp/build/coprocessor/zephyr/.config | head -20
```
Expected: `CONFIG_OPENTHREAD_COPROCESSOR=y`, `CONFIG_OPENTHREAD_NORDIC_LIBRARY_RCP=y` both set to `y`. If either is missing, the sample did not build as RCP — stop and investigate.

- [ ] **Step 5: Commit nothing**

Build artifacts are gitignored. Skip commit.

---

## Task 6: Flash the firmware onto the XIAO

**Files:** None — physical action on hardware.

**Before you start:** Connect the XIAO to your Mac with a USB-C cable. Do **not** yet wire it to the ESP32.

- [ ] **Step 1: Put the XIAO into UF2 bootloader mode**

Double-tap the reset button on the XIAO within ~0.5 seconds. The onboard LED should pulse yellow/orange.

Run:
```bash
ls /Volumes/
```
Expected: `XIAO-SENSE` (or similar, e.g. `NRF52BOOT`) now appears.

If no volume appears, the double-tap timing is tricky — try again, slightly slower or faster. On a fresh XIAO it may also require a single tap.

- [ ] **Step 2: Copy the UF2 onto the board**

Run:
```bash
cp xiao-rcp/xiao-rcp.uf2 /Volumes/XIAO-SENSE/
```
Adjust the volume name if it's different. Expected: command returns in ~1 second. The XIAO automatically reboots and the volume disappears.

- [ ] **Step 3: Confirm the board rebooted into the new firmware**

Run:
```bash
ls /Volumes/ | grep -i xiao || echo "no xiao volume — board is running firmware"
```
Expected: `no xiao volume — board is running firmware`. The XIAO should also enumerate as a USB-CDC serial device:
```bash
ls /dev/tty.usbmodem*
```
Expected: at least one `tty.usbmodem*` entry.

- [ ] **Step 4: Commit nothing**

Hardware flash, no repo change.

---

## Task 7: Smoke-test the RCP over the USB-CDC serial port

**Files:** None — verification only.

The RCP speaks the binary **Spinel protocol** — it will not respond to typed CLI commands. But we can at least confirm the firmware booted without a fault by reading any startup bytes and checking power draw.

- [ ] **Step 1: Open the serial port and capture any output**

Run:
```bash
python3 -c "
import serial, time
s = serial.Serial('/dev/tty.usbmodem1101', 1000000, timeout=2)
time.sleep(0.5)
data = s.read(256)
print('got', len(data), 'bytes:', data.hex())
s.close()
"
```

Adjust `/dev/tty.usbmodem1101` to match the device that appeared in Task 6, Step 3.

Expected: a handful of bytes (likely starting with `7e` — the HDLC frame delimiter). If 0 bytes, the RCP might be awaiting host initialization (normal for Spinel); that's also acceptable for this smoke test.

**If `python3 -c "import serial"` fails with ModuleNotFoundError:**
```bash
pip3 install pyserial
```

- [ ] **Step 2: Press the XIAO reset button and watch for boot bytes**

While the capture script above is running, hit the reset button once. On reboot, the RCP should emit at least a few HDLC-framed bytes.

- [ ] **Step 3: Decide whether to proceed**

If Step 1 shows bytes starting with `7e`, you have a working RCP — move on to wiring it to the ESP32. If it shows 0 bytes across multiple boots, the firmware may not be running; re-run Task 5 and check the build output for silent failures.

- [ ] **Step 4: Commit nothing**

---

## Task 8: Archive the working UF2 in the repo (optional)

Only do this step once Task 7 succeeds AND a downstream ESP32-S3 OTBR test (not in scope of this plan) has confirmed the RCP actually talks Spinel correctly. Don't archive an unverified binary.

**Files:**
- Modify: `xiao-rcp/.gitignore` (remove the `xiao-rcp.uf2` line)
- Create: `xiao-rcp/prebuilt/xiao-rcp-v3.3.0.uf2`

- [ ] **Step 1: Move the verified UF2 into a `prebuilt/` directory**

Run:
```bash
mkdir -p xiao-rcp/prebuilt
cp xiao-rcp/xiao-rcp.uf2 xiao-rcp/prebuilt/xiao-rcp-v3.3.0.uf2
```

- [ ] **Step 2: Update `.gitignore` to keep `prebuilt/` tracked**

Edit `xiao-rcp/.gitignore` — remove the `xiao-rcp.uf2` line (the root-level copy stays ignored; the `prebuilt/` copy gets committed). New content:
```
build/
xiao-rcp.uf2
uf2conv.py
uf2families.json
```
(Actually, `xiao-rcp.uf2` stays — it only ignores root-level `xiao-rcp.uf2`, not `prebuilt/xiao-rcp-v3.3.0.uf2`. Verify with `git status` in next step.)

- [ ] **Step 3: Verify the prebuilt UF2 is tracked but the build output is not**

Run:
```bash
git status xiao-rcp/
```
Expected: `xiao-rcp/prebuilt/xiao-rcp-v3.3.0.uf2` shown as untracked/new; `xiao-rcp/build/`, `xiao-rcp/xiao-rcp.uf2`, `xiao-rcp/uf2conv.py` NOT listed.

- [ ] **Step 4: Commit the prebuilt binary with provenance**

Run:
```bash
git add xiao-rcp/prebuilt/xiao-rcp-v3.3.0.uf2
git commit -m "xiao-rcp: archive verified UF2 built from ncs v3.3.0"
```

---

## Task 9: Update the main `README.md` baud-rate spec

The project `README.md` currently states 460 800 baud for the ESP32↔XIAO link, but we shipped the RCP at 1 000 000 baud (the ncs sample default). Update the spec to match reality.

**Files:**
- Modify: `README.md` — lines around the GPIO table and Communication Protocols table.

- [ ] **Step 1: Update the GPIO table row for GPIO 1**

In `README.md`, find the line:
```
| 1 | XIAO nRF52840 TX | Output | UART1 | Thread RCP, 460800 baud |
```
Change `460800 baud` to `1000000 baud`.

- [ ] **Step 2: Update the Communication Protocols table**

In `README.md`, find the row:
```
| ESP32 ↔ XIAO | UART1 | 460800 | Thread RCP (HDLC frames) |
```
Change `460800` to `1000000`.

- [ ] **Step 3: Commit**

Run:
```bash
git add README.md
git commit -m "docs: update Thread RCP baud to 1 Mbit/s to match ncs sample default"
```

---

## Self-Review

**Spec coverage:**
- Flash RCP firmware onto XIAO nRF52840 Plus: Task 6.
- Build firmware from ncs source: Tasks 1–5.
- Match ESP-IDF OTBR requirements (RCP API v6): Task 2 (pinning ncs v3.3.0 ensures this).
- UART pin configuration matching ESP32-S3 wiring plan (GPIO1/GPIO2 ↔ XIAO D6/D7): Task 3 (documented in overlay + README).
- Verification: Task 7 (smoke test), Task 8 archive is predicated on downstream ESP32-side verification.

**Placeholder scan:** No TBDs, no "implement later" steps. Every code block and shell command is concrete.

**Consistency check:**
- `xiao_ble/nrf52840` board target used consistently across Task 3 (build.sh), Task 5.
- Family ID `0xADA52840` used consistently across build.sh (Task 3) and uf2conv.py verification (Task 4).
- Baud rate `1 000 000` consistent across Task 3 (README), Task 7 (smoke test), Task 9 (spec update).
- UART pins P1.11/P1.12 = D6/D7 consistent across overlay comment (Task 3), README wiring table (Task 3), and plan header.

**Known risks:**
- XIAO Plus hardware differs from base XIAO in ways we haven't verified — if the Plus's UF2 bootloader has a different volume name or family ID, Task 6 may need tweaks. Fallback is documented inline.
- `nrfutil` download URL may rot — if Step 1 of Task 1 fails, user can install via `brew install --cask nrf-connect-for-desktop` instead and use its GUI Toolchain Manager.
- The smoke test in Task 7 is weak: "got some bytes" doesn't prove the RCP is functional. True verification requires an ESP32-side OTBR build, which is out of scope here.
