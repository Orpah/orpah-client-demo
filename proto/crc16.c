/* crc16.c — 见 crc16.h 的出处说明。**逐行照抄** CryptoAuthLib `atCRC()` 的口径：
 *   数据位**低位先喂** + 寄存器**左移** + poly 0x8005 + 初值 0（★ 不是标准 CRC 变体 ✗）。
 * 逐位实现（不建表）：ATECC 一次只算 ~40 字节（8 MHz 上 ≈320 次循环，微秒级），
 * 建表省下的那点 CPU 远不值 512 B 的表。*/
#include "crc16.h"

uint16_t crc16_atca(const uint8_t *data, size_t len)
{
    uint16_t crc = 0u;
    size_t i;
    unsigned shift;

    if (data == 0) {
        return 0u;
    }
    for (i = 0u; i < len; i++) {
        /* ★ 与原厂一致：shift 从 0x01 递增到 0x80 ⇒ 数据位**低位先喂** */
        for (shift = 0x01u; shift <= 0x80u; shift <<= 1) {
            uint8_t data_bit = (uint8_t)((data[i] & shift) ? 1u : 0u);
            uint8_t crc_bit  = (uint8_t)((crc >> 15) & 1u);

            crc = (uint16_t)(crc << 1);          /* ★ 寄存器**左移** */
            if (data_bit != crc_bit) {
                crc ^= 0x8005u;
            }
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
