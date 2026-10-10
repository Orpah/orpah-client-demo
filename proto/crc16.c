/* crc16.c — 见 crc16.h 的出处说明。
 *
 * ★ 实现说明（**本仓自己的写法**）：ATECC 那个 CRC 等价于
 *     「把每个字节**按位倒过来**，再喂进**教科书式 MSB-first 的 CRC-16**（poly 0x8005、初值 0）」
 *   —— 因为“数据位低位先喂 + 寄存器左移”与“先倒位、再按高位移出”**逐位等价**（证明见 crc16.h）。
 *   两者给出同一个 16 位余数，但**代码是本仓自己写的** ✓（出处只引“口径/行为”，不搬代码 ✗）。
 * 逐位实现（不建表）：一次只算 ~40 字节（8 MHz 上 ≈320 次循环，微秒级），不值 512 B 的表。
 */
#include "crc16.h"

/* 字节按位倒序（0b1000_0001 -> 0b1000_0001，0b1110_0000 -> 0b0000_0111）*/
static uint8_t rev8(uint8_t b)
{
    uint8_t r = 0u;
    int i;

    for (i = 0; i < 8; i++) {
        r = (uint8_t)((r << 1) | (b & 1u));
        b = (uint8_t)(b >> 1);
    }
    return r;
}

uint16_t crc16_atca(const uint8_t *data, size_t len)
{
    uint16_t crc = 0u;
    size_t i;
    int bit;

    if (data == 0) {
        return 0u;
    }
    for (i = 0u; i < len; i++) {
        crc ^= (uint16_t)((uint16_t)rev8(data[i]) << 8);
        for (bit = 0; bit < 8; bit++) {
            crc = (uint16_t)((crc & 0x8000u) ? (uint16_t)((crc << 1) ^ 0x8005u)
                                             : (uint16_t)(crc << 1));
        }
    }
    return crc;
}

void crc16_atca_le(const uint8_t *data, size_t len, uint8_t out_le[2])
{
    uint16_t crc = crc16_atca(data, len);

    if (out_le == 0) {
        return;
    }
    out_le[0] = (uint8_t)(crc & 0xFFu);
    out_le[1] = (uint8_t)((crc >> 8) & 0xFFu);
}
