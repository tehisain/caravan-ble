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
