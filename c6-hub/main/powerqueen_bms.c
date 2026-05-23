#include "powerqueen_bms.h"
#include <string.h>

// Request frame: 00 00 04 01 <opcode> 55 AA <crc>
// opcode 0x13 = GET_BATTERY_INFO
// CRC = sum(preceding bytes) & 0xFF

static uint8_t crc8(const uint8_t *data, size_t len)
{
    uint8_t sum = 0;
    for (size_t i = 0; i < len; i++) sum += data[i];
    return sum;
}

void pq_bms_build_get_battery_info(uint8_t out[PQ_BMS_REQ_LEN])
{
    out[0] = 0x00;
    out[1] = 0x00;
    out[2] = 0x04;
    out[3] = 0x01;
    out[4] = 0x13;
    out[5] = 0x55;
    out[6] = 0xAA;
    out[7] = crc8(out, 7);
}

static uint16_t rd_u16_le(const uint8_t *p) {
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static int16_t rd_i16_le(const uint8_t *p) {
    return (int16_t)rd_u16_le(p);
}

static uint32_t rd_u32_le(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static int32_t rd_i32_le(const uint8_t *p) {
    return (int32_t)rd_u32_le(p);
}

bool pq_bms_decode_response(const uint8_t *frame, size_t len,
                            battery_reading_t *out)
{
    if (len < 105) return false;
    if (frame[3] != 0x01) return false;
    // Response opcode = request opcode | 0x80, so 0x13 | 0x80 = 0x93
    if (frame[4] != 0x93) return false;
    if (crc8(frame, len - 1) != frame[len - 1]) return false;

    memset(out, 0, sizeof(*out));
    out->pack_voltage_v     = rd_u32_le(frame + 8)  / 1000.0f;
    out->terminal_voltage_v = rd_u32_le(frame + 12) / 1000.0f;
    out->current_a          = rd_i32_le(frame + 48) / 1000.0f;
    out->cell_temp_c        = (int8_t)rd_i16_le(frame + 52);
    out->mosfet_temp_c      = (int8_t)rd_i16_le(frame + 54);
    out->remaining_ah       = rd_u16_le(frame + 62) / 100.0f;
    out->capacity_ah        = rd_u16_le(frame + 64) / 100.0f;
    out->state              = (uint8_t)rd_u16_le(frame + 88);
    out->soc_pct            = (uint8_t)rd_u16_le(frame + 90);
    out->soh_pct            = (uint8_t)rd_u32_le(frame + 92);
    out->cycles             = rd_u32_le(frame + 96);

    uint8_t cell_count = 0;
    for (int i = 0; i < 16; i++) {
        uint16_t v = rd_u16_le(frame + 16 + i * 2);
        out->cell_mv[i] = v;
        if (v) cell_count++;
    }
    out->cell_count = cell_count;
    return true;
}
