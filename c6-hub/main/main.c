#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_mac.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "wifi_manager.h"
#include "ota_handler.h"
#include "sdkconfig.h"

static const char *TAG = "caravan";
static const char *BLE = "ble";

static void log_chip_info(void)
{
    esp_chip_info_t chip;
    esp_chip_info(&chip);

    uint32_t flash_size = 0;
    esp_flash_get_size(NULL, &flash_size);

    uint8_t mac[8] = {0};
    esp_read_mac(mac, ESP_MAC_IEEE802154);

    ESP_LOGI(TAG, "model=%d cores=%d revision=%d.%d",
             chip.model, chip.cores,
             chip.revision / 100, chip.revision % 100);
    ESP_LOGI(TAG, "features: WiFi=%d BT=%d BLE=%d 802.15.4=%d",
             (chip.features & CHIP_FEATURE_WIFI_BGN) != 0,
             (chip.features & CHIP_FEATURE_BT) != 0,
             (chip.features & CHIP_FEATURE_BLE) != 0,
             (chip.features & CHIP_FEATURE_IEEE802154) != 0);
    ESP_LOGI(TAG, "flash=%" PRIu32 " bytes (%" PRIu32 " MB)",
             flash_size, flash_size / (1024 * 1024));
    ESP_LOGI(TAG, "ieee802154 mac=%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x",
             mac[0], mac[1], mac[2], mac[3],
             mac[4], mac[5], mac[6], mac[7]);
}

static int ble_gap_event_cb(struct ble_gap_event *event, void *arg)
{
    if (event->type != BLE_GAP_EVENT_DISC) {
        return 0;
    }
    const uint8_t *a = event->disc.addr.val;
    struct ble_hs_adv_fields f;
    char name[32] = "(no name)";
    if (ble_hs_adv_parse_fields(&f, event->disc.data, event->disc.length_data) == 0
        && f.name_len > 0) {
        int n = f.name_len < (int)sizeof(name) - 1 ? f.name_len : (int)sizeof(name) - 1;
        memcpy(name, f.name, n);
        name[n] = 0;
    }
    // ble_addr_t stores the address LSB-first; print MSB-first to match other tools
    ESP_LOGI(BLE, "%02x:%02x:%02x:%02x:%02x:%02x  rssi=%4d  %s",
             a[5], a[4], a[3], a[2], a[1], a[0],
             event->disc.rssi, name);
    return 0;
}

static void ble_on_sync(void)
{
    uint8_t own_addr_type;
    int rc = ble_hs_id_infer_auto(0, &own_addr_type);
    if (rc != 0) {
        ESP_LOGE(BLE, "infer addr type failed: %d", rc);
        return;
    }
    struct ble_gap_disc_params dp = {
        .itvl = 0,
        .window = 0,
        .filter_policy = 0,
        .limited = 0,
        .passive = 1,
        .filter_duplicates = 1,
    };
    rc = ble_gap_disc(own_addr_type, BLE_HS_FOREVER, &dp, ble_gap_event_cb, NULL);
    if (rc != 0) {
        ESP_LOGE(BLE, "scan start failed: %d", rc);
    } else {
        ESP_LOGI(BLE, "passive scan started (dedup on)");
    }
}

static void ble_host_task(void *param)
{
    nimble_port_run();
    nimble_port_freertos_deinit();
}

static void ble_init(void)
{
    esp_err_t err = nimble_port_init();
    if (err != ESP_OK) {
        ESP_LOGE(BLE, "nimble_port_init failed: %d", err);
        return;
    }
    ble_hs_cfg.sync_cb = ble_on_sync;
    nimble_port_freertos_init(ble_host_task);
}

void app_main(void)
{
    ESP_LOGI(TAG, "boot");
    log_chip_info();

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }

    if (wifi_manager_init() == ESP_OK) {
        if (wifi_manager_wait_connected(CONFIG_OTA_WIFI_TIMEOUT_MS) == ESP_OK) {
            ota_check_and_update();
        } else {
            ESP_LOGW(TAG, "wifi timeout, continuing without it");
        }
    }

    ble_init();

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
}
