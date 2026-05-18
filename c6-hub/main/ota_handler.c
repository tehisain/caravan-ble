#include "ota_handler.h"

#include <string.h>
#include <ctype.h>
#include "esp_log.h"
#include "esp_app_desc.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_https_ota.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
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

// Find the first occurrence of `"key":"value"` in src starting at *cursor
// (or anywhere if *cursor is NULL). On success, *out is set to point to
// the value char (just past the opening quote) and *out_len to its
// length; *cursor is advanced past the value. Returns true on success.
//
// Does not handle JSON escape sequences in the value — fine for the
// fields we care about (tag_name, name, browser_download_url).
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

    const esp_app_desc_t *desc = esp_app_get_description();
    char expected[80];
    snprintf(expected, sizeof(expected), "fw-%s", desc->version);

    if (strcmp(tag_buf, expected) == 0) {
        ESP_LOGI(TAG, "already current (%s)", tag_buf);
        return ESP_OK;
    }
    ESP_LOGI(TAG, "update available: running %s, release %s", expected, tag_buf);

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
    // redirect ourselves with esp_http_client (HEAD), then hand the
    // final URL to esp_https_ota.
    static char final_url[512];
    esp_http_client_config_t redir_cfg = {
        .url = asset_url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .method = HTTP_METHOD_HEAD,
        .disable_auto_redirect = true,
        .timeout_ms = 10000,
        .user_agent = "c6-hub/1.0",
    };
    esp_http_client_handle_t rc = esp_http_client_init(&redir_cfg);
    err = esp_http_client_perform(rc);
    int rs = esp_http_client_get_status_code(rc);
    final_url[0] = 0;
    if (err == ESP_OK && (rs == 301 || rs == 302 || rs == 303 || rs == 307 || rs == 308)) {
        char *loc = NULL;
        if (esp_http_client_get_header(rc, "Location", &loc) == ESP_OK && loc) {
            strncpy(final_url, loc, sizeof(final_url) - 1);
        }
    } else {
        ESP_LOGE(TAG, "redirect probe failed: err=%s status=%d",
                 esp_err_to_name(err), rs);
    }
    esp_http_client_cleanup(rc);
    if (final_url[0] == 0) {
        ESP_LOGE(TAG, "could not resolve redirect from %s", asset_url);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "downloading (resolved) %s", final_url);

    esp_http_client_config_t ota_http = {
        .url = final_url,
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
