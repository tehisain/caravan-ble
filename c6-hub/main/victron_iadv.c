#include "victron_iadv.h"
#include <string.h>
// ESP-IDF v6.0 uses mbedtls 4, which removed mbedtls/aes.h from the
// public API in favor of PSA Crypto. We use ESP-IDF's esp_aes port
// instead — same API surface as the old mbedtls_aes_*, with the bonus
// of hardware acceleration on the C6.
#include "aes/esp_aes.h"

// Victron "Extra Manufacturer Data Record" header, learned by dumping
// real frames from a SmartSolar MPPT 75/15 and a Blue Smart IP65
// charger and matching mfg[7] against the first key byte:
//
//   offset 0  prefix              0x10
//   offset 1  record_kind         0x02 = SolarCharger, 0x00 = AcCharger
//                                 (observed; not yet generalized — dispatch on this)
//   offset 2-3  model_id (LE)     freeform — we don't trust it for dispatch
//   offset 4  read_state          observed 0x01
//   offset 5-6  nonce (LE)        AES-CTR initial counter low bytes
//   offset 7  first byte of key   sanity-check against key[0]
//   offset 8+  ciphertext
#define VIADV_HEADER_LEN  8

#define VIADV_KIND_BYTE_SOLAR   0x02
#define VIADV_KIND_BYTE_CHARGER 0x00

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
// AES-128-CTR with key from caller, nonce = (mfg[5], mfg[6], 14 × 0x00).
static size_t viadv_decrypt(const uint8_t *mfg, size_t len,
                            const uint8_t key[16],
                            uint8_t plain[16])
{
    if (len < VIADV_HEADER_LEN + 1) return 0;
    if (mfg[0] != 0x10) return 0;
    if (mfg[7] != key[0]) return 0;             // first-byte-of-key sanity check

    uint8_t nonce[16] = {0};
    nonce[0] = mfg[5];
    nonce[1] = mfg[6];

    size_t ct_len = len - VIADV_HEADER_LEN;
    if (ct_len > 16) ct_len = 16;
    uint8_t ct_buf[16];
    memcpy(ct_buf, mfg + VIADV_HEADER_LEN, ct_len);

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

// Victron's "no measurement available" sentinel for u16 fields.
// Observed live: when the IP65 charger is in STORAGE with no real
// current, it broadcasts 0xFF00. We treat any value with the high
// byte set (>= 0xFF00 / 6553.5 in 0.1 A units) as unavailable → 0.
static float u16_to_amps_or_zero(uint16_t raw)
{
    if (raw >= 0xFF00) return 0.0f;
    return raw / 10.0f;
}

static viadv_kind_t decode_solar(const uint8_t *p, size_t len,
                                 solar_reading_t *out)
{
    if (len < 10) return VIADV_KIND_NONE;
    memset(out, 0, sizeof(*out));
    out->charge_state         = p[0];
    out->battery_voltage_v    = rd_i16_le(p + 2) / 100.0f;
    out->charging_current_a   = u16_to_amps_or_zero(rd_u16_le(p + 4));
    out->yield_today_wh       = rd_u16_le(p + 6) * 10;
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
    out->output_current_a   = u16_to_amps_or_zero(rd_u16_le(p + 4));
    return VIADV_KIND_CHARGER;
}

viadv_kind_t viadv_decode(const uint8_t *mfg, size_t len,
                          const uint8_t key[16],
                          solar_reading_t *solar_out,
                          charger_reading_t *charger_out)
{
    if (len < VIADV_HEADER_LEN + 1) return VIADV_KIND_NONE;

    uint8_t plain[16];
    size_t plen = viadv_decrypt(mfg, len, key, plain);
    if (plen == 0) return VIADV_KIND_NONE;

    switch (mfg[1]) {
        case VIADV_KIND_BYTE_SOLAR:
            return decode_solar(plain, plen, solar_out);
        case VIADV_KIND_BYTE_CHARGER:
            return decode_charger(plain, plen, charger_out);
        default:
            return VIADV_KIND_NONE;
    }
}
