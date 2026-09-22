/* crc16.c — 见 crc16.h 的出处说明。逐位实现（不建表）：ATECC 一次只算 ~40 字节，
 * 建表省下的那点 CPU（8 MHz 上 40 字节 ≈ 320 次循环，微秒级）远不值 512 B 的表。*/
#include "crc16.h"

uint16_t crc16_bypass(const uint8_t *data, size_t len)
{
    uint16_t crc = 0u;
    size_t i;
    int bit;

    if (data == 0) {
        return 0u;
    }
    for (i = 0u; i < len; i++) {
        for (bit = 7; bit >= 0; bit--) {
            uint8_t data_bit = (uint8_t)((data[i] >> bit) & 1u);
            uint8_t crc_bit  = (uint8_t)((crc >> 15) & 1u);

            crc = (uint16_t)(crc << 1);
            if (data_bit != crc_bit) {
                crc ^= 0x8005u;
            }
        }
    }
    return crc;
}

void crc16_bypass_le(const uint8_t *data, size_t len, uint8_t out_le[2])
{
    uint16_t crc = crc16_bypass(data, len);

    if (out_le == 0) {
        return;
    }
    out_le[0] = (uint8_t)(crc & 0xFFu);
    out_le[1] = (uint8_t)((crc >> 8) & 0xFFu);
}
