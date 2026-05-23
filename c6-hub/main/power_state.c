#include "power_state.h"

#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "power_monitor.h"

static const char *TAG = "ps";

#define IDLE_THRESHOLD_A       0.3f
#define FLOAT_SOC_THRESHOLD    95
#define BMS_STATE_FULL         4
#define EVAL_INTERVAL_MS       1000
#define FRESHNESS_MS           5000

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static power_state_reading_t s_cache;

static int64_t now_ms(void)
{
    return esp_timer_get_time() / 1000;
}

static const char *state_name(power_state_t st)
{
    switch (st) {
        case POWER_STATE_UNKNOWN:     return "UNKNOWN";
        case POWER_STATE_CHARGING:    return "CHARGING";
        case POWER_STATE_DISCHARGING: return "DISCHARGING";
        case POWER_STATE_FLOAT:       return "FLOAT";
        case POWER_STATE_FAULT:       return "FAULT";
    }
    return "?";
}

static power_state_t derive_state(const battery_reading_t *b)
{
    if (b->current_a > IDLE_THRESHOLD_A)  return POWER_STATE_CHARGING;
    if (b->current_a < -IDLE_THRESHOLD_A) return POWER_STATE_DISCHARGING;
    if (b->state == BMS_STATE_FULL)       return POWER_STATE_FLOAT;
    if (b->soc_pct >= FLOAT_SOC_THRESHOLD) return POWER_STATE_FLOAT;
    return POWER_STATE_FLOAT;
}

static void power_state_task(void *arg)
{
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(EVAL_INTERVAL_MS));

        battery_reading_t b;
        solar_reading_t   s;
        charger_reading_t c;
        bool b_ok = (power_monitor_get_battery(&b) == ESP_OK);
        bool s_ok = (power_monitor_get_solar(&s)   == ESP_OK);
        bool c_ok = (power_monitor_get_charger(&c) == ESP_OK);

        power_state_reading_t now = {
            .valid = true,
            .last_eval_ts_ms = now_ms(),
        };

        if (!b_ok || !s_ok || !c_ok) {
            now.state = POWER_STATE_UNKNOWN;
        } else {
            now.state         = derive_state(&b);
            now.shore_active  = c.output_current_a   > IDLE_THRESHOLD_A;
            now.solar_active  = s.charging_current_a > IDLE_THRESHOLD_A;
            now.net_current_a = b.current_a;
            now.soc_pct       = b.soc_pct;
        }

        power_state_reading_t prev;
        portENTER_CRITICAL(&s_lock);
        prev = s_cache;
        s_cache = now;
        portEXIT_CRITICAL(&s_lock);

        bool changed = !prev.valid
                    || prev.state        != now.state
                    || prev.shore_active != now.shore_active
                    || prev.solar_active != now.solar_active;
        if (changed) {
            ESP_LOGI(TAG, "%s shore=%d solar=%d i=%+.2f SoC=%u",
                     state_name(now.state),
                     (int)now.shore_active, (int)now.solar_active,
                     now.net_current_a, now.soc_pct);
        }
    }
}

esp_err_t power_state_init(void)
{
    BaseType_t rc = xTaskCreate(power_state_task, "power_state",
                                2560, NULL, 4, NULL);
    if (rc != pdPASS) {
        ESP_LOGE(TAG, "task create failed");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t power_state_get(power_state_reading_t *out)
{
    portENTER_CRITICAL(&s_lock);
    *out = s_cache;
    portEXIT_CRITICAL(&s_lock);
    if (!out->valid) return ESP_ERR_NOT_FOUND;
    if (now_ms() - out->last_eval_ts_ms > FRESHNESS_MS) return ESP_ERR_NOT_FOUND;
    return ESP_OK;
}
