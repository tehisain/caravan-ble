#include "ota_handler.h"

#include <string.h>
#include <ctype.h>
#include <time.h>
#include <inttypes.h>
#include "esp_log.h"
#include "esp_app_desc.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_https_ota.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

static const char *TAG = "ota";

#define API_URL_FMT       "https://api.github.com/repos/%s/releases/latest"
#define NVS_NS            "ota"
#define NVS_KEY_LAST_TS   "last_pub_ts"

static char  s_json[6144];
static int   s_json_len;
// GitHub signed-blob URLs include a long JWT + SAS params; observed
// lengths in the 1000–1500 byte range. Size with headroom.
static char  s_redirect_url[2048];

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

static esp_err_t on_redirect_event(esp_http_client_event_t *evt)
{
    if (evt->event_id == HTTP_EVENT_ON_HEADER
        && evt->header_key && evt->header_value
        && strcasecmp(evt->header_key, "Location") == 0) {
        strncpy(s_redirect_url, evt->header_value, sizeof(s_redirect_url) - 1);
        s_redirect_url[sizeof(s_redirect_url) - 1] = 0;
    }
    return ESP_OK;
}

// Find the first occurrence of `"key":"value"` in src starting at *cursor
// (or anywhere if *cursor is NULL). On success, *out is set to point to
// the value char (just past the opening quote) and *out_len to its
// length; *cursor is advanced past the value. Returns true on success.
//
// Does not handle JSON escape sequences in the value — fine for the
// fields we care about (tag_name, name, browser_download_url,
// published_at).
static bool find_string_field(const char *src, const char **cursor,
                              const char *key,
                              const char **out, int *out_len)
{
    char needle[64];
    int klen = snprintf(needle, sizeof(needle), "\"%s\":", key);
    if (klen <= 0 || klen >= (int)sizeof(needle)) return false;

    const char *p = strstr(*cursor ? *cursor : src, needle);
    if (!p) return false;
    p += klen;
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    if (*p != '"') return false;
    p++;
    const char *start = p;
    while (*p && *p != '"') p++;
    if (*p != '"') return false;

    *out = start;
    *out_len = (int)(p - start);
    *cursor = p + 1;
    return true;
}

// Parse "YYYY-MM-DDTHH:MM:SSZ" into a unix timestamp. Returns 0 on
// failure (which propagates as "couldn't anchor on this", caller skips).
static int64_t parse_iso8601_utc(const char *s, int len)
{
    if (len < 20) return 0;
    char buf[24];
    if (len >= (int)sizeof(buf)) return 0;
    memcpy(buf, s, len);
    buf[len] = 0;

    struct tm tm = {0};
    if (sscanf(buf, "%d-%d-%dT%d:%d:%dZ",
               &tm.tm_year, &tm.tm_mon, &tm.tm_mday,
               &tm.tm_hour, &tm.tm_min, &tm.tm_sec) != 6) {
        return 0;
    }
    tm.tm_year -= 1900;
    tm.tm_mon  -= 1;
    // timegm() interprets tm as UTC. ESP-IDF provides it.
    time_t t = timegm(&tm);
    return (t == (time_t)-1) ? 0 : (int64_t)t;
}

static int64_t nvs_get_last_pub_ts(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return 0;
    int64_t ts = 0;
    nvs_get_i64(h, NVS_KEY_LAST_TS, &ts);
    nvs_close(h);
    return ts;
}

static void nvs_set_last_pub_ts(int64_t ts)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    if (nvs_set_i64(h, NVS_KEY_LAST_TS, ts) == ESP_OK) {
        nvs_commit(h);
    }
    nvs_close(h);
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

    const char *cursor = NULL;
    const char *tag;
    int tag_len;
    if (!find_string_field(s_json, &cursor, "tag_name", &tag, &tag_len)) {
        ESP_LOGE(TAG, "json missing tag_name");
        return ESP_FAIL;
    }
    char tag_buf[80];
    int n = tag_len < (int)sizeof(tag_buf) - 1 ? tag_len : (int)sizeof(tag_buf) - 1;
    memcpy(tag_buf, tag, n);
    tag_buf[n] = 0;

    // The API returns published_at as an ISO8601 UTC string, sometimes
    // earlier in the document than tag_name and sometimes later. Search
    // from the start of the JSON to avoid order assumptions.
    const char *zero_cursor = NULL;
    const char *pub;
    int pub_len;
    int64_t remote_ts = 0;
    if (find_string_field(s_json, &zero_cursor, "published_at", &pub, &pub_len)) {
        remote_ts = parse_iso8601_utc(pub, pub_len);
    }
    if (remote_ts == 0) {
        ESP_LOGW(TAG, "could not parse published_at; refusing to update without an anchor");
        return ESP_FAIL;
    }

    const esp_app_desc_t *desc = esp_app_get_description();
    char expected[80];
    snprintf(expected, sizeof(expected), "fw-%s", desc->version);
    int64_t local_ts = nvs_get_last_pub_ts();

    if (strcmp(tag_buf, expected) == 0) {
        ESP_LOGI(TAG, "already current (%s, pub_ts=%" PRId64 ")", tag_buf, remote_ts);
        // The currently-running build is what the server reports as
        // latest — anchor on its timestamp so future downgrades are
        // refused even if /latest temporarily points elsewhere.
        if (local_ts != remote_ts) {
            nvs_set_last_pub_ts(remote_ts);
            ESP_LOGI(TAG, "anchor updated: %" PRId64 " -> %" PRId64, local_ts, remote_ts);
        }
        return ESP_OK;
    }

    if (remote_ts <= local_ts) {
        ESP_LOGW(TAG, "refusing downgrade: running %s anchored %" PRId64
                     ", remote %s pub_ts=%" PRId64,
                 expected, local_ts, tag_buf, remote_ts);
        return ESP_OK;
    }

    ESP_LOGI(TAG, "update available: running %s (anchor %" PRId64
                 "), release %s (pub_ts %" PRId64 ")",
             expected, local_ts, tag_buf, remote_ts);

    // Walk the assets array looking for an entry whose "name" equals
    // CONFIG_OTA_ASSET_NAME, then capture its browser_download_url.
    char asset_url[256] = {0};
    const char *name_ptr;
    int name_len;
    while (find_string_field(s_json, &cursor, "name", &name_ptr, &name_len)) {
        bool match = (name_len == (int)strlen(CONFIG_OTA_ASSET_NAME))
                  && (memcmp(name_ptr, CONFIG_OTA_ASSET_NAME, name_len) == 0);
        if (match) {
            const char *url_ptr;
            int url_len;
            if (find_string_field(s_json, &cursor, "browser_download_url",
                                  &url_ptr, &url_len)
                && url_len < (int)sizeof(asset_url)) {
                memcpy(asset_url, url_ptr, url_len);
                asset_url[url_len] = 0;
                break;
            }
        }
    }

    if (asset_url[0] == 0) {
        ESP_LOGW(TAG, "release has no '%s' asset; aborting", CONFIG_OTA_ASSET_NAME);
        return ESP_FAIL;
    }

    // GitHub's /releases/download/* always 302-redirects to a signed
    // release-assets.githubusercontent.com URL. esp_https_ota's connect
    // path doesn't reliably handle that initial hop, so resolve the
    // redirect ourselves with esp_http_client (HEAD), capture the
    // Location header in the event callback, then hand the final URL
    // to esp_https_ota.
    s_redirect_url[0] = 0;
    esp_http_client_config_t redir_cfg = {
        .url = asset_url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .method = HTTP_METHOD_HEAD,
        .disable_auto_redirect = true,
        .event_handler = on_redirect_event,
        .timeout_ms = 10000,
        .user_agent = "c6-hub/1.0",
    };
    esp_http_client_handle_t rc = esp_http_client_init(&redir_cfg);
    err = esp_http_client_perform(rc);
    int rs = esp_http_client_get_status_code(rc);
    esp_http_client_cleanup(rc);

    if (err != ESP_OK || rs < 300 || rs >= 400) {
        ESP_LOGE(TAG, "redirect probe: err=%s status=%d",
                 esp_err_to_name(err), rs);
        return ESP_FAIL;
    }
    if (s_redirect_url[0] == 0) {
        ESP_LOGE(TAG, "%d response missing Location header", rs);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "downloading (%d bytes URL)", (int)strlen(s_redirect_url));

    esp_http_client_config_t ota_http = {
        .url = s_redirect_url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 30000,
        .keep_alive_enable = true,
        // The signed release-assets URL is ~1100–1500 bytes. Default
        // 512-byte buffers overflow on the request line.
        .buffer_size = 4096,
        .buffer_size_tx = 4096,
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
