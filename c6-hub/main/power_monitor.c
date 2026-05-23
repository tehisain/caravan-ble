#include "power_monitor.h"

#include <string.h>
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "sdkconfig.h"

#include "freertos/semphr.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/ble_gap.h"
#include "host/ble_gatt.h"

#include "victron_iadv.h"
#include "powerqueen_bms.h"

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

// BMS GATT-cycle state. Updated by GAP/GATT events on the NimBLE host
// task; bms_poll_task waits on s_bms_done between cycles.
#define PQ_SVC_UUID16  0xFFE0
#define PQ_CHR_UUID16  0xFFE1

static SemaphoreHandle_t s_bms_done;
static uint16_t          s_bms_conn_handle = 0xFFFF;
static uint16_t          s_bms_chr_val_handle;
static bool              s_bms_ok;

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

// Forward declarations for the BMS GATT-discovery callbacks.
static int bms_on_disc_svc(uint16_t conn_handle, const struct ble_gatt_error *error,
                           const struct ble_gatt_svc *svc, void *arg);
static int bms_on_disc_chr(uint16_t conn_handle, const struct ble_gatt_error *error,
                           const struct ble_gatt_chr *chr, void *arg);
static int bms_on_subscribe(uint16_t conn_handle, const struct ble_gatt_error *error,
                            struct ble_gatt_attr *attr, void *arg);

static void bms_finish(bool ok)
{
    s_bms_ok = ok;
    if (s_bms_conn_handle != 0xFFFF) {
        ble_gap_terminate(s_bms_conn_handle, BLE_ERR_REM_USER_CONN_TERM);
    }
    xSemaphoreGive(s_bms_done);
}

static int bms_on_disc_svc(uint16_t conn_handle, const struct ble_gatt_error *error,
                           const struct ble_gatt_svc *svc, void *arg)
{
    if (error->status == BLE_HS_EDONE) return 0;
    if (error->status != 0 || svc == NULL) {
        ESP_LOGW(TAG, "bms svc disc err=%d", error->status);
        bms_finish(false);
        return 0;
    }
    ble_uuid16_t chr_uuid = BLE_UUID16_INIT(PQ_CHR_UUID16);
    int rc = ble_gattc_disc_chrs_by_uuid(conn_handle,
                                         svc->start_handle, svc->end_handle,
                                         &chr_uuid.u, bms_on_disc_chr, NULL);
    if (rc != 0) {
        ESP_LOGW(TAG, "bms disc_chrs rc=%d", rc);
        bms_finish(false);
    }
    return 0;
}

static int bms_on_disc_chr(uint16_t conn_handle, const struct ble_gatt_error *error,
                           const struct ble_gatt_chr *chr, void *arg)
{
    if (error->status == BLE_HS_EDONE) return 0;
    if (error->status != 0 || chr == NULL) {
        ESP_LOGW(TAG, "bms chr disc err=%d", error->status);
        bms_finish(false);
        return 0;
    }
    s_bms_chr_val_handle = chr->val_handle;
    // The CCC descriptor sits at val_handle+1 by convention. Write 0x0001
    // to enable notifications.
    uint8_t notify_on[2] = {0x01, 0x00};
    int rc = ble_gattc_write_flat(conn_handle, chr->val_handle + 1,
                                  notify_on, sizeof(notify_on),
                                  bms_on_subscribe, NULL);
    if (rc != 0) {
        ESP_LOGW(TAG, "bms ccc write rc=%d", rc);
        bms_finish(false);
    }
    return 0;
}

static int bms_on_subscribe(uint16_t conn_handle, const struct ble_gatt_error *error,
                            struct ble_gatt_attr *attr, void *arg)
{
    if (error->status != 0) {
        ESP_LOGW(TAG, "bms subscribe err=%d", error->status);
        bms_finish(false);
        return 0;
    }
    uint8_t req[PQ_BMS_REQ_LEN];
    pq_bms_build_get_battery_info(req);
    int rc = ble_gattc_write_flat(conn_handle, s_bms_chr_val_handle,
                                  req, sizeof(req), NULL, NULL);
    if (rc != 0) {
        ESP_LOGW(TAG, "bms write_flat rc=%d", rc);
        bms_finish(false);
    }
    return 0;
}

static int gap_event_cb(struct ble_gap_event *event, void *arg)
{
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            s_bms_conn_handle = event->connect.conn_handle;
            ble_uuid16_t svc_uuid = BLE_UUID16_INIT(PQ_SVC_UUID16);
            int rc = ble_gattc_disc_svc_by_uuid(s_bms_conn_handle, &svc_uuid.u,
                                                bms_on_disc_svc, NULL);
            if (rc != 0) {
                ESP_LOGW(TAG, "bms disc_svc rc=%d", rc);
                bms_finish(false);
            }
        } else {
            ESP_LOGW(TAG, "bms connect failed: %d", event->connect.status);
            bms_finish(false);
        }
        return 0;

    case BLE_GAP_EVENT_DISCONNECT:
        s_bms_conn_handle = 0xFFFF;
        return 0;

    case BLE_GAP_EVENT_NOTIFY_RX: {
        const struct os_mbuf *om = event->notify_rx.om;
        size_t total = OS_MBUF_PKTLEN(om);
        uint8_t resp[160];
        if (total > sizeof(resp)) total = sizeof(resp);
        if (os_mbuf_copydata(om, 0, total, resp) != 0) return 0;

        battery_reading_t b;
        if (pq_bms_decode_response(resp, total, &b)) {
            b.valid = true;
            b.last_seen_ts_ms = now_ms();
            portENTER_CRITICAL(&s_lock);
            bool changed = !s_battery.valid || s_battery.state != b.state;
            s_battery = b;
            portEXIT_CRITICAL(&s_lock);
            if (changed) {
                ESP_LOGI(TAG, "bms v=%.3f i=%.3f SoC=%u SoH=%u state=%u",
                         b.terminal_voltage_v, b.current_a,
                         b.soc_pct, b.soh_pct, b.state);
            }
            bms_finish(true);
        } else {
            ESP_LOGW(TAG, "bms frame parse failed (%u bytes)", (unsigned)total);
            bms_finish(false);
        }
        return 0;
    }

    case BLE_GAP_EVENT_DISC: {
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
    default:
        return 0;
    }
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

static void resume_scan(void)
{
    uint8_t own_addr_type;
    if (ble_hs_id_infer_auto(0, &own_addr_type) != 0) return;
    struct ble_gap_disc_params dp = {
        .passive = 1,
        .filter_duplicates = 0,
    };
    ble_gap_disc(own_addr_type, BLE_HS_FOREVER, &dp, gap_event_cb, NULL);
}

static void bms_poll_task(void *arg)
{
    // Wait a few seconds after boot so Wi-Fi + OTA + the first Victron
    // sample have all settled.
    vTaskDelay(pdMS_TO_TICKS(5000));

    ble_addr_t target = {.type = BLE_ADDR_PUBLIC};
    for (int i = 0; i < 6; i++) {
        target.val[5 - i] = s_bms_mac[i];   // NimBLE wants LSB-first
    }

    for (;;) {
        // Pause the discovery scan; ble_gap_connect can race with scan
        // on some controllers.
        ble_gap_disc_cancel();
        vTaskDelay(pdMS_TO_TICKS(50));

        s_bms_ok = false;
        s_bms_conn_handle = 0xFFFF;
        xSemaphoreTake(s_bms_done, 0);    // drain any stale token

        int rc = ble_gap_connect(BLE_OWN_ADDR_PUBLIC, &target,
                                 5000 /*ms*/, NULL, gap_event_cb, NULL);
        if (rc != 0) {
            ESP_LOGW(TAG, "bms connect kick rc=%d", rc);
        } else {
            if (xSemaphoreTake(s_bms_done, pdMS_TO_TICKS(6000)) != pdTRUE) {
                ESP_LOGW(TAG, "bms cycle timed out");
                if (s_bms_conn_handle != 0xFFFF) {
                    ble_gap_terminate(s_bms_conn_handle,
                                      BLE_ERR_REM_USER_CONN_TERM);
                }
            }
        }

        resume_scan();
        vTaskDelay(pdMS_TO_TICKS(CONFIG_POWER_BMS_POLL_INTERVAL_MS));
    }
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

    s_bms_done = xSemaphoreCreateBinary();
    if (!s_bms_done) {
        ESP_LOGE(TAG, "could not create bms semaphore");
        return ESP_ERR_NO_MEM;
    }
    xTaskCreate(bms_poll_task, "bms_poll", 6144, NULL, 5, NULL);
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
