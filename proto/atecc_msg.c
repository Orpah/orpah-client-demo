/* atecc_msg.c — 见 atecc_msg.h 的出处说明。纯函数、无 stdio/malloc。*/
#include "atecc_msg.h"

#include "crc16.h"

size_t atecc_msg_random(uint8_t *out, size_t cap, uint8_t mode, int with_crc)
{
    size_t n = with_crc ? ATECC_CMD_LEN_CRC : ATECC_CMD_LEN_NOCRC;

    if (out == 0 || cap < n) {
        return 0u;
    }
    out[0] = (uint8_t)n;                 /* count：含 count 自身与 CRC */
    out[1] = ATECC_OP_RANDOM;
    out[2] = mode;                       /* param1 */
    out[3] = 0x00u;                      /* param2 高 */
    out[4] = 0x00u;                      /* param2 低 */
    if (with_crc) {
        /* CRC 覆盖 count..param2（即前 5 字节），不含 CRC 自身 */
        crc16_bypass_le(out, ATECC_CMD_LEN_NOCRC, &out[5]);
    }
    return n;
}

uint32_t atecc_msg_count(const uint8_t c4[4])
{
    if (c4 == 0) {
        return 0u;
    }
    return ((uint32_t)c4[0] << 24) | ((uint32_t)c4[1] << 16) |
           ((uint32_t)c4[2] << 8) | (uint32_t)c4[3];
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
