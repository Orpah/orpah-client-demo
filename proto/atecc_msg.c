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
        crc16_atca_le(out, ATECC_CMD_LEN_NOCRC, &out[5]);
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

size_t atecc_msg_sign(uint8_t *out, size_t cap, uint16_t slot, int with_crc)
{
    return cmd5(out, cap, ATECC_OP_SIGN, ATECC_SIGN_MODE_EXTERNAL, slot, with_crc);
}

/* ★ d 步：Nonce 比"无数据命令"多 **32 B 数据**（实测包 `27 16 43 00 00 <32 B 摘要> 30 1A`）。
 * count = 包总长（37 不带 CRC / 39 带 CRC）、CRC 覆盖 count..最后一个数据字节（**含数据**）✓。*/
size_t atecc_msg_nonce_load(uint8_t *out, size_t cap,
                            const uint8_t digest32[ATECC_NONCE_DIGEST_BYTES], int with_crc)
{
    size_t n = with_crc ? ATECC_CMD_LEN_NONCE_CRC : ATECC_CMD_LEN_NONCE_NOCRC;
    size_t i;

    if (out == 0 || digest32 == 0 || cap < n) {
        return 0u;
    }
    out[0] = (uint8_t)n;
    out[1] = ATECC_OP_NONCE;
    out[2] = ATECC_NONCE_MODE_MSGDIGBUF;
    out[3] = 0x00u;
    out[4] = 0x00u;
    for (i = 0u; i < ATECC_NONCE_DIGEST_BYTES; i++) {
        out[5u + i] = digest32[i];
    }
    if (with_crc) {
        crc16_atca_le(out, ATECC_CMD_LEN_NONCE_NOCRC, &out[ATECC_CMD_LEN_NONCE_NOCRC]);
    }
    return n;
}

/* ★ 2026-10-10 改正：I²C 上 count **就是 1 字节**（出处见 atecc_msg.h 的文件头）。
 *   原来按 4 字节大端读 ✗ —— 那会把器件回的 4 字节错误包 `04 FF 01 42` 算成
 *   `0x04FF0142`（天文数字）⇒ 直接判"长度离谱"，真话（状态 0xFF）反而被吃掉。*/
uint32_t atecc_msg_count(const uint8_t *resp)
{
    return (resp == 0) ? 0u : (uint32_t)resp[0];
}

int atecc_msg_err_pkt_status(const uint8_t *resp, size_t n, uint8_t *status)
{
    if (resp == 0 || status == 0 || n != ATECC_ERR_PKT_LEN) {
        return ATECC_MSG_E_LEN;
    }
    if (resp[0] != (uint8_t)ATECC_ERR_PKT_LEN) {
        return ATECC_MSG_E_LEN;         /* count 字段必须是 4 */
    }
    *status = resp[1];
    return 0;
}

const char *atecc_msg_status_name(uint8_t status)
{
    /* 措辞照 `isATCAError()` / `calib_command.h` 的原话（别自己另起说法）。*/
    switch (status) {
    case ATECC_STATUS_OK:         return "ok";
    case ATECC_STATUS_CHECKMAC:   return "checkmac or verify failed";
    case ATECC_STATUS_PARSE:      return "parse error (bad length/opcode/param)";
    case ATECC_STATUS_ECC:        return "ecc fault";
    case ATECC_STATUS_SELFTEST:   return "self test failure mode";
    case ATECC_STATUS_RNG_HEALTH: return "random number generator health test error";
    case ATECC_STATUS_EXEC:       return "cannot execute command";
    case ATECC_STATUS_WAKEUP:     return "woke up";
    case ATECC_STATUS_COMM:       return "bad crc found (command not properly received) or other comm error";
    default:                      return "unknown";
    }
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

        crc16_atca_le(resp, ATECC_RESP4_LEN_NOCRC, crc);
        if (crc[0] != resp[ATECC_RESP4_LEN_NOCRC] ||
            crc[1] != resp[ATECC_RESP4_LEN_NOCRC + 1u]) {
            return ATECC_MSG_E_CRC;
        }
    }
    for (i = 0u; i < 4u; i++) {
        out4[i] = resp[ATECC_RESP_DATA_IDX + i];
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

        crc16_atca_le(resp, ATECC_RESP_LEN_NOCRC, crc);
        if (crc[0] != resp[ATECC_RESP_LEN_NOCRC] ||
            crc[1] != resp[ATECC_RESP_LEN_NOCRC + 1u]) {
            return ATECC_MSG_E_CRC;
        }
    }
    for (i = 0u; i < ATECC_RANDOM_BYTES; i++) {
        out32[i] = resp[ATECC_RESP_DATA_IDX + i];
    }
    return 0;
}

/* ★ d 步：签名响应 = `43 | r(32) | s(32) | crc(2)`（67 B；不带 CRC 时 65）——
 *   与 `Random` 同一套规则（count 只占 1 字节 ✓），只是数据是 64 B。*/
int atecc_msg_resp_sign(const uint8_t *resp, size_t n, uint8_t out64[ATECC_SIGN_BYTES])
{
    size_t i;

    if (resp == 0 || out64 == 0) {
        return ATECC_MSG_E_ARG;
    }
    if (n != ATECC_RESP_LEN_SIG_NOCRC && n != ATECC_RESP_LEN_SIG_CRC) {
        return ATECC_MSG_E_LEN;
    }
    if (atecc_msg_count(resp) != (uint32_t)n) {
        return ATECC_MSG_E_LEN;
    }
    if (n == ATECC_RESP_LEN_SIG_CRC) {
        uint8_t crc[2];

        crc16_atca_le(resp, ATECC_RESP_LEN_SIG_NOCRC, crc);
        if (crc[0] != resp[ATECC_RESP_LEN_SIG_NOCRC] ||
            crc[1] != resp[ATECC_RESP_LEN_SIG_NOCRC + 1u]) {
            return ATECC_MSG_E_CRC;
        }
    }
    for (i = 0u; i < ATECC_SIGN_BYTES; i++) {
        out64[i] = resp[ATECC_RESP_DATA_IDX + i];
    }
    return 0;
}
