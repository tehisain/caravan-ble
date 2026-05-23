#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "power_monitor.h"

#define PQ_BMS_REQ_LEN 8

// Fill `out` with the 8-byte GET_BATTERY_INFO request frame.
void pq_bms_build_get_battery_info(uint8_t out[PQ_BMS_REQ_LEN]);

// Validate framing (length, opcode, CRC) and decode the payload of a
// GET_BATTERY_INFO response. Returns true on success.
bool pq_bms_decode_response(const uint8_t *frame, size_t len,
                            battery_reading_t *out);
