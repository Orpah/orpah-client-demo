/* atecc_msg.c — 见 atecc_msg.h 的出处说明。纯函数、无 stdio/malloc。*/
#include "atecc_msg.h"

#include "crc16.h"

/* 一条命令包的**唯一一份**构造（★ 2026-10-11 抽出来）：
 *   count(1) + opcode(1) + param1(1) + param2(2, **小端**) + [数据 dn B] + [CRC(2, 小端)]
 * count = **包总长**（含 count 自身与 CRC）；CRC 覆盖 count..最后一个数据字节、**不含 CRC 自身**。
 * `dn == 0` 时就是原来的 `cmd5()`（Random / Info / Read / Sign / GenKey / Lock ✓）。
 * ★★ 2026-10-11 **实测纠正**：param2 是**小端**（低字节在前）✗ —— 原厂栈读配置区第 1 块
 *   实发 `07 02 80 08 00 0A 4D`（`Read` 的字地址 8 ⇒ `08 00`）✓；此前验过的包里 param2
 *   恰好全是 0，所以大端/小端看不出区别 ⇒ 一直没暴露 ✗（一上机读非零地址就报 `0x03` parse）。*/
static size_t cmd_data(uint8_t *out, size_t cap, uint8_t op, uint8_t p1,
                       uint16_t p2, const uint8_t *data, size_t dn, int with_crc)
{
    size_t n = 1u + 4u + dn + (with_crc ? 2u : 0u);
    size_t i;

    if (out == 0 || cap < n || n > 255u) {
        return 0u;                       /* count 只占 1 字节 ⇒ 包不能超过 255 */
    }
    if (data == 0 && dn != 0u) {
        return 0u;
    }
    out[0] = (uint8_t)n;
    out[1] = op;
    out[2] = p1;                         /* param1 */
    out[3] = (uint8_t)(p2 & 0xFFu);      /* param2 低字节（**小端**）*/
    out[4] = (uint8_t)(p2 >> 8);         /* param2 高字节 */
    for (i = 0u; i < dn; i++) {
        out[5u + i] = data[i];
    }
    if (with_crc) {
        crc16_atca_le(out, n - 2u, &out[n - 2u]);
    }
    return n;
}

static size_t cmd5(uint8_t *out, size_t cap, uint8_t op, uint8_t p1, uint16_t p2, int with_crc)
{
    return cmd_data(out, cap, op, p1, p2, 0, 0u, with_crc);
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
 * count = 包总长（37 不带 CRC / 39 带 CRC）、CRC 覆盖 count..最后一个数据字节（**含数据**）✓。
 * ★ 2026-10-11：改成调 `cmd_data()`（同一条规则只有一份实现 ✓）——字节应与改前**完全相同**，
 *   由交叉测试的 29 行向量行守（✓ 实测：改后逐行一致）。*/
size_t atecc_msg_nonce_load(uint8_t *out, size_t cap,
                            const uint8_t digest32[ATECC_NONCE_DIGEST_BYTES], int with_crc)
{
    return cmd_data(out, cap, ATECC_OP_NONCE, ATECC_NONCE_MODE_MSGDIGBUF, 0x0000u,
                    digest32, ATECC_NONCE_DIGEST_BYTES, with_crc);
}

/* ★★ d 步：`SHA(0x47)` 三连（HMAC）—— 见 `atecc_msg.h` 那一段的锚点与出处。
 * ⚠ 三条都走 `cmd_data()` ⇒ 包形状只有一份实现 ✓。*/
size_t atecc_msg_sha_start(uint8_t *out, size_t cap, uint16_t key_slot, int with_crc)
{
    return cmd_data(out, cap, ATECC_OP_SHA, ATECC_SHA_MODE_HMAC_START, key_slot, 0, 0u, with_crc);
}

size_t atecc_msg_sha_update(uint8_t *out, size_t cap,
                            const uint8_t *blk, size_t n, int with_crc)
{
    if (n > ATECC_SHA_BLOCK) {
        return 0u;                       /* 器件一次只吃 64 B ⇒ 超过就是调用方该分块 ✗ */
    }
    /* ★★ `param2` = **本块字节数**（喂满块时 = 64）—— 出处 = **原厂源码** ✓，不是猜 ✗：
     *   `cryptoauthlib/lib/calib/calib_sha.c` 的 `calib_sha_hmac_update()` 里
     *   `calib_sha_base(device, SHA_MODE_HMAC_UPDATE, ATCA_SHA256_BLOCK_SIZE, ctx->block, …)`
     *   ⇒ `param2 = 64` ✓；而 `calib_sha_base()` 是 `packet->param2 = length` ✓，
     *   `atSHA()` 又把 `txsize = ATCA_CMD_SIZE_MIN + param2` ✓ ⇒ **`count` 与 `param2` 必须自洽**。
     * ★ 2026-10-11 我曾把这里填 0 ✗（依据是“同一条 100 B 消息 p2=0 逐字节相同”）——
     *   那条实测**被后来推翻** ✓：真正的故障在**多块**时（第二块起被拒），而根因是
     *   `count` 与 `param2` 不自洽的写法在**尾块/多块**路径上暴露出来 ✗。
     *   长消息（≥160 B，需要 ≥2 条 UPDATE）当时一直失败、两种主机都一样 ✗。
     * ⚠ `START` 的 `param2` = key slot ✓、`END` 的 `param2` = **尾块字节数**（可为 0 ✓）
     *   —— 三者**各按各的**，别“顺手统一” ✗。*/
    return cmd_data(out, cap, ATECC_OP_SHA, ATECC_SHA_MODE_HMAC_UPDATE,
                    (uint16_t)ATECC_SHA_BLOCK, blk, n, with_crc);
}

size_t atecc_msg_sha_hmac_end(uint8_t *out, size_t cap,
                              const uint8_t *tail, size_t n, int with_crc)
{
    if (n > ATECC_SHA_BLOCK) {
        return 0u;
    }
    /* param1 = END | 落点（TempKey ✓）；param2 = 尾块字节数 ✓ */
    return cmd_data(out, cap, ATECC_OP_SHA,
                    (uint8_t)(ATECC_SHA_MODE_608_HMAC_END | ATECC_SHA_TARGET_TEMPKEY),
                    (uint16_t)n, tail, n, with_crc);
}

/* ★ d 步：HMAC 摘要响应的形状与 `Random` **完全一样**（33/35 B、count 1 字节、CRC 规则同）
 *   ⇒ 直接转发给那一份实现 ✓（别再拄一份长度/CRC 校验 ✗ —— 抄一份就多一个错处）。*/
int atecc_msg_resp_sha(const uint8_t *resp, size_t n, uint8_t out32[ATECC_SHA_DIGEST_BYTES])
{
    return atecc_msg_resp_random(resp, n, out32);
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
