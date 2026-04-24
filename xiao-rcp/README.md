# xiao-rcp

OpenThread Radio Co-Processor firmware for the Seeed XIAO nRF52840 Plus.

Exposes the nRF52840's 802.15.4 radio to the ESP32-S3 hub over UART1 so the
hub can run OpenThread Border Router without its own radio silicon.

**Framework:** Zephyr + nRF Connect SDK v3.3.0, building the upstream
`samples/openthread/coprocessor` sample unchanged.

## Wiring

| XIAO pin | nRF52840 GPIO | Signal  | → ESP32-S3   |
|----------|---------------|---------|--------------|
| D6       | P1.11         | UART TX | GPIO2 (RX)   |
| D7       | P1.12         | UART RX | GPIO1 (TX)   |
| GND      | —             | GND     | GND          |

Baud rate: **1 000 000 bit/s** (sample default).

## Build

### 1. Install the toolchain (one time)

```bash
curl -L -o /tmp/nrfutil https://files.nordicsemi.com/artifactory/swtools/external/nrfutil/executables/x86_64-apple-darwin/nrfutil
chmod +x /tmp/nrfutil && sudo mv /tmp/nrfutil /usr/local/bin/nrfutil
nrfutil install toolchain-manager
nrfutil toolchain-manager install --ncs-version v3.3.0
```

### 2. Initialize the ncs workspace (one time, ~3 GB)

```bash
mkdir -p ~/ncs
nrfutil toolchain-manager launch --ncs-version v3.3.0 --shell
# inside the launched shell:
cd ~/ncs
west init -m https://github.com/nrfconnect/sdk-nrf --mr v3.3.0 v3.3.0
cd v3.3.0 && west update
```

### 3. Build (inside the toolchain shell)

```bash
cd /path/to/powerqueen/xiao-rcp
./build.sh --uf2
```

Produces `xiao-rcp/xiao-rcp.uf2`.

## Flash

1. Double-tap the XIAO's reset button. The board mounts as `XIAO-SENSE`
   (volume name may vary — check `ls /Volumes/`).
2. Copy `xiao-rcp.uf2` onto that volume.
3. The board reboots automatically into the new firmware.

```bash
cp xiao-rcp/xiao-rcp.uf2 /Volumes/XIAO-SENSE/
```

## Verify

After flashing, the XIAO appears as a USB-CDC serial device but the RCP
does **not** respond to human CLI input — it speaks the binary Spinel
protocol over UART pins D6/D7, not over USB. To verify:

- Connect XIAO D6/D7 to ESP32-S3 GPIO2/GPIO1 per the wiring table.
- Build the ESP-IDF `ot_br` example on the ESP32-S3 and check the boot log:
  it should report "RCP API Version: 6" and start an OpenThread network.

## Layout

```
xiao-rcp/
├── README.md                          # This file
├── build.sh                           # Reproducible build wrapper
├── prj.conf                           # Kconfig overlay (empty today)
├── boards/
│   └── xiao_ble_nrf52840.overlay      # Devicetree overlay (empty today)
├── .gitignore                         # Ignores build/ and root-level UF2
├── build/                             # (gitignored) west build output
└── xiao-rcp.uf2                       # (gitignored) latest build output
```

The overlay designates `uart0` as the OpenThread Spinel transport via
`chosen { zephyr,ot-uart = &uart0; }` — the stock `xiao_ble` board file
doesn't set this because it isn't normally a Thread target. Everything else
(UART pins P1.11/P1.12, baud reconfigured to 1 Mbit/s at runtime,
`CONFIG_OPENTHREAD_COPROCESSOR=y`) comes from the ncs sample defaults.

Zephyr's sysbuild produces a UF2 directly, with the correct Adafruit
family ID (`0xADA52840`) and bootloader offset (`0x27000`) — no
`uf2conv.py` post-processing step is needed.
