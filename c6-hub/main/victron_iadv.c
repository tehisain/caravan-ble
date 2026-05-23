#include "victron_iadv.h"
#include <string.h>
// ESP-IDF v6.0 uses mbedtls 4, which removed mbedtls/aes.h from the
// public API in favor of PSA Crypto. We use ESP-IDF's esp_aes port
// instead — same API surface as the old mbedtls_aes_*, with the bonus
// of hardware acceleration on the C6.
#include "aes/esp_aes.h"

// Public Victron model IDs we handle.
#define MODEL_SOLAR_CHARGER 0xA043
#define MODEL_AC_CHARGER    0xA248

static int hex_nibble(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
    if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
    return -1;
}

bool viadv_hex_to_key(const char *hex, uint8_t out[16])
{
    if (!hex || strlen(hex) != 32) return false;
    for (int i = 0; i < 16; i++) {
        int hi = hex_nibble(hex[i * 2]);
        int lo = hex_nibble(hex[i * 2 + 1]);
        if (hi < 0 || lo < 0) return false;
        out[i] = (uint8_t)((hi << 4) | lo);
    }
    return true;
}

static uint16_t rd_u16_le(const uint8_t *p) {
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}
static int16_t rd_i16_le(const uint8_t *p) {
    return (int16_t)rd_u16_le(p);
}

// Decrypt the ciphertext portion of a Victron manufacturer-data frame
// into `plain`. Returns plaintext length, or 0 on framing error.
//
// AES-128-CTR with:
//   key   = caller's 16-byte key
//   nonce = iv (2 bytes from frame) followed by 14 zero bytes
static size_t viadv_decrypt(const uint8_t *mfg, size_t len,
                            const uint8_t key[16],
                            uint8_t plain[16])
{
    if (len < 8) return 0;
    if (mfg[0] != 0x10) return 0;           // not an "Extra Manufacturer Data Record"
    if (mfg[6] != key[0]) return 0;         // first-byte-of-key sanity check

    uint8_t nonce[16] = {0};
    nonce[0] = mfg[4];
    nonce[1] = mfg[5];

    size_t ct_len = len - 7;
    if (ct_len > 16) ct_len = 16;
    uint8_t ct_buf[16];
    memcpy(ct_buf, mfg + 7, ct_len);

    esp_aes_context aes;
    esp_aes_init(&aes);
    if (esp_aes_setkey(&aes, key, 128) != 0) {
        esp_aes_free(&aes);
        return 0;
    }
    size_t nc_off = 0;
    uint8_t stream_block[16] = {0};
    if (esp_aes_crypt_ctr(&aes, ct_len, &nc_off, nonce, stream_block,
                          ct_buf, plain) != 0) {
        esp_aes_free(&aes);
        return 0;
    }
    esp_aes_free(&aes);
    return ct_len;
}

static viadv_kind_t decode_solar(const uint8_t *p, size_t len,
                                 solar_reading_t *out)
{
    if (len < 10) return VIADV_KIND_NONE;
    memset(out, 0, sizeof(*out));
    out->charge_state         = p[0];
    out->battery_voltage_v    = rd_i16_le(p + 2) / 100.0f;
    out->charging_current_a   = rd_i16_le(p + 4) / 10.0f;
    out->yield_today_wh       = rd_u16_le(p + 6) * 10;   // 0.01 kWh -> Wh × 10
    out->solar_power_w        = (int16_t)rd_u16_le(p + 8);
    return VIADV_KIND_SOLAR;
}

static viadv_kind_t decode_charger(const uint8_t *p, size_t len,
                                   charger_reading_t *out)
{
    if (len < 6) return VIADV_KIND_NONE;
    memset(out, 0, sizeof(*out));
    out->charge_state       = p[0];
    out->output_voltage_v   = rd_u16_le(p + 2) / 100.0f;
    out->output_current_a   = rd_u16_le(p + 4) / 10.0f;
    return VIADV_KIND_CHARGER;
}

viadv_kind_t viadv_decode(const uint8_t *mfg, size_t len,
                          const uint8_t key[16],
                          solar_reading_t *solar_out,
                          charger_reading_t *charger_out)
{
    if (len < 8) return VIADV_KIND_NONE;
    uint16_t model = rd_u16_le(mfg + 1);

    uint8_t plain[16];
    size_t plen = viadv_decrypt(mfg, len, key, plain);
    if (plen == 0) return VIADV_KIND_NONE;

    switch (model) {
        case MODEL_SOLAR_CHARGER:
            return decode_solar(plain, plen, solar_out);
        case MODEL_AC_CHARGER:
            return decode_charger(plain, plen, charger_out);
        default:
            return VIADV_KIND_NONE;
    }
}
