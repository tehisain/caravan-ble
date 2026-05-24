#include "alerts.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "power_monitor.h"
#include "power_state.h"

static const char *TAG = "al";

// Hysteresis thresholds.
#define LOW_SOC_ENTER             20
#define LOW_SOC_EXIT              25
#define CRITICAL_SOC_ENTER        10
#define CRITICAL_SOC_EXIT         15
#define OVER_TEMP_CELL_ENTER      50
#define OVER_TEMP_CELL_EXIT       45
#define UNDER_TEMP_CHARGE_ENTER   0
#define UNDER_TEMP_CHARGE_EXIT    5

#define EVAL_INTERVAL_MS          1000

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static alert_state_t s_state[ALERT_COUNT];

static const char *s_names[ALERT_COUNT] = {
    [ALERT_LOW_SOC]            = "LOW_SOC",
    [ALERT_CRITICAL_SOC]       = "CRITICAL_SOC",
    [ALERT_OVER_TEMP_CELL]     = "OVER_TEMP_CELL",
    [ALERT_UNDER_TEMP_CHARGE]  = "UNDER_TEMP_CHARGE",
};

static int64_t now_ms(void)
{
    return esp_timer_get_time() / 1000;
}

static bool enter_predicate(alert_id_t id,
                            const battery_reading_t *b,
                            const power_state_reading_t *ps)
{
    switch (id) {
        case ALERT_LOW_SOC:
            return b->soc_pct < LOW_SOC_ENTER;
        case ALERT_CRITICAL_SOC:
            return b->soc_pct < CRITICAL_SOC_ENTER;
        case ALERT_OVER_TEMP_CELL:
            return b->cell_temp_c > OVER_TEMP_CELL_ENTER;
        case ALERT_UNDER_TEMP_CHARGE:
            return b->cell_temp_c < UNDER_TEMP_CHARGE_ENTER
                && ps->state == POWER_STATE_CHARGING;
        case ALERT_COUNT:
            return false;
    }
    return false;
}

static bool exit_predicate(alert_id_t id,
                           const battery_reading_t *b,
                           const power_state_reading_t *ps)
{
    (void)ps;
    switch (id) {
        case ALERT_LOW_SOC:
            return b->soc_pct > LOW_SOC_EXIT;
        case ALERT_CRITICAL_SOC:
            return b->soc_pct > CRITICAL_SOC_EXIT;
        case ALERT_OVER_TEMP_CELL:
            return b->cell_temp_c < OVER_TEMP_CELL_EXIT;
        case ALERT_UNDER_TEMP_CHARGE:
            return b->cell_temp_c > UNDER_TEMP_CHARGE_EXIT;
        case ALERT_COUNT:
            return false;
    }
    return false;
}

static void log_transition(const char *verb, alert_id_t id,
                           const battery_reading_t *b)
{
    if (id == ALERT_OVER_TEMP_CELL || id == ALERT_UNDER_TEMP_CHARGE) {
        ESP_LOGI(TAG, "%-5s %-17s T=%d", verb, s_names[id], b->cell_temp_c);
    } else {
        ESP_LOGI(TAG, "%-5s %-17s SoC=%u", verb, s_names[id], b->soc_pct);
    }
}

static void raise_alert(alert_id_t id, const battery_reading_t *b)
{
    int64_t t = now_ms();
    portENTER_CRITICAL(&s_lock);
    s_state[id].active = true;
    s_state[id].first_active_ts_ms = t;
    s_state[id].last_change_ts_ms = t;
    portEXIT_CRITICAL(&s_lock);
    log_transition("ENTER", id, b);
}

static void clear_alert(alert_id_t id, const battery_reading_t *b)
{
    int64_t t = now_ms();
    portENTER_CRITICAL(&s_lock);
    s_state[id].active = false;
    s_state[id].last_change_ts_ms = t;
    portEXIT_CRITICAL(&s_lock);
    log_transition("CLEAR", id, b);
}

static void alerts_task(void *arg)
{
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(EVAL_INTERVAL_MS));

        battery_reading_t b;
        power_state_reading_t ps;
        if (power_monitor_get_battery(&b) != ESP_OK) continue;
        if (power_state_get(&ps) != ESP_OK) continue;

        for (int id = 0; id < ALERT_COUNT; id++) {
            bool was_active;
            portENTER_CRITICAL(&s_lock);
            was_active = s_state[id].active;
            portEXIT_CRITICAL(&s_lock);

            if (was_active) {
                if (exit_predicate(id, &b, &ps)) clear_alert(id, &b);
            } else {
                if (enter_predicate(id, &b, &ps)) raise_alert(id, &b);
            }
        }
    }
}

esp_err_t alerts_init(void)
{
    portENTER_CRITICAL(&s_lock);
    for (int i = 0; i < ALERT_COUNT; i++) {
        s_state[i] = (alert_state_t){0};
    }
    portEXIT_CRITICAL(&s_lock);

    BaseType_t rc = xTaskCreate(alerts_task, "alerts", 2560, NULL, 4, NULL);
    if (rc != pdPASS) {
        ESP_LOGE(TAG, "task create failed");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t alerts_get(alert_id_t id, alert_state_t *out)
{
    if (id >= ALERT_COUNT) return ESP_ERR_INVALID_ARG;
    portENTER_CRITICAL(&s_lock);
    *out = s_state[id];
    portEXIT_CRITICAL(&s_lock);
    return ESP_OK;
}

const char *alerts_name(alert_id_t id)
{
    if (id >= ALERT_COUNT) return "?";
    return s_names[id];
}
