/* atecc_msg.c — 见 atecc_msg.h 的出处说明。纯函数、无 stdio/malloc。*/
#include "atecc_msg.h"

#include "crc16.h"

/* 一条"无数据命令"的包：count + opcode + param1 + param2(2 B) + [CRC]。
 * Random / Info / Read 全是一个形状（区别只在 opcode 与 param）⇒ 只写一份。*/
static size_t cmd5(uint8_t *out, size_t cap, uint8_t op, uint8_t p1, uint16_t p2, int with_crc)
{
    size_t n = with_crc ? ATECC_CMD_LEN_CRC : ATECC_CMD_LEN_NOCRC;

    if (out == 0 || cap < n) {
        return 0u;
    }
    out[0] = (uint8_t)n;                 /* count：含 count 自身与 CRC */
    out[1] = op;
    out[2] = p1;                         /* param1 */
    out[3] = (uint8_t)(p2 >> 8);         /* param2 高 */
    out[4] = (uint8_t)(p2 & 0xFFu);      /* param2 低 */
    if (with_crc) {
        /* CRC 覆盖 count..param2（即前 5 字节），不含 CRC 自身 */
        crc16_bypass_le(out, ATECC_CMD_LEN_NOCRC, &out[5]);
    }
    return n;
}

size_t atecc_msg_random(uint8_t *out, size_t cap, uint8_t mode, int with_crc)
{
    return cmd5(out, cap, ATECC_OP_RANDOM, mode, 0x0000u, with_crc);
}

size_t atecc_msg_info(uint8_t *out, size_t cap, uint8_t mode, int with_crc)
{
    return cmd5(out, cap, ATECC_OP_INFO, mode, 0x0000u, with_crc);
}

size_t atecc_msg_read(uint8_t *out, size_t cap, uint8_t zone, uint16_t addr, int with_crc)
{
    return cmd5(out, cap, ATECC_OP_READ, zone, addr, with_crc);
}

uint32_t atecc_msg_count(const uint8_t c4[4])
{
    if (c4 == 0) {
        return 0u;
    }
    return ((uint32_t)c4[0] << 24) | ((uint32_t)c4[1] << 16) |
           ((uint32_t)c4[2] << 8) | (uint32_t)c4[3];
}

int atecc_msg_resp_get4(const uint8_t *resp, size_t n, uint8_t out4[4])
{
    size_t i;

    if (resp == 0 || out4 == 0) {
        return ATECC_MSG_E_ARG;
    }
    if (n != ATECC_RESP4_LEN_NOCRC && n != ATECC_RESP4_LEN_CRC) {
        return ATECC_MSG_E_LEN;
    }
    if (atecc_msg_count(resp) != (uint32_t)n) {
        return ATECC_MSG_E_LEN;          /* count 与实际长度必须一致 */
    }
    if (n == ATECC_RESP4_LEN_CRC) {
        uint8_t crc[2];

        crc16_bypass_le(resp, ATECC_RESP4_LEN_NOCRC, crc);
        if (crc[0] != resp[ATECC_RESP4_LEN_NOCRC] ||
            crc[1] != resp[ATECC_RESP4_LEN_NOCRC + 1u]) {
            return ATECC_MSG_E_CRC;
        }
    }
    for (i = 0u; i < 4u; i++) {
        out4[i] = resp[4u + i];
    }
    return 0;
}

int atecc_msg_resp_random(const uint8_t *resp, size_t n, uint8_t out32[ATECC_RANDOM_BYTES])
{
    size_t i;

    if (resp == 0 || out32 == 0) {
        return ATECC_MSG_E_ARG;
    }
    if (n != ATECC_RESP_LEN_NOCRC && n != ATECC_RESP_LEN_CRC) {
        return ATECC_MSG_E_LEN;
    }
    if (atecc_msg_count(resp) != (uint32_t)n) {
        return ATECC_MSG_E_LEN;          /* count 与实际长度必须一致 */
    }
    if (n == ATECC_RESP_LEN_CRC) {
        uint8_t crc[2];

        crc16_bypass_le(resp, ATECC_RESP_LEN_NOCRC, crc);
        if (crc[0] != resp[ATECC_RESP_LEN_NOCRC] ||
            crc[1] != resp[ATECC_RESP_LEN_NOCRC + 1u]) {
            return ATECC_MSG_E_CRC;
        }
    }
    for (i = 0u; i < ATECC_RANDOM_BYTES; i++) {
        out32[i] = resp[4u + i];
    }
    return 0;
}
