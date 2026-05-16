# Wi-Fi OTA Self-Update — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make the ESP32-C6 in the caravan pull and self-install new firmware from GitHub Releases on boot, so future iterations no longer require physical USB access or SSH-tunneled `esptool`.

**Architecture:** Two new modules in `c6-hub/main/`. `wifi_manager.c` brings up Wi-Fi STA from Kconfig-stored credentials with retry/backoff. `ota_handler.c` polls the GitHub Releases API, compares `tag_name` against `esp_app_desc_t.version`, and invokes `esp_https_ota` if newer. On every successful boot, the app marks itself valid after a 60 s heartbeat; if the new image never reaches that point, the bootloader rolls back automatically. HTTPS validation uses ESP-IDF's built-in CA cert bundle (a small refinement of the spec, which proposed bundling DigiCert manually — the bundle is one Kconfig flag and survives GitHub changing TLS issuers).

**Tech Stack:** ESP-IDF v6.0, NimBLE (already integrated), `esp_wifi`, `esp_http_client`, `esp_https_ota`, `cJSON`, `mbedTLS` cert bundle, `gh` CLI for releases.

**Spec reference:** `docs/superpowers/specs/2026-05-16-ota-self-update-design.md`.

---

## File Structure

```
c6-hub/main/
├── CMakeLists.txt           MODIFY — add wifi_manager.c, ota_handler.c to SRCS
├── Kconfig.projbuild        NEW    — SSID, password, repo slug, asset name, timeouts
├── main.c                   MODIFY — boot-time wifi+ota sequence, delayed mark_valid
├── wifi_manager.h           NEW    — public API
├── wifi_manager.c           NEW    — STA bring-up, retry, event-group sync
├── ota_handler.h            NEW    — public API
└── ota_handler.c            NEW    — release fetch, version compare, https_ota call

c6-hub/sdkconfig.defaults    MODIFY — rollback enable, cert bundle on

c6-hub/release.sh            NEW    — wraps build.sh + `gh release create`

docs/superpowers/specs/
└── 2026-05-16-ota-self-update-design.md   MODIFY — note cert-bundle refinement
```

The cert-bundle choice replaces the manually-bundled DigiCert PEM from the spec. The plan applies that change to the spec's Architecture section in Task 1 so the two stay consistent.

---

## Key Decisions Locked In

| Decision | Value |
|---|---|
| Wi-Fi credentials | Kconfig (`menuconfig`), stored in local `sdkconfig` (gitignored) |
| Wi-Fi retry policy | 5 attempts, backoff 1/2/4/8/15 s (30 s total budget) |
| OTA trigger | Boot-time, once, after Wi-Fi GOT_IP |
| Release host | GitHub Releases on `tehisain/caravan-ble` (default; configurable) |
| Release asset | `c6-hub.bin` (default; configurable) |
| Tag scheme | `fw-<git-short-sha>` |
| Version compare | String inequality vs `"fw-" + esp_app_get_description()->version` |
| TLS validation | `CONFIG_MBEDTLS_CERTIFICATE_BUNDLE=y` (ESP-IDF's bundled CAs) |
| Rollback | `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y`; `esp_ota_mark_app_valid_cancel_rollback()` called after 60 s of healthy operation |
| Failure policy | Every OTA-path failure logs and falls through to normal app |

---

## Task 1 — Kconfig, sdkconfig, spec refinement

**Files:**
- Create: `c6-hub/main/Kconfig.projbuild`
- Modify: `c6-hub/sdkconfig.defaults`
- Modify: `docs/superpowers/specs/2026-05-16-ota-self-update-design.md`

- [ ] **Step 1: Create `Kconfig.projbuild`**

Write `c6-hub/main/Kconfig.projbuild`:

```kconfig
menu "Caravan OTA"

config OTA_WIFI_SSID
    string "Wi-Fi SSID for OTA backhaul"
    default ""
    help
      SSID of the caravan AP. Leave empty to disable Wi-Fi/OTA entirely.

config OTA_WIFI_PASSWORD
    string "Wi-Fi password"
    default ""

config OTA_GITHUB_REPO
    string "GitHub repo slug"
    default "tehisain/caravan-ble"
    help
      The repo whose /releases/latest is polled. Format: user/repo.

config OTA_ASSET_NAME
    string "Release asset filename"
    default "c6-hub.bin"

config OTA_WIFI_TIMEOUT_MS
    int "Wi-Fi connect timeout (ms)"
    default 30000

config OTA_MARK_VALID_DELAY_MS
    int "Delay before marking app valid (ms)"
    default 60000

endmenu
```

- [ ] **Step 2: Extend `sdkconfig.defaults`**

Edit `c6-hub/sdkconfig.defaults`, append after the BLE section:

```conf
# OTA — rollback + bundled CA roots
CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y
CONFIG_MBEDTLS_CERTIFICATE_BUNDLE=y
CONFIG_MBEDTLS_CERTIFICATE_BUNDLE_DEFAULT_FULL=y
```

- [ ] **Step 3: Update the spec to match**

Edit `docs/superpowers/specs/2026-05-16-ota-self-update-design.md`:

- In the **Architecture / `ota_handler.{c,h}`** section, replace the line `- DigiCert root CA bundled at c6-hub/main/certs/digicert_root.pem (GitHub's TLS issuer).` with:
  `- HTTPS validated against ESP-IDF's bundled CA roots (CONFIG_MBEDTLS_CERTIFICATE_BUNDLE=y). Survives GitHub changing TLS issuers without a firmware change.`
- In **Repo Changes**, remove the `certs/digicert_root.pem` line.

- [ ] **Step 4: Verify Kconfig is picked up**

```bash
cd c6-hub
./build.sh menuconfig
```

Expected: `Caravan OTA` menu appears at the top level of menuconfig with six items. Exit without saving (press `Q`, choose `N`).

- [ ] **Step 5: Commit**

```bash
git add c6-hub/main/Kconfig.projbuild c6-hub/sdkconfig.defaults \
        docs/superpowers/specs/2026-05-16-ota-self-update-design.md
git commit -m "c6-hub: Kconfig + sdkconfig prep for OTA"
```

---

## Task 2 — `wifi_manager` module

**Files:**
- Create: `c6-hub/main/wifi_manager.h`
- Create: `c6-hub/main/wifi_manager.c`
- Modify: `c6-hub/main/CMakeLists.txt`
- Modify: `c6-hub/main/main.c`

- [ ] **Step 1: Write `wifi_manager.h`**

```c
#pragma once

#include <stdint.h>
#include "esp_err.h"

// Initialize netif/event-loop/Wi-Fi, start STA association with the SSID
// from Kconfig. Returns ESP_ERR_INVALID_STATE if no SSID is configured
// (in which case OTA should be skipped entirely).
esp_err_t wifi_manager_init(void);

// Block until associated (got IP) or the timeout elapses. Returns ESP_OK
// on success, ESP_ERR_TIMEOUT otherwise.
esp_err_t wifi_manager_wait_connected(uint32_t timeout_ms);
```

- [ ] **Step 2: Write `wifi_manager.c`**

```c
#include "wifi_manager.h"

#include <string.h>
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_log.h"
#include "sdkconfig.h"

#define BIT_CONNECTED  BIT0
#define BIT_FAILED     BIT1

static const char *TAG = "wifi";
static EventGroupHandle_t s_events;
static int s_retries;

static const uint32_t s_backoff_ms[] = {1000, 2000, 4000, 8000, 15000};
#define MAX_RETRIES (sizeof(s_backoff_ms) / sizeof(s_backoff_ms[0]))

static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
        return;
    }
    if (id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_retries < (int)MAX_RETRIES) {
            uint32_t delay = s_backoff_ms[s_retries];
            ESP_LOGW(TAG, "disconnected; retry %d/%d in %" PRIu32 " ms",
                     s_retries + 1, (int)MAX_RETRIES, delay);
            vTaskDelay(pdMS_TO_TICKS(delay));
            s_retries++;
            esp_wifi_connect();
        } else {
            ESP_LOGE(TAG, "all %d retries exhausted", (int)MAX_RETRIES);
            xEventGroupSetBits(s_events, BIT_FAILED);
        }
    }
}

static void on_ip_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "got ip " IPSTR, IP2STR(&e->ip_info.ip));
        s_retries = 0;
        xEventGroupSetBits(s_events, BIT_CONNECTED);
    }
}

esp_err_t wifi_manager_init(void)
{
    if (strlen(CONFIG_OTA_WIFI_SSID) == 0) {
        ESP_LOGW(TAG, "no SSID configured; skipping Wi-Fi");
        return ESP_ERR_INVALID_STATE;
    }
    s_events = xEventGroupCreate();
    if (!s_events) return ESP_ERR_NO_MEM;

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &on_wifi_event, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &on_ip_event, NULL, NULL));

    wifi_config_t wc = {0};
    strncpy((char *)wc.sta.ssid,     CONFIG_OTA_WIFI_SSID,     sizeof(wc.sta.ssid)     - 1);
    strncpy((char *)wc.sta.password, CONFIG_OTA_WIFI_PASSWORD, sizeof(wc.sta.password) - 1);

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_LOGI(TAG, "connecting to '%s'", CONFIG_OTA_WIFI_SSID);
    return ESP_OK;
}

esp_err_t wifi_manager_wait_connected(uint32_t timeout_ms)
{
    EventBits_t b = xEventGroupWaitBits(s_events,
                                        BIT_CONNECTED | BIT_FAILED,
                                        pdFALSE, pdFALSE,
                                        pdMS_TO_TICKS(timeout_ms));
    if (b & BIT_CONNECTED) return ESP_OK;
    return ESP_ERR_TIMEOUT;
}
```

- [ ] **Step 3: Add `wifi_manager.c` to the build**

Edit `c6-hub/main/CMakeLists.txt`:

```cmake
idf_component_register(
    SRCS "main.c" "wifi_manager.c"
    INCLUDE_DIRS "."
)
```

- [ ] **Step 4: Hook into `main.c`**

Edit `c6-hub/main/main.c`:

- Add include near the top:
  ```c
  #include "wifi_manager.h"
  ```
- In `app_main`, after the `nvs_flash_init` block and **before** `ble_init()`:
  ```c
      if (wifi_manager_init() == ESP_OK) {
          if (wifi_manager_wait_connected(CONFIG_OTA_WIFI_TIMEOUT_MS) == ESP_OK) {
              ESP_LOGI(TAG, "wifi up");
          } else {
              ESP_LOGW(TAG, "wifi timeout, continuing without it");
          }
      }
  ```

- [ ] **Step 5: Configure SSID/password**

```bash
cd c6-hub
./build.sh menuconfig
```

Navigate to `Caravan OTA → Wi-Fi SSID / Wi-Fi password`, type the caravan AP credentials, save, exit.

- [ ] **Step 6: Build, flash via Pi, capture boot log**

```bash
cd c6-hub
./build.sh
cd build
tar -czf /tmp/c6-hub-flash.tar.gz \
    bootloader/bootloader.bin partition_table/partition-table.bin \
    ota_data_initial.bin c6-hub.bin
scp /tmp/c6-hub-flash.tar.gz caravan:~/c6-hub-flash/
ssh caravan 'cd ~/c6-hub-flash && tar -xzf c6-hub-flash.tar.gz && \
    ~/.local/esptool-venv/bin/esptool.py --chip esp32c6 -p /dev/ttyACM0 \
    --before default-reset --after hard-reset write-flash \
    --flash-mode dio --flash-size 16MB --flash-freq 80m \
    0x0 bootloader/bootloader.bin \
    0x8000 partition_table/partition-table.bin \
    0xf000 ota_data_initial.bin \
    0x20000 c6-hub.bin'
ssh caravan 'python3 -c "
import serial, sys, time
s = serial.Serial(\"/dev/ttyACM0\", 115200, timeout=0.3)
s.setDTR(False); s.setRTS(True); time.sleep(0.1); s.setRTS(False)
end = time.time() + 20
while time.time() < end:
    line = s.readline()
    if line: sys.stdout.write(line.decode(errors=\"replace\"))
"'
```

Expected: within ~10 s of boot, log shows `wifi: connecting to '<SSID>'` then `wifi: got ip <addr>` then `caravan: wifi up`, followed by BLE scanner output.

- [ ] **Step 7: Commit**

```bash
git add c6-hub/main/wifi_manager.h c6-hub/main/wifi_manager.c \
        c6-hub/main/CMakeLists.txt c6-hub/main/main.c
git commit -m "c6-hub: wifi_manager module for OTA backhaul"
```

---

## Task 3 — `ota_handler` release fetch + version compare (no flash yet)

**Files:**
- Create: `c6-hub/main/ota_handler.h`
- Create: `c6-hub/main/ota_handler.c`
- Modify: `c6-hub/main/CMakeLists.txt`
- Modify: `c6-hub/main/main.c`

- [ ] **Step 1: Write `ota_handler.h`**

```c
#pragma once

#include "esp_err.h"

// Check GitHub Releases for a newer firmware than what's running.
// In this task: log the result only. Actual download/flash arrives in Task 4.
// Returns ESP_OK if the check completed (regardless of whether an update
// would apply); error code on transport failure.
esp_err_t ota_check_and_update(void);
```

- [ ] **Step 2: Write `ota_handler.c` — check-only first iteration**

```c
#include "ota_handler.h"

#include <string.h>
#include "esp_log.h"
#include "esp_app_desc.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "cJSON.h"
#include "sdkconfig.h"

static const char *TAG = "ota";

#define API_URL_FMT "https://api.github.com/repos/%s/releases/latest"

static char  s_json[6144];
static int   s_json_len;

static esp_err_t on_http_event(esp_http_client_event_t *evt)
{
    if (evt->event_id == HTTP_EVENT_ON_DATA && !esp_http_client_is_chunked_response(evt->client)) {
        int room = (int)sizeof(s_json) - 1 - s_json_len;
        int n = evt->data_len < room ? evt->data_len : room;
        if (n > 0) {
            memcpy(s_json + s_json_len, evt->data, n);
            s_json_len += n;
            s_json[s_json_len] = 0;
        }
    }
    return ESP_OK;
}

esp_err_t ota_check_and_update(void)
{
    char url[160];
    snprintf(url, sizeof(url), API_URL_FMT, CONFIG_OTA_GITHUB_REPO);
    ESP_LOGI(TAG, "checking %s", url);

    s_json_len = 0;
    s_json[0] = 0;

    esp_http_client_config_t cfg = {
        .url = url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .event_handler = on_http_event,
        .user_agent = "c6-hub/1.0",
        .timeout_ms = 10000,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    esp_err_t err = esp_http_client_perform(c);
    int status = esp_http_client_get_status_code(c);
    esp_http_client_cleanup(c);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "http perform: %s", esp_err_to_name(err));
        return err;
    }
    if (status == 404) {
        ESP_LOGI(TAG, "no releases yet on this repo");
        return ESP_OK;
    }
    if (status != 200) {
        ESP_LOGE(TAG, "http status %d", status);
        return ESP_FAIL;
    }

    cJSON *root = cJSON_Parse(s_json);
    if (!root) {
        ESP_LOGE(TAG, "json parse failed");
        return ESP_FAIL;
    }
    cJSON *tag = cJSON_GetObjectItem(root, "tag_name");
    if (!cJSON_IsString(tag)) {
        ESP_LOGE(TAG, "json missing tag_name");
        cJSON_Delete(root);
        return ESP_FAIL;
    }

    const esp_app_desc_t *desc = esp_app_get_description();
    char expected[80];
    snprintf(expected, sizeof(expected), "fw-%s", desc->version);

    if (strcmp(tag->valuestring, expected) == 0) {
        ESP_LOGI(TAG, "already current (%s)", tag->valuestring);
    } else {
        ESP_LOGI(TAG, "update available: running %s, release %s",
                 expected, tag->valuestring);
    }
    cJSON_Delete(root);
    return ESP_OK;
}
```

- [ ] **Step 3: Add to build**

Edit `c6-hub/main/CMakeLists.txt`:

```cmake
idf_component_register(
    SRCS "main.c" "wifi_manager.c" "ota_handler.c"
    INCLUDE_DIRS "."
)
```

- [ ] **Step 4: Hook into `main.c`**

Edit `c6-hub/main/main.c`:

- Add include:
  ```c
  #include "ota_handler.h"
  ```
- In `app_main`, replace the `ESP_LOGI(TAG, "wifi up");` line with:
  ```c
              ota_check_and_update();
  ```

- [ ] **Step 5: Build, flash via Pi, capture boot log**

Same flow as Task 2 Step 6. Expected new lines after `wifi: got ip ...`:

- If no releases exist yet on the repo:
  ```
  I (...) ota: checking https://api.github.com/repos/tehisain/caravan-ble/releases/latest
  I (...) ota: no releases yet on this repo
  ```
- If a release exists with a different tag (you create one as Task 4 verification):
  ```
  I (...) ota: update available: running fw-<sha>, release fw-<other-sha>
  ```

- [ ] **Step 6: Commit**

```bash
git add c6-hub/main/ota_handler.h c6-hub/main/ota_handler.c \
        c6-hub/main/CMakeLists.txt c6-hub/main/main.c
git commit -m "c6-hub: ota_handler — GitHub release check + version compare"
```

---

## Task 4 — Full update path: `esp_https_ota` + delayed mark_valid

**Files:**
- Modify: `c6-hub/main/ota_handler.h`
- Modify: `c6-hub/main/ota_handler.c`
- Modify: `c6-hub/main/main.c`

- [ ] **Step 1: Add `ota_mark_valid()` to the header**

Edit `c6-hub/main/ota_handler.h`, append:

```c
// Mark the running image as valid so the bootloader stops considering rollback.
// Safe to call repeatedly; no-op unless the image is in PENDING_VERIFY state.
void ota_mark_valid(void);
```

- [ ] **Step 2: Replace `ota_check_and_update` body with the full path**

Edit `c6-hub/main/ota_handler.c`. Add these includes near the top:

```c
#include "esp_https_ota.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
```

Replace the **body of `ota_check_and_update`** from the `if (strcmp(tag->valuestring, expected) == 0)` block to the end of the function with:

```c
    if (strcmp(tag->valuestring, expected) == 0) {
        ESP_LOGI(TAG, "already current (%s)", tag->valuestring);
        cJSON_Delete(root);
        return ESP_OK;
    }
    ESP_LOGI(TAG, "update available: running %s, release %s",
             expected, tag->valuestring);

    char asset_url[256] = {0};
    cJSON *assets = cJSON_GetObjectItem(root, "assets");
    if (cJSON_IsArray(assets)) {
        cJSON *a;
        cJSON_ArrayForEach(a, assets) {
            cJSON *name = cJSON_GetObjectItem(a, "name");
            cJSON *u    = cJSON_GetObjectItem(a, "browser_download_url");
            if (cJSON_IsString(name) && cJSON_IsString(u)
                && strcmp(name->valuestring, CONFIG_OTA_ASSET_NAME) == 0) {
                strncpy(asset_url, u->valuestring, sizeof(asset_url) - 1);
                break;
            }
        }
    }
    cJSON_Delete(root);

    if (asset_url[0] == 0) {
        ESP_LOGW(TAG, "release has no '%s' asset; aborting", CONFIG_OTA_ASSET_NAME);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "downloading %s", asset_url);

    esp_http_client_config_t ota_http = {
        .url = asset_url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 30000,
        .keep_alive_enable = true,
    };
    esp_https_ota_config_t hcfg = {
        .http_config = &ota_http,
    };
    err = esp_https_ota(&hcfg);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "OTA ok, rebooting");
        esp_restart();
    }
    ESP_LOGE(TAG, "esp_https_ota: %s", esp_err_to_name(err));
    return err;
}

void ota_mark_valid(void)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t state;
    if (esp_ota_get_state_partition(running, &state) != ESP_OK) {
        ESP_LOGW(TAG, "could not read OTA state");
        return;
    }
    if (state == ESP_OTA_IMG_PENDING_VERIFY) {
        if (esp_ota_mark_app_valid_cancel_rollback() == ESP_OK) {
            ESP_LOGI(TAG, "marked app valid; rollback cancelled");
        }
    } else {
        ESP_LOGI(TAG, "no rollback to cancel (state=%d)", (int)state);
    }
}
```

- [ ] **Step 3: Wire `ota_mark_valid` into the heartbeat loop**

Edit `c6-hub/main/main.c`. Replace the `while (true)` block in `app_main` with:

```c
    TickType_t boot_tick = xTaskGetTickCount();
    bool marked = false;
    uint32_t tick = 0;
    while (true) {
        if (!marked && (xTaskGetTickCount() - boot_tick)
                       >= pdMS_TO_TICKS(CONFIG_OTA_MARK_VALID_DELAY_MS)) {
            ota_mark_valid();
            marked = true;
        }
        ESP_LOGI(TAG, "alive tick=%" PRIu32, tick++);
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
```

- [ ] **Step 4: Build, flash via Pi**

Run the same build+flash+monitor sequence as Task 2 Step 6. Expected: with no release present, boot log shows `ota: no releases yet on this repo`, app continues normally, and after ~60 s logs `ota: marked app valid; rollback cancelled`.

- [ ] **Step 5: Commit baseline**

```bash
git add c6-hub/main/ota_handler.h c6-hub/main/ota_handler.c c6-hub/main/main.c
git commit -m "c6-hub: full OTA update path + delayed mark_valid"
```

- [ ] **Step 6: Cut the first GitHub release manually**

This commit becomes the **baseline** release the C6 will eventually upgrade away from.

```bash
SHA=$(git rev-parse --short HEAD)
gh release create "fw-$SHA" c6-hub/build/c6-hub.bin \
    --title "fw-$SHA" --notes "Baseline OTA release"
```

Verify on GitHub: <https://github.com/tehisain/caravan-ble/releases> shows `fw-<sha>` with one asset.

- [ ] **Step 7: Power-cycle the C6 and verify "already current"**

```bash
ssh caravan 'python3 -c "
import serial, sys, time
s = serial.Serial(\"/dev/ttyACM0\", 115200, timeout=0.3)
s.setDTR(False); s.setRTS(True); time.sleep(0.1); s.setRTS(False)
end = time.time() + 20
while time.time() < end:
    line = s.readline()
    if line: sys.stdout.write(line.decode(errors=\"replace\"))
"'
```

Expected: boot log includes `ota: already current (fw-<sha>)`.

---

## Task 5 — Verify the upgrade path end-to-end

**Files:**
- Modify: `c6-hub/main/main.c` (whitespace / trivial change to force a new SHA)

- [ ] **Step 1: Make a no-op change to force a new commit SHA**

Edit `c6-hub/main/main.c` and bump the boot log:

```c
    ESP_LOGI(TAG, "boot");
```
→
```c
    ESP_LOGI(TAG, "boot (OTA upgrade test)");
```

- [ ] **Step 2: Commit**

```bash
git add c6-hub/main/main.c
git commit -m "c6-hub: bump boot log to test OTA upgrade"
```

- [ ] **Step 3: Build and release the new version**

```bash
cd c6-hub && ./build.sh && cd ..
SHA=$(git rev-parse --short HEAD)
gh release create "fw-$SHA" c6-hub/build/c6-hub.bin \
    --title "fw-$SHA" --notes "OTA upgrade test"
```

GitHub now has two releases; `latest` is the new one.

- [ ] **Step 4: Power-cycle the C6 and watch the upgrade**

Use the same monitor command as Task 4 Step 7, but increase the window to 90 s to capture download + reboot + new app boot:

```bash
ssh caravan 'python3 -c "
import serial, sys, time
s = serial.Serial(\"/dev/ttyACM0\", 115200, timeout=0.3)
s.setDTR(False); s.setRTS(True); time.sleep(0.1); s.setRTS(False)
end = time.time() + 90
while time.time() < end:
    line = s.readline()
    if line: sys.stdout.write(line.decode(errors=\"replace\"))
"'
```

Expected sequence:
1. `ota: update available: running fw-<old-sha>, release fw-<new-sha>`
2. `ota: downloading https://objects.githubusercontent.com/...`
3. `esp_https_ota: ...` progress lines from ESP-IDF
4. `ota: OTA ok, rebooting`
5. Fresh boot: bootloader log → `App version: <new-sha>` → `caravan: boot (OTA upgrade test)`
6. After ~60 s: `ota: marked app valid; rollback cancelled`

If you see this whole sequence, OTA is working.

---

## Task 6 — Verify rollback with a deliberately broken release

**Files:**
- Modify: `c6-hub/main/main.c` (introduce a crash, then revert)

This task validates that `BOOTLOADER_APP_ROLLBACK_ENABLE` actually rolls back. **No code lands in `main` from this task** — the breakage is intentional and gets reverted at the end.

- [ ] **Step 1: Branch off `main`**

```bash
git switch -c ota-rollback-test
```

- [ ] **Step 2: Introduce an early crash**

Edit `c6-hub/main/main.c`. At the very top of `app_main`, before any other code:

```c
    ESP_LOGE(TAG, "deliberate rollback test crash");
    abort();
```

- [ ] **Step 3: Build, commit, release**

```bash
cd c6-hub && ./build.sh && cd ..
git add c6-hub/main/main.c
git commit -m "TEST: deliberate crash to verify OTA rollback (do not merge)"
SHA=$(git rev-parse --short HEAD)
gh release create "fw-$SHA" c6-hub/build/c6-hub.bin \
    --title "fw-$SHA" --notes "Rollback test — do not deploy"
```

- [ ] **Step 4: Power-cycle the C6 and observe rollback**

Same long monitor as Task 5 Step 4 (90 s window).

Expected:
1. C6 boots running the prior good image.
2. Detects the new release, downloads, flashes, reboots.
3. New image runs `app_main`, logs `deliberate rollback test crash`, aborts.
4. Bootloader sees `ESP_OTA_IMG_PENDING_VERIFY` was never confirmed → marks aborted slot invalid → boots the previous slot.
5. Log shows the **old** SHA running again, and `ota: already current (fw-<old-sha>)` would falsely report — actually it'll see the new release as still newer. So expect: another OTA attempt → another crash → another rollback. The device is now in a rollback loop until you delete the bad release.

- [ ] **Step 5: Delete the broken release on GitHub**

```bash
gh release delete "fw-$SHA" --yes --cleanup-tag
```

- [ ] **Step 6: Reset back to `main` and clean up**

```bash
git switch main
git branch -D ota-rollback-test
```

The local commit is discarded. The release is deleted. The next power-cycle finds `fw-<task5-sha>` as `latest` again and stays on it.

- [ ] **Step 7: Confirm device is back on a healthy image**

```bash
ssh caravan 'python3 -c "
import serial, sys, time
s = serial.Serial(\"/dev/ttyACM0\", 115200, timeout=0.3)
s.setDTR(False); s.setRTS(True); time.sleep(0.1); s.setRTS(False)
end = time.time() + 20
while time.time() < end:
    line = s.readline()
    if line: sys.stdout.write(line.decode(errors=\"replace\"))
"'
```

Expected: boot log shows the Task-5 SHA, and `ota: already current (fw-<task5-sha>)`.

---

## Task 7 — `release.sh` helper

**Files:**
- Create: `c6-hub/release.sh`

- [ ] **Step 1: Write the script**

```bash
#!/usr/bin/env bash
# Cut a new firmware release on GitHub from the current commit.
#
# Refuses to run if the tree is dirty (so the running binary's
# version string matches the tag).
set -euo pipefail

cd "$(dirname "${BASH_SOURCE[0]}")"

if ! git diff --quiet || ! git diff --cached --quiet; then
    echo "error: working tree has uncommitted changes; commit or stash first" >&2
    exit 1
fi

SHA=$(git rev-parse --short HEAD)
TAG="fw-$SHA"

if gh release view "$TAG" >/dev/null 2>&1; then
    echo "error: release $TAG already exists" >&2
    exit 1
fi

./build.sh

gh release create "$TAG" build/c6-hub.bin \
    --title "$TAG" \
    --notes "Built from $(git log -1 --pretty=%s)"

echo "released $TAG"
```

- [ ] **Step 2: Make it executable**

```bash
chmod +x c6-hub/release.sh
```

- [ ] **Step 3: Dry-run smoke test**

```bash
# Should error because the latest release tag (from Task 5) already exists
c6-hub/release.sh
```

Expected: `error: release fw-<sha> already exists` and exit code 1.

- [ ] **Step 4: Commit**

```bash
git add c6-hub/release.sh
git commit -m "c6-hub: release.sh helper for cutting firmware releases"
```

- [ ] **Step 5: Push the branch**

```bash
git push origin main
```

---

## Acceptance Criteria (from spec)

After all tasks complete, verify against the spec:

1. ✅ Wi-Fi connect within 30 s of boot — observed in Task 2 Step 6.
2. ✅ `release.sh` creates `fw-<sha>` release with `c6-hub.bin` — Task 7 Step 3.
3. ✅ Power-cycle pulls new release, reboots, reports new SHA — Task 5 Step 4.
4. ✅ Broken release rolls back within one boot cycle — Task 6 Step 4.
5. ✅ No new release ⇒ boot completes normally + BLE scanner runs — Task 4 Step 4.

---

## Notes for the engineer

- **`CONFIG_OTA_GITHUB_REPO`** defaults to `tehisain/caravan-ble`. If you fork or rename, change it via `menuconfig` and rebuild.
- **GitHub rate limit**: anonymous API hits are 60/hour per IP. With boot-time-only polling that's fine, but if you find yourself rebooting many times for testing, you'll see HTTP 403. The fix is to wait an hour or use a `GITHUB_TOKEN` (out of scope).
- **First flash is still via esptool over SSH** — there's no way for the C6 to OTA onto itself the very first time. Only subsequent updates use OTA.
- **The `app_init: App version` line in the boot log is the source of truth** for what's running. ESP-IDF derives it from `git describe --always --dirty`. A `-dirty` suffix means the build was made from an uncommitted tree and `release.sh` refuses to use it.
