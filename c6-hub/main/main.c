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
#include "wifi_manager.h"
#include "ota_handler.h"
#include "power_monitor.h"
#include "sdkconfig.h"

static const char *TAG = "caravan";

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

    power_monitor_init();

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
