# Wi-Fi OTA Self-Update — Design

**Date:** 2026-05-16
**Status:** Approved, ready for plan

## Overview

The ESP32-C6 is now installed in the caravan and reachable only via the Raspberry Pi's SSH tunnel. Every firmware iteration currently means `scp` + `esptool` over a cellular Tailscale link. Wi-Fi OTA collapses that loop: the C6 pulls new firmware directly from GitHub Releases when the caravan AP has internet.

This unblocks all subsequent firmware work — `power_monitor`, `state_machine`, `sensors`, etc. — by removing the only step that requires the Pi.

## Goals

- C6 pulls and self-installs new firmware from GitHub Releases without USB.
- Failure of any OTA step is non-fatal: the device falls through to its normal app.
- Bricked uploads roll back automatically.

## Non-goals

- BLE GATT trigger for on-demand update — defer.
- Periodic polling — let the state machine drive this once it exists.
- GitHub Actions auto-release — manual `gh release` for now.
- Code signing / secure boot — relying on HTTPS cert validation only.
- Multi-board support — single C6 in `tehisain/caravan-ble` only.

## Architecture

Two new modules in `c6-hub/main/`:

### `wifi_manager.{c,h}`

Wi-Fi STA bring-up and lifecycle.

- `wifi_manager_init()` — register handlers, start Wi-Fi STA with `CONFIG_OTA_WIFI_SSID` / `CONFIG_OTA_WIFI_PASSWORD` from Kconfig.
- `wifi_manager_wait_connected(timeout_ms)` — block until `WIFI_EVENT_STA_GOT_IP` or timeout. Returns `ESP_OK` or `ESP_ERR_TIMEOUT`.
- Internal retry: up to 5 attempts with exponential backoff (1, 2, 4, 8, 15 s) before giving up.

### `ota_handler.{c,h}`

GitHub release check + `esp_https_ota` invocation.

- `ota_check_and_update()` — synchronous; returns when finished or failed.
  1. HTTPS GET `https://api.github.com/repos/$CONFIG_OTA_GITHUB_REPO/releases/latest` (repo slug from Kconfig, default `tehisain/caravan-ble`).
  2. Parse JSON (`cJSON`); locate the asset whose `name` equals `CONFIG_OTA_ASSET_NAME` (default `c6-hub.bin`) and read its `browser_download_url`.
  3. Compare `tag_name` against `"fw-" + esp_app_get_description()->version`.
  4. If different, run `esp_https_ota(&config)` against the asset URL. On success, `esp_restart()` (never returns).
- HTTPS validated against ESP-IDF's bundled CA roots (`CONFIG_MBEDTLS_CERTIFICATE_BUNDLE=y`). Survives GitHub changing TLS issuers without a firmware change.

### Boot flow

```
app_main
  ├─ nvs_init
  ├─ log_chip_info
  ├─ wifi_manager_init
  ├─ wifi_manager_wait_connected(30 000 ms)
  │     └─ on ESP_OK: ota_check_and_update()   ← may reboot here
  ├─ ble_init                                   ← reached only if no update applied
  └─ main loop (alive tick + BLE scan)
          └─ after 60 s healthy: esp_ota_mark_app_valid_cancel_rollback()
```

## Versioning & Release Workflow

- **Running version**: ESP-IDF injects the git short SHA into `esp_app_desc_t.version` (`+"-dirty"` if the tree has uncommitted changes).
- **Release tag**: `fw-<short-sha>` (e.g., `fw-ef693bc`).
- **Release asset**: single binary `c6-hub.bin`, copied from `c6-hub/build/`.
- **Comparison**: string inequality. `tag_name != "fw-" + version` ⇒ update. Safe for single-developer flow; we accept the downgrade risk.
- **Release command**: `release.sh` on the Mac wraps:
  ```
  ./build.sh
  SHA=$(git rev-parse --short HEAD)
  gh release create fw-$SHA build/c6-hub.bin --title "fw-$SHA" --notes "Auto-generated release"
  ```
  Refuses to run if the tree is dirty.

## Failure Modes & Rollback

| Scenario | Behavior |
|---|---|
| Wi-Fi connect times out (30 s) | Log + fall through to normal app |
| GitHub API unreachable / TLS failure | Log + skip OTA, continue |
| `tag_name` matches running version | Already current, skip |
| Download interrupted | `esp_https_ota` rejects via image-header validation; `ota_0` remains active |
| Power loss mid-flash | Bootloader sees unfinished `ota_1`, boots `ota_0` |
| New app crashes / never reaches Wi-Fi | `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y` reverts to `ota_0` because the new app never called `esp_ota_mark_app_valid_cancel_rollback()` |
| New app healthy 60 s post-boot | Calls `esp_ota_mark_app_valid_cancel_rollback()`; new slot is committed |

## Repo Changes

```
c6-hub/main/
  wifi_manager.{c,h}          new
  ota_handler.{c,h}           new
  Kconfig.projbuild           new — CONFIG_OTA_WIFI_SSID/PASSWORD + repo path + asset name
  main.c                      modified — pre-BLE wifi+ota sequence + mark_valid timer

c6-hub/sdkconfig.defaults     modified — rollback, esp_https_ota, mbedTLS CA bundle off
release.sh                    new — wraps build + gh release create
```

## Acceptance Criteria

1. After `idf.py menuconfig` sets SSID/PW, the C6 connects to caravan Wi-Fi within 30 s of boot.
2. Running `release.sh` from the Mac creates a GitHub release tagged `fw-<sha>` with `c6-hub.bin` attached.
3. Power-cycling the C6 (when caravan AP has internet) makes it pull the new release, flash, reboot, and report the new SHA in its boot log.
4. A deliberately broken release (e.g., immediate `assert(false)` in `app_main`) rolls back to the previous slot within one boot cycle.
5. With no new release available, boot completes normally and the BLE scanner continues to work.

## Out of Scope / Future

- BLE GATT characteristic to trigger an on-demand check.
- State-machine-driven periodic polling, gated on CAMPSITE / ARRIVED only.
- GitHub Actions workflow that builds and releases automatically on push to `main`.
- Semver tags with proper ordering.
- Secure boot + signed firmware.
