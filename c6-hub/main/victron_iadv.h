#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "power_monitor.h"

typedef enum {
    VIADV_KIND_NONE,        // bad frame, or device type we don't decode
    VIADV_KIND_SOLAR,       // output written into solar_out
    VIADV_KIND_CHARGER,     // output written into charger_out
} viadv_kind_t;

// Decode a manufacturer-data payload (the bytes that followed
// manufacturer-data ID 0x02E1, i.e. starting at the 0x10 prefix byte).
// `key` is the 16-byte AES key (already hex-decoded). Outputs are
// only written when the return value is the matching kind.
viadv_kind_t viadv_decode(const uint8_t *mfg_data, size_t len,
                          const uint8_t key[16],
                          solar_reading_t *solar_out,
                          charger_reading_t *charger_out);

// Convert a 32-char hex key string from Kconfig into 16 bytes.
// Returns false on bad input.
bool viadv_hex_to_key(const char *hex, uint8_t out[16]);
