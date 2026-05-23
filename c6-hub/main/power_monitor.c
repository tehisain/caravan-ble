#include "power_monitor.h"

#include <string.h>
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "sdkconfig.h"

#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/ble_gap.h"

#include "victron_iadv.h"

static const char *TAG = "pm";

#define VICTRON_MFG_ID 0x02E1

// Module-private cache + spinlock.
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static battery_reading_t s_battery;
static solar_reading_t   s_solar;
static charger_reading_t s_charger;

static uint8_t s_solar_mac[6];
static uint8_t s_solar_key[16];
static bool    s_solar_enabled;

static uint8_t s_ip65_mac[6];
static uint8_t s_ip65_key[16];
static bool    s_ip65_enabled;

static uint8_t s_bms_mac[6];

// Parse "AA:BB:CC:DD:EE:FF" into 6 bytes (MSB-first). Returns false on bad input.
static bool parse_mac(const char *s, uint8_t out[6])
{
    if (!s || strlen(s) != 17) return false;
    for (int i = 0; i < 6; i++) {
        char a = s[i * 3], b = s[i * 3 + 1];
        int hi = (a >= '0' && a <= '9') ? a - '0'
               : (a >= 'a' && a <= 'f') ? 10 + a - 'a'
               : (a >= 'A' && a <= 'F') ? 10 + a - 'A' : -1;
        int lo = (b >= '0' && b <= '9') ? b - '0'
               : (b >= 'a' && b <= 'f') ? 10 + b - 'a'
               : (b >= 'A' && b <= 'F') ? 10 + b - 'A' : -1;
        if (hi < 0 || lo < 0) return false;
        out[i] = (uint8_t)((hi << 4) | lo);
        if (i < 5 && s[i * 3 + 2] != ':') return false;
    }
    return true;
}

// NimBLE ble_addr_t stores the MAC LSB-first; our parsed s_*_mac is MSB-first.
static bool mac_match(const uint8_t addr_lsb_first[6], const uint8_t mac_msb_first[6])
{
    for (int i = 0; i < 6; i++) {
        if (addr_lsb_first[5 - i] != mac_msb_first[i]) return false;
    }
    return true;
}

// Find manufacturer-data field with the given company ID inside a raw
// BLE advertising payload. Sets *out to point inside the payload (just
// past the 2-byte company ID) and *out_len to remaining bytes.
static bool find_mfg_data(const uint8_t *adv, size_t adv_len, uint16_t company_id,
                          const uint8_t **out, size_t *out_len)
{
    size_t i = 0;
    while (i + 1 < adv_len) {
        uint8_t fld_len = adv[i];
        if (fld_len == 0) return false;
        if (i + 1 + fld_len > adv_len) return false;
        uint8_t fld_type = adv[i + 1];
        if (fld_type == 0xFF && fld_len >= 3) {
            uint16_t cid = (uint16_t)adv[i + 2] | ((uint16_t)adv[i + 3] << 8);
            if (cid == company_id) {
                *out = adv + i + 4;
                *out_len = fld_len - 3;
                return true;
            }
        }
        i += 1 + fld_len;
    }
    return false;
}

static int64_t now_ms(void)
{
    return esp_timer_get_time() / 1000;
}

static void log_solar_if_changed(const solar_reading_t *prev, const solar_reading_t *now)
{
    if (!prev->valid || prev->charge_state != now->charge_state) {
        ESP_LOGI(TAG, "solar v=%.2f i=%.1f W=%d state=%u",
                 now->battery_voltage_v, now->charging_current_a,
                 (int)now->solar_power_w, now->charge_state);
    }
}

static void log_charger_if_changed(const charger_reading_t *prev, const charger_reading_t *now)
{
    if (!prev->valid || prev->charge_state != now->charge_state) {
        ESP_LOGI(TAG, "charger v=%.2f i=%.1f state=%u",
                 now->output_voltage_v, now->output_current_a, now->charge_state);
    }
}

static int gap_event_cb(struct ble_gap_event *event, void *arg)
{
    if (event->type != BLE_GAP_EVENT_DISC) return 0;

    const uint8_t *addr = event->disc.addr.val;
    const uint8_t *data = event->disc.data;
    size_t data_len     = event->disc.length_data;

    bool is_solar = s_solar_enabled && mac_match(addr, s_solar_mac);
    bool is_ip65  = s_ip65_enabled  && mac_match(addr, s_ip65_mac);
    if (!is_solar && !is_ip65) return 0;

    const uint8_t *mfg;
    size_t mfg_len;
    if (!find_mfg_data(data, data_len, VICTRON_MFG_ID, &mfg, &mfg_len)) {
        return 0;
    }

    solar_reading_t tmp_solar = {0};
    charger_reading_t tmp_charger = {0};
    const uint8_t *key = is_solar ? s_solar_key : s_ip65_key;
    viadv_kind_t kind = viadv_decode(mfg, mfg_len, key, &tmp_solar, &tmp_charger);

    if (kind == VIADV_KIND_SOLAR && is_solar) {
        tmp_solar.valid = true;
        tmp_solar.last_seen_ts_ms = now_ms();
        portENTER_CRITICAL(&s_lock);
        solar_reading_t prev = s_solar;
        s_solar = tmp_solar;
        portEXIT_CRITICAL(&s_lock);
        log_solar_if_changed(&prev, &tmp_solar);
    } else if (kind == VIADV_KIND_CHARGER && is_ip65) {
        tmp_charger.valid = true;
        tmp_charger.last_seen_ts_ms = now_ms();
        portENTER_CRITICAL(&s_lock);
        charger_reading_t prev = s_charger;
        s_charger = tmp_charger;
        portEXIT_CRITICAL(&s_lock);
        log_charger_if_changed(&prev, &tmp_charger);
    }
    return 0;
}

static void on_ble_sync(void)
{
    uint8_t own_addr_type;
    int rc = ble_hs_id_infer_auto(0, &own_addr_type);
    if (rc != 0) {
        ESP_LOGE(TAG, "infer addr: %d", rc);
        return;
    }

    struct ble_gap_disc_params dp = {
        .itvl = 0,
        .window = 0,
        .filter_policy = 0,
        .limited = 0,
        .passive = 1,
        // Duplicate filtering OFF — Victron payloads change every ~1 s
        // and we must observe each unique payload.
        .filter_duplicates = 0,
    };
    rc = ble_gap_disc(own_addr_type, BLE_HS_FOREVER, &dp, gap_event_cb, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "scan start: %d", rc);
    } else {
        ESP_LOGI(TAG, "scan started; solar=%d ip65=%d",
                 (int)s_solar_enabled, (int)s_ip65_enabled);
    }
}

static void ble_host_task(void *param)
{
    nimble_port_run();
    nimble_port_freertos_deinit();
}

esp_err_t power_monitor_init(void)
{
    s_solar_enabled = parse_mac(CONFIG_POWER_VICTRON_SOLAR_MAC, s_solar_mac)
                   && viadv_hex_to_key(CONFIG_POWER_VICTRON_SOLAR_KEY, s_solar_key)
                   && strlen(CONFIG_POWER_VICTRON_SOLAR_KEY) > 0;
    s_ip65_enabled  = parse_mac(CONFIG_POWER_VICTRON_IP65_MAC,  s_ip65_mac)
                   && viadv_hex_to_key(CONFIG_POWER_VICTRON_IP65_KEY,  s_ip65_key)
                   && strlen(CONFIG_POWER_VICTRON_IP65_KEY) > 0;
    if (!s_solar_enabled) ESP_LOGW(TAG, "solar disabled (bad mac or empty key)");
    if (!s_ip65_enabled)  ESP_LOGW(TAG, "ip65 disabled (bad mac or empty key)");

    if (!parse_mac(CONFIG_POWER_BMS_MAC, s_bms_mac)) {
        ESP_LOGW(TAG, "bms mac unparseable");
    }

    esp_err_t err = nimble_port_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nimble_port_init: %d", err);
        return err;
    }
    ble_hs_cfg.sync_cb = on_ble_sync;
    nimble_port_freertos_init(ble_host_task);
    return ESP_OK;
}

esp_err_t power_monitor_get_battery(battery_reading_t *out)
{
    portENTER_CRITICAL(&s_lock);
    *out = s_battery;
    portEXIT_CRITICAL(&s_lock);
    if (!out->valid) return ESP_ERR_NOT_FOUND;
    if (now_ms() - out->last_seen_ts_ms > 60000) return ESP_ERR_NOT_FOUND;
    return ESP_OK;
}

esp_err_t power_monitor_get_solar(solar_reading_t *out)
{
    portENTER_CRITICAL(&s_lock);
    *out = s_solar;
    portEXIT_CRITICAL(&s_lock);
    if (!out->valid) return ESP_ERR_NOT_FOUND;
    if (now_ms() - out->last_seen_ts_ms > 10000) return ESP_ERR_NOT_FOUND;
    return ESP_OK;
}

esp_err_t power_monitor_get_charger(charger_reading_t *out)
{
    portENTER_CRITICAL(&s_lock);
    *out = s_charger;
    portEXIT_CRITICAL(&s_lock);
    if (!out->valid) return ESP_ERR_NOT_FOUND;
    if (now_ms() - out->last_seen_ts_ms > 10000) return ESP_ERR_NOT_FOUND;
    return ESP_OK;
}
