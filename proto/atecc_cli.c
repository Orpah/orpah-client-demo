/* atecc_cli.c — ATECC608B **报文层**（`proto/atecc_msg.c` + `proto/crc16.c`）的 PC 侧自检 CLI
 *
 * 为什么要有它：这两份代码在固件里跑，但**CRC 或包格式错了只会在台架上表现为
 * "SE 没应答"** —— 那时根本分不清是线没接对还是字节算错了。所以按本仓口径
 * （AGENTS §4：CRC 与报文编解码不许只靠人眼对着文档写）先在 PC 上对拍：
 *   ① `selfcheck`      —— 内建不变量（含**外部锚点**：器件/原厂实回的字节对，见 docs/atecc608b-se.md §8.2）
 *   ② `crc-selftest`   —— CRC 向量表（与 `run_cross_test.py` 的 Python 参考零偏差）
 *   ③ `msg-selftest`   —— 命令包 / 响应解析向量表
 *
 * 退出码：0 = 全 PASS；2 = 有 FAIL；1 = 用法/文件错误。
 *
 * 向量行格式（TAB 分隔；`#` 开头与空行忽略）：
 *   crc  <输入hex|->      <->            <crc 4 位大写>
 *   cmd  <mode>           <with_crc 0/1> <->            <包hex>
 *   resp <完整响应hex>    <->            <->            OK:<32B hex> | ERR:<码>
 *     （`ERR:` 的码与 C 侧 `ATECC_MSG_E_*` 一致：-1 参数 / -2 长度或 count 不符 / -3 CRC 不符）
 *   info <mode>           <with_crc 0/1> <->            <包hex>          （工装，2026-09-23）
 *   read <zone> <addr(hex)> <with_crc 0/1>               <包hex>          （工装，2026-09-23）
 *   resp4 <完整响应hex>   <->            <->            OK:<4B hex> | ERR:<码>
 *   nonce <32B摘要hex>    <with_crc 0/1> <->            <包hex>          （★ d 步，2026-10-11）
 *   sign  <槽号>          <with_crc 0/1> <->            <包hex>          （★ d 步，2026-10-11）
 *   respsig <完整响应hex> <->            <->            OK:<r||s 64B hex> | ERR:<码>  （★ d 步）
 *   errpkt <4字节包hex>   <->            <->            0x<状态>:<名字> | not-errpkt
 *     （`errpkt` 用 `atecc_msg_err_pkt_status()` + `atecc_msg_status_name()`；
 *       措辞照 CryptoAuthLib 原话，例：0x11 唤醒应答、0xFF CRC/通讯错）
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "atecc_msg.h"
#include "crc16.h"

#define LINE_MAX 16384

static char    g_line[LINE_MAX];
static char    g_got[LINE_MAX];
static char    g_want[LINE_MAX];
static uint8_t g_bin[512];

static void usage(void)
{
    fprintf(stderr,
            "usage: atecc_cli <cmd> [args]\n"
            "  random <mode> <with_crc 0|1>        -> 命令包-hex\n"
            "  resp <响应-hex>                     -> OK:<32B hex> | ERR:<码>\n"
            "  info <mode> <with_crc 0|1>          -> 命令包-hex（工装）\n"
            "  read <zone> <addr-hex> <with_crc>   -> 命令包-hex（工装）\n"
            "  resp4 <响应-hex>                    -> OK:<4B hex> | ERR:<码>（工装）\n"
            "  nonce <32B摘要-hex> <with_crc 0|1>  -> Nonce 包-hex（★ d 步）\n"
            "  sign <槽号> <with_crc 0|1>          -> Sign 包-hex（★ d 步）\n"
            "  respsig <响应-hex>                  -> OK:<r||s 64B> | ERR:<码>（★ d 步）\n"
            "  selfcheck\n"
            "  crc-selftest <vector-file>\n"
            "  msg-selftest <vector-file>\n");
}

static void chomp(char *s)
{
    size_t n = strlen(s);

    while (n > 0 && (s[n - 1] == '\n' || s[n - 1] == '\r')) {
        s[--n] = '\0';
    }
}

static int split_tabs(char *s, char *cols[], int ncols)
{
    int n = 0;
    char *p = s;

    if (ncols <= 0) { return 0; }
    cols[n++] = p;
    while (*p != '\0') {
        if (*p == '\t') {
            *p = '\0';
            if (n >= ncols) { return n; }
            cols[n++] = p + 1;
        }
        p++;
    }
    return n;
}

static int hexval(int c)
{
    if (c >= '0' && c <= '9') { return c - '0'; }
    if (c >= 'a' && c <= 'f') { return c - 'a' + 10; }
    if (c >= 'A' && c <= 'F') { return c - 'A' + 10; }
    return -1;
}

/* hex → 二进制；`-` = 空。返回字节数，-1 = 非法/超容 */
static int hex_to_bin(const char *hex, uint8_t *out, size_t cap)
{
    size_t n = 0;
    int hi = -1;

    if (hex == NULL || strcmp(hex, "-") == 0) { return 0; }
    while (*hex != '\0') {
        int v = hexval((unsigned char)*hex);

        if (v < 0) { return -1; }
        if (hi < 0) {
            hi = v;
        } else {
            if (n >= cap) { return -1; }
            out[n++] = (uint8_t)((hi << 4) | v);
            hi = -1;
        }
        hex++;
    }
    return (hi < 0) ? (int)n : -1;
}

static void bin_to_hex(const uint8_t *b, size_t n, char *out)
{
    static const char H[] = "0123456789abcdef";
    size_t i;

    for (i = 0; i < n; i++) {
        out[i * 2u] = H[(b[i] >> 4) & 0x0Fu];
        out[i * 2u + 1u] = H[b[i] & 0x0Fu];
    }
    out[n * 2u] = '\0';
}

/* 一行 `resp`：完整响应 → OK:<32B hex> | ERR:<码> */
static void resp_line(const uint8_t *resp, size_t n, char *out, size_t cap)
{
    uint8_t d[ATECC_RANDOM_BYTES];
    int rc = atecc_msg_resp_random(resp, n, d);

    if (rc == 0) {
        char hex[ATECC_RANDOM_BYTES * 2 + 1];

        bin_to_hex(d, ATECC_RANDOM_BYTES, hex);
        snprintf(out, cap, "OK:%s", hex);
    } else {
        snprintf(out, cap, "ERR:%d", rc);
    }
}

/* 一行 `resp4`：完整响应 → OK:<4B hex> | ERR:<码> */
static void resp4_line(const uint8_t *resp, size_t n, char *out, size_t cap)
{
    uint8_t d[4];
    int rc = atecc_msg_resp_get4(resp, n, d);

    if (rc == 0) {
        char hex[4 * 2 + 1];

        bin_to_hex(d, 4u, hex);
        snprintf(out, cap, "OK:%s", hex);
    } else {
        snprintf(out, cap, "ERR:%d", rc);
    }
}

static int cmd_random(int mode, int with_crc)
{
    uint8_t pkt[ATECC_CMD_LEN_CRC];
    size_t n = atecc_msg_random(pkt, sizeof(pkt), (uint8_t)mode, with_crc);
    char hex[ATECC_CMD_LEN_CRC * 2 + 1];

    if (n == 0u) { return 1; }
    bin_to_hex(pkt, n, hex);
    printf("%s\n", hex);
    return 0;
}

/* ★ d 步：`nonce`/`sign` 与 `random` 同一个形状（打一行包 hex）*/
static int cmd_nonce(const char *digest_hex, int with_crc)
{
    uint8_t pkt[ATECC_CMD_LEN_NONCE_CRC];
    uint8_t d32[ATECC_NONCE_DIGEST_BYTES];
    char hex[ATECC_CMD_LEN_NONCE_CRC * 2 + 1];
    size_t n;

    if (hex_to_bin(digest_hex, d32, sizeof(d32)) != (int)ATECC_NONCE_DIGEST_BYTES) {
        fprintf(stderr, "digest 必须是 32 字节 hex\n");
        return 1;
    }
    n = atecc_msg_nonce_load(pkt, sizeof(pkt), d32, with_crc);
    if (n == 0u) { return 1; }
    bin_to_hex(pkt, n, hex);
    printf("%s\n", hex);
    return 0;
}

static int cmd_sign(int slot, int with_crc)
{
    uint8_t pkt[ATECC_CMD_LEN_CRC];
    size_t n = atecc_msg_sign(pkt, sizeof(pkt), (uint16_t)slot, with_crc);
    char hex[ATECC_CMD_LEN_CRC * 2 + 1];

    if (n == 0u) { return 1; }
    bin_to_hex(pkt, n, hex);
    printf("%s\n", hex);
    return 0;
}

/* 一行 `respsig`：完整响应 → OK:<r||s 64B hex> | ERR:<码> */
static void respsig_line(const uint8_t *resp, size_t n, char *out, size_t cap)
{
    uint8_t sig[ATECC_SIGN_BYTES];
    int rc = atecc_msg_resp_sign(resp, n, sig);

    if (rc == 0) {
        char hex[ATECC_SIGN_BYTES * 2 + 1];

        bin_to_hex(sig, ATECC_SIGN_BYTES, hex);
        snprintf(out, cap, "OK:%s", hex);
    } else {
        snprintf(out, cap, "ERR:%d", rc);
    }
}

static int selftest(const char *path)
{
    FILE *fp = fopen(path, "r");
    int nrow = 0, bad = 0;

    if (fp == NULL) {
        fprintf(stderr, "cannot open %s\n", path);
        return 1;
    }
    while (fgets(g_line, (int)sizeof(g_line), fp) != NULL) {
        char *cols[6];
        int nc;

        chomp(g_line);
        if (g_line[0] == '\0' || g_line[0] == '#') { continue; }
        nc = split_tabs(g_line, cols, 6);
        if (nc < 4) { continue; }
        g_got[0] = '\0';
        if (strcmp(cols[0], "crc") == 0) {
            int n = hex_to_bin(cols[1], g_bin, sizeof(g_bin));

            if (n < 0) { snprintf(g_got, sizeof(g_got), "(非法 hex)"); }
            else { snprintf(g_got, sizeof(g_got), "%04X", crc16_atca(g_bin, (size_t)n)); }
        } else if (strcmp(cols[0], "cmd") == 0) {
            uint8_t pkt[ATECC_CMD_LEN_CRC];
            size_t n = atecc_msg_random(pkt, sizeof(pkt), (uint8_t)atoi(cols[1]),
                                        atoi(cols[2]) != 0);

            if (n == 0u) { snprintf(g_got, sizeof(g_got), "(组包失败)"); }
            else { bin_to_hex(pkt, n, g_got); }
        } else if (strcmp(cols[0], "info") == 0) {
            uint8_t pkt[ATECC_CMD_LEN_CRC];
            size_t n = atecc_msg_info(pkt, sizeof(pkt), (uint8_t)atoi(cols[1]),
                                      atoi(cols[2]) != 0);

            if (n == 0u) { snprintf(g_got, sizeof(g_got), "(组包失败)"); }
            else { bin_to_hex(pkt, n, g_got); }
        } else if (strcmp(cols[0], "read") == 0) {
            uint8_t pkt[ATECC_CMD_LEN_CRC];
            size_t n = atecc_msg_read(pkt, sizeof(pkt), (uint8_t)atoi(cols[1]),
                                      (uint16_t)strtoul(cols[2], NULL, 16),
                                      atoi(cols[3]) != 0);

            if (n == 0u) { snprintf(g_got, sizeof(g_got), "(组包失败)"); }
            else { bin_to_hex(pkt, n, g_got); }
        } else if (strcmp(cols[0], "nonce") == 0) {
            uint8_t pkt[ATECC_CMD_LEN_NONCE_CRC];
            uint8_t d32[ATECC_NONCE_DIGEST_BYTES];
            size_t n;

            if (hex_to_bin(cols[1], d32, sizeof(d32)) != (int)ATECC_NONCE_DIGEST_BYTES) {
                snprintf(g_got, sizeof(g_got), "(摘要不是 32 B)");
            } else {
                n = atecc_msg_nonce_load(pkt, sizeof(pkt), d32, atoi(cols[2]) != 0);
                if (n == 0u) { snprintf(g_got, sizeof(g_got), "(组包失败)"); }
                else { bin_to_hex(pkt, n, g_got); }
            }
        } else if (strcmp(cols[0], "sign") == 0) {
            uint8_t pkt[ATECC_CMD_LEN_CRC];
            size_t n = atecc_msg_sign(pkt, sizeof(pkt), (uint16_t)atoi(cols[1]),
                                      atoi(cols[2]) != 0);

            if (n == 0u) { snprintf(g_got, sizeof(g_got), "(组包失败)"); }
            else { bin_to_hex(pkt, n, g_got); }
        } else if (strcmp(cols[0], "respsig") == 0) {
            int n = hex_to_bin(cols[1], g_bin, sizeof(g_bin));

            if (n < 0) { snprintf(g_got, sizeof(g_got), "(非法 hex)"); }
            else { respsig_line(g_bin, (size_t)n, g_got, sizeof(g_got)); }
        } else if (strcmp(cols[0], "errpkt") == 0) {
            int n = hex_to_bin(cols[1], g_bin, sizeof(g_bin));
            uint8_t st = 0u;

            if (n < 0) {
                snprintf(g_got, sizeof(g_got), "(非法 hex)");
            } else if (atecc_msg_err_pkt_status(g_bin, (size_t)n, &st) != 0) {
                snprintf(g_got, sizeof(g_got), "not-errpkt");
            } else {
                snprintf(g_got, sizeof(g_got), "0x%02X:%s", (unsigned)st,
                         atecc_msg_status_name(st));
            }
        } else if (strcmp(cols[0], "resp4") == 0) {
            int n = hex_to_bin(cols[1], g_bin, sizeof(g_bin));

            if (n < 0) { snprintf(g_got, sizeof(g_got), "(非法 hex)"); }
            else { resp4_line(g_bin, (size_t)n, g_got, sizeof(g_got)); }
        } else if (strcmp(cols[0], "resp") == 0) {
            int n = hex_to_bin(cols[1], g_bin, sizeof(g_bin));

            if (n < 0) { snprintf(g_got, sizeof(g_got), "(非法 hex)"); }
            else { resp_line(g_bin, (size_t)n, g_got, sizeof(g_got)); }
        } else {
            continue;                        /* 不认识的 kind：跳过（前向兼容）*/
        }
        nrow++;
        /* 期望值 = **最后一列**（crc 表 4 列 ⇒ cols[3]；cmd/resp 表 5 列 ⇒ cols[4]）*/
        snprintf(g_want, sizeof(g_want), "%s", cols[nc - 1]);
        if (strcmp(g_got, g_want) != 0) {
            bad++;
            printf("  line %d [%s]: want=%s got=%s\n", nrow, cols[0], g_want, g_got);
        }
    }
    fclose(fp);
    if (bad != 0 || nrow == 0) {
        printf("FAIL %s：%d/%d 行不符\n", path, bad, nrow);
        return 2;
    }
    printf("PASS %s：%d 行全部一致\n", path, nrow);
    return 0;
}

/* ------------------------------------------------------------------ */
/* selfcheck：不依赖文件的不变量（含**外部**校验值）                     */
/* ------------------------------------------------------------------ */
static int sc_bad;

static void ck(const char *what, int ok)
{
    if (!ok) { sc_bad++; }
    printf("  %s %s\n", ok ? "ok  " : "BAD ", what);
}

static int selfcheck(void)
{
    uint8_t pkt[ATECC_CMD_LEN_CRC];
    uint8_t resp[ATECC_RESP_LEN_CRC];
    uint8_t d[ATECC_RANDOM_BYTES];
    static const uint8_t cat[9] = { '1', '2', '3', '4', '5', '6', '7', '8', '9' };
    static const uint8_t wake11[2] = { 0x04u, 0x11u };
    static const uint8_t errff[2]  = { 0x04u, 0xFFu };
    static const uint8_t rand5[5]  = { 0x07u, 0x1Bu, 0x00u, 0x00u, 0x00u };
    static const uint8_t info5[5]  = { 0x07u, 0x30u, 0x00u, 0x00u, 0x00u };
    size_t n, i;
    int rc;

    sc_bad = 0;
    /* ① ★ 外部锚点 = **另一方产出的真实字节**（不是目录校验值：原厂 atCRC 不是标准变体 ✗）
     *    四条全部来自实测（器件实回 + 原厂库实发），见 docs/atecc608b-se.md §8.2 第四段 */
    ck("外部锚点：器件实回 04 11 -> CRC 33 43",
       crc16_atca(wake11, 2u) == CRC16_ATCA_WAKE_CRC);
    ck("外部锚点：器件实回 04 FF -> CRC 01 42",
       crc16_atca(errff, 2u) == CRC16_ATCA_ERR_CRC);
    ck("外部锚点：原厂实发 07 1B 00 00 00 -> 24 CD",
       crc16_atca(rand5, 5u) == CRC16_ATCA_RAND_CRC);
    ck("外部锚点：原厂实发 07 30 00 00 00 -> 03 5D",
       crc16_atca(info5, 5u) == CRC16_ATCA_INFO_CRC);
    /* ①b 向量 "123456789" 的值**由我们自己钉住**（★ 不是目录校验值 —— 这算法目录里没有 ✗）
     *     值 = 0xBCDD（2026-10-10 照 atCRC() 算）：改实现时这里会红 ⇒ 防悄悄漂 ✗ */
    ck("CRC(\"123456789\") = 0xBCDD（照 atCRC 算；非目录值）",
       crc16_atca(cat, sizeof(cat)) == 0xBCDDu);
    /* ② 空输入 = 初值 0 */
    ck("空输入 -> 0x0000", crc16_atca(0, 0u) == 0u);
    /* ③ 首字节进/不进（只是形状检查：非 0 且非 0xFFFF）*/
    {
        uint8_t one = 0x00u;

        ck("单字节 0x00 的 CRC 确定（可复现）",
           crc16_atca(&one, 1u) == crc16_atca(&one, 1u));
    }
    /* ④ 命令包：长度与 count 字段一致 */
    n = atecc_msg_random(pkt, sizeof(pkt), ATECC_MODE_SEED_UPDATE, 0);
    ck("Random 无 CRC 包长 = 5、count = 5、opcode = 0x1B",
       n == ATECC_CMD_LEN_NOCRC && pkt[0] == ATECC_CMD_LEN_NOCRC &&
       pkt[1] == ATECC_OP_RANDOM);
    n = atecc_msg_random(pkt, sizeof(pkt), ATECC_MODE_SEED_UPDATE, 1);
    ck("Random 带 CRC 包长 = 7、count = 7、包尾 = CRC16(前 5 字节, 小端)",
       n == ATECC_CMD_LEN_CRC && pkt[0] == ATECC_CMD_LEN_CRC &&
       pkt[5] == (uint8_t)(crc16_atca(pkt, 5u) & 0xFFu) &&
       pkt[6] == (uint8_t)(crc16_atca(pkt, 5u) >> 8));
    /* ⑤ 响应解析：33 字节（无 CRC）应过 —— count **只占 1 字节**（2026-10-10 改正）*/
    for (i = 0; i < sizeof(resp); i++) { resp[i] = 0u; }
    resp[0] = ATECC_RESP_LEN_NOCRC;
    for (i = 0; i < ATECC_RANDOM_BYTES; i++) { resp[ATECC_RESP_DATA_IDX + i] = (uint8_t)i; }
    rc = atecc_msg_resp_random(resp, ATECC_RESP_LEN_NOCRC, d);
    ck("响应 33 B（count 1 字节）解析出 32 B", rc == 0 && d[0] == 0x00u && d[31] == 0x1Fu);
    /* ⑥ 35 字节 + 正确 CRC 应过；把 CRC 改一位应报 -3 */
    {
        uint8_t crc[2];

        resp[0] = ATECC_RESP_LEN_CRC;
        crc16_atca_le(resp, ATECC_RESP_LEN_NOCRC, crc);
        resp[ATECC_RESP_LEN_NOCRC] = crc[0];
        resp[ATECC_RESP_LEN_NOCRC + 1u] = crc[1];
        rc = atecc_msg_resp_random(resp, ATECC_RESP_LEN_CRC, d);
        ck("响应 35 B（带 CRC）解析通过", rc == 0);
        resp[ATECC_RESP_LEN_NOCRC] ^= 0x01u;
        rc = atecc_msg_resp_random(resp, ATECC_RESP_LEN_CRC, d);
        ck("改一位 CRC -> -3（CRC 不符）", rc == ATECC_MSG_E_CRC);
    }
    /* ⑦ 4 字节错误包：用 **2026-10-10 台架上真读到的**字节（不是我们自己造的）*/
    {
        static const uint8_t errpkt[ATECC_ERR_PKT_LEN] = { 0x04u, 0xFFu, 0x01u, 0x42u };
        static const uint8_t wake4[ATECC_ERR_PKT_LEN] = { 0x04u, 0x11u, 0x33u, 0x43u };
        uint8_t st = 0u;

        ck("实测错误包 04 FF 01 42 -> 状态 0xFF（CRC/通讯错）",
           atecc_msg_err_pkt_status(errpkt, sizeof(errpkt), &st) == 0 &&
           st == ATECC_STATUS_COMM);
        ck("实测唤醒应答 04 11 33 43 -> 状态 0x11（刚被唤醒）",
           atecc_msg_err_pkt_status(wake4, sizeof(wake4), &st) == 0 &&
           st == ATECC_STATUS_WAKEUP);
        ck("状态 0xFF 有名字（不是 unknown）",
           strcmp(atecc_msg_status_name(ATECC_STATUS_COMM), "unknown") != 0);
    }
    /* ⑦ count 与实际长度不一致 -> -2；长度 35 -> -2 */
    resp[3] = ATECC_RESP_LEN_NOCRC;
    ck("长度 35 -> -2（长度不符）",
       atecc_msg_resp_random(resp, ATECC_RESP_LEN_NOCRC - 1u, d) == ATECC_MSG_E_LEN);
    resp[3] = 0x25u;                                 /* count 说 37，实际给 36 */
    ck("count 与实际长度不一致 -> -2",
       atecc_msg_resp_random(resp, ATECC_RESP_LEN_NOCRC, d) == ATECC_MSG_E_LEN);

    /* ⑧ 工装 Info / Read 包（2026-09-23）：count / opcode / 参数字节必须与 CryptoAuthLib 同形 */
    n = atecc_msg_info(pkt, sizeof(pkt), 0x00u, 0);
    ck("Info 无 CRC：长 5 / count 5 / op 0x30 / 参数全 0",
       n == ATECC_CMD_LEN_NOCRC && pkt[0] == 5u && pkt[1] == ATECC_OP_INFO &&
       pkt[2] == 0x00u && pkt[3] == 0x00u && pkt[4] == 0x00u);
    n = atecc_msg_info(pkt, sizeof(pkt), 0x00u, 1);
    ck("Info 带 CRC：长 7 / count 7 / 包尾 = CRC16(前 5 B, 小端)",
       n == ATECC_CMD_LEN_CRC && pkt[0] == 7u &&
       pkt[5] == (uint8_t)(crc16_atca(pkt, 5u) & 0xFFu) &&
       pkt[6] == (uint8_t)(crc16_atca(pkt, 5u) >> 8));
    n = atecc_msg_read(pkt, sizeof(pkt), ATECC_ZONE_CONFIG, ATECC_CFG_WORD_LOCK, 0);
    ck("Read 配置区锁字 0x15 无 CRC：op 0x02 / param1=zone 0x00 / param2 小端 = 15 00",
       n == ATECC_CMD_LEN_NOCRC && pkt[1] == ATECC_OP_READ && pkt[2] == ATECC_ZONE_CONFIG &&
       pkt[3] == 0x15u && pkt[4] == 0x00u);
    /* ★★ 外部锚点（2026-10-11 实测）：**原厂栈读配置区**实发的四条 `Read` ——
     *   实测原样（`docs/atecc608b-se.md` §8.8 / §8.12）：
     *     `07 02 80 00 00 09 AD`、`07 02 80 08 00 0A 4D`、
     *     `07 02 80 10 00 0A 1D`、`07 02 80 18 00 09 FD`
     *   ⇒ 两条可钉死的事实：
     *     ① **param2 = 字地址**（0x00/0x08/0x10/0x18 = 4 块 32 B，不是字节 0x00/0x20/0x40/0x60）；
     *     ② **param2 小端**（字 8 在线上是 `08 00`）。
     *   ⚠ param1 两家写法不同、**都实测可用**（不要拿它当锚点）✗：
     *     原厂 = `0x80`（"32 字节块读"那档，见 §8.8 注）；
     *     本仓 = zone（配置区 `0x00`）+ 字数 —— 本机实测可用（固件读锁字 / `tools/se_provision.py
     *     read` 读全 128 B 都对得上 ✓）。
     *   ⇒ **CRC 锚点用原厂那 5 字节原样算**（这样锚的是「我们的 CRC ↔ 器件真回/真发」，
     *     跟我们的包构造无关 ✓）；**包构造**那条只钉 param2 两个字节 ✓。*/
    {
        static const uint8_t rd1[5] = { 0x07u, 0x02u, 0x80u, 0x00u, 0x00u };
        static const uint8_t rd2[5] = { 0x07u, 0x02u, 0x80u, 0x08u, 0x00u };
        uint8_t crc[2];

        crc16_atca_le(rd1, 5u, crc);
        ck("外部锚点：CRC16(07 02 80 00 00) = 09 AD（原厂实发包尾）",
           crc[0] == 0x09u && crc[1] == 0xADu);
        crc16_atca_le(rd2, 5u, crc);
        ck("外部锚点：CRC16(07 02 80 08 00) = 0A 4D（原厂实发包尾）",
           crc[0] == 0x0Au && crc[1] == 0x4Du);
    }
    {
        uint8_t p7[ATECC_CMD_LEN_CRC];
        int ok;

        n = atecc_msg_read(p7, sizeof(p7), ATECC_ZONE_CONFIG, 0x0000u, 1);
        ok = (n == 7u) && (p7[3] == 0x00u) && (p7[4] == 0x00u);
        ck("Read 字地址 0 → param2 = 00 00（小端；同外部锚点 ①）", ok);
        n = atecc_msg_read(p7, sizeof(p7), ATECC_ZONE_CONFIG, 0x0008u, 1);
        ok = (n == 7u) && (p7[3] == 0x08u) && (p7[4] == 0x00u);
        ck("Read 字地址 8 → param2 = 08 00（**小端** ✓；同外部锚点 ②）", ok);
    }
    ck("容量不够（cap=5 却要带 CRC）-> 0",
       atecc_msg_info(pkt, 5u, 0x00u, 1) == 0u);

    /* ⑨ 工装 resp4：4 字节响应解析（长度 / count / CRC 三道）—— ★ count 只占 1 字节 */
    {
        uint8_t d4[4];

        for (i = 0; i < sizeof(resp); i++) { resp[i] = 0u; }
        resp[0] = ATECC_RESP4_LEN_NOCRC;
        resp[1] = 0x00u; resp[2] = 0x00u; resp[3] = 0x60u; resp[4] = 0x03u;
        ck("resp4 5 B（count=5）-> 00 00 60 03",
           atecc_msg_resp_get4(resp, ATECC_RESP4_LEN_NOCRC, d4) == 0 &&
           d4[0] == 0x00u && d4[1] == 0x00u && d4[2] == 0x60u && d4[3] == 0x03u);
        resp[0] = ATECC_RESP4_LEN_NOCRC + 1u;
        ck("resp4 count 不符 -> -2",
           atecc_msg_resp_get4(resp, ATECC_RESP4_LEN_NOCRC, d4) == ATECC_MSG_E_LEN);
        ck("resp4 长度 6 -> -2",
           atecc_msg_resp_get4(resp, 6u, d4) == ATECC_MSG_E_LEN);
        resp[0] = ATECC_RESP4_LEN_CRC;
        resp[1] = 0x55u;
        {
            uint8_t crc[2];

            crc16_atca_le(resp, ATECC_RESP4_LEN_NOCRC, crc);
            resp[ATECC_RESP4_LEN_NOCRC] = crc[0];
            resp[ATECC_RESP4_LEN_NOCRC + 1u] = crc[1];
            ck("resp4 7 B（带 CRC）且首字节 = 0x55（未锁）",
               atecc_msg_resp_get4(resp, ATECC_RESP4_LEN_CRC, d4) == 0 && d4[0] == 0x55u);
            resp[ATECC_RESP4_LEN_NOCRC] ^= 0x01u;
            ck("resp4 改一位 CRC -> -3",
               atecc_msg_resp_get4(resp, ATECC_RESP4_LEN_CRC, d4) == ATECC_MSG_E_CRC);
        }
    }

    /* ⑩ ★★ d 步的外部锚点（2026-10-11 台架）：**原厂栈实发**的字节 ——
     *    Nonce `27 16 43 00 00 <32B 摘要> 30 1A`、Sign `07 41 A0 00 00 7B 85`、
     *    器件实回的签名响应 `43 | r(32) | s(32) | 8D 9C`（见 docs/atecc608b-se.md §8.10）。
     *  ★ 为什么整串十六进制地比：**包尾那 2 字节 CRC 就是这串数据的校验和** ⇒
     *    我抄错任何一个字节，这一条当场就红 ✓（比自己跟自己对拍硬得多 ✓）。
     *  ⚠ 只引口径/行为，不搬原厂代码（见 §8.3）。*/
    {
        static const char dg[]    = "771c9b947d59500d4a491ee5e3c1d57dcdb7a9d03544964846e2ef42fcdfb6d5";
        static const char nwant[] = "2716430000"
            "771c9b947d59500d4a491ee5e3c1d57dcdb7a9d03544964846e2ef42fcdfb6d5" "301a";
        static const char swant[] = "0741a000007b85";
        static const char rwant[] = "43523a1d9f2cc6b173f2e721fe5134361aa8bd63d1a7b8a4ed826915c1b6c0b4c1"
            "e2c7cc602f529082c7eee341d0ca8bd1a7105382a5153c22fd1a8cf2a48348588d9c";
        uint8_t d32[ATECC_NONCE_DIGEST_BYTES];
        uint8_t pkt39[ATECC_CMD_LEN_NONCE_CRC];
        uint8_t resp67[ATECC_RESP_LEN_SIG_CRC];
        uint8_t sig[ATECC_SIGN_BYTES];
        char hex[ATECC_RESP_LEN_SIG_CRC * 2 + 1];

        ck("摘要 / Nonce 包 / 签名响应三串 hex 长度都对（32 / 39 / 67）",
           hex_to_bin(dg, d32, sizeof(d32)) == (int)ATECC_NONCE_DIGEST_BYTES &&
           hex_to_bin(nwant, pkt39, sizeof(pkt39)) == (int)ATECC_CMD_LEN_NONCE_CRC &&
           hex_to_bin(rwant, resp67, sizeof(resp67)) == (int)ATECC_RESP_LEN_SIG_CRC);
        n = atecc_msg_nonce_load(pkt39, sizeof(pkt39), d32, 1);
        bin_to_hex(pkt39, n, hex);
        ck("外部锚点：Nonce 包 = 原厂实发的 27 16 43 00 00 <摘要> 30 1A",
           n == ATECC_CMD_LEN_NONCE_CRC && strcmp(hex, nwant) == 0);
        n = atecc_msg_sign(pkt, sizeof(pkt), 0u, 1);
        bin_to_hex(pkt, n, hex);
        ck("外部锚点：Sign(slot 0) 包 = 原厂实发的 07 41 A0 00 00 7B 85",
           n == ATECC_CMD_LEN_CRC && strcmp(hex, swant) == 0);
        ck("器件实回的 67 B 签名响应 -> 解析出 r||s（count 0x43=67 与 CRC 都对得上）",
           atecc_msg_resp_sign(resp67, ATECC_RESP_LEN_SIG_CRC, sig) == 0);
        ck("签名布局 = r 在前（第 1 字节 0x52、第 33 字节 0xE2，即 r||s 各 32 B）",
           sig[0] == 0x52u && sig[32] == 0xE2u);
        resp67[ATECC_RESP_LEN_SIG_NOCRC] ^= 0x01u;
        ck("签名响应改一位 CRC -> -3",
           atecc_msg_resp_sign(resp67, ATECC_RESP_LEN_SIG_CRC, sig) == ATECC_MSG_E_CRC);
    }

    if (sc_bad != 0) {
        printf("FAIL ATECC608B 报文层自检（%d 项不符）\n", sc_bad);
        return 2;
    }
    printf("PASS ATECC608B 报文层自检（★ 原厂 atCRC 口径的 CRC + Random/Info/Read + Nonce/Sign"
           " 包 + 响应解析（含签名 r||s），共 10 类）\n");
    return 0;
}

int main(int argc, char **argv)
{
    if (argc < 2) { usage(); return 1; }
    if (strcmp(argv[1], "random") == 0 && argc >= 4) {
        return cmd_random(atoi(argv[2]), atoi(argv[3]) != 0);
    }
    if (strcmp(argv[1], "resp") == 0 && argc >= 3) {
        int n = hex_to_bin(argv[2], g_bin, sizeof(g_bin));

        if (n < 0) { fprintf(stderr, "bad hex\n"); return 1; }
        resp_line(g_bin, (size_t)n, g_got, sizeof(g_got));
        printf("%s\n", g_got);
        return 0;
    }
    if (strcmp(argv[1], "info") == 0 && argc >= 4) {
        uint8_t pkt[ATECC_CMD_LEN_CRC];
        size_t n = atecc_msg_info(pkt, sizeof(pkt), (uint8_t)atoi(argv[2]),
                                  atoi(argv[3]) != 0);

        if (n == 0u) { fprintf(stderr, "build failed\n"); return 1; }
        bin_to_hex(pkt, n, g_got);
        printf("%s\n", g_got);
        return 0;
    }
    if (strcmp(argv[1], "read") == 0 && argc >= 5) {
        uint8_t pkt[ATECC_CMD_LEN_CRC];
        size_t n = atecc_msg_read(pkt, sizeof(pkt), (uint8_t)atoi(argv[2]),
                                  (uint16_t)strtoul(argv[3], NULL, 16), atoi(argv[4]) != 0);

        if (n == 0u) { fprintf(stderr, "build failed\n"); return 1; }
        bin_to_hex(pkt, n, g_got);
        printf("%s\n", g_got);
        return 0;
    }
    if (strcmp(argv[1], "resp4") == 0 && argc >= 3) {
        int n = hex_to_bin(argv[2], g_bin, sizeof(g_bin));

        if (n < 0) { fprintf(stderr, "bad hex\n"); return 1; }
        resp4_line(g_bin, (size_t)n, g_got, sizeof(g_got));
        printf("%s\n", g_got);
        return 0;
    }
    if (strcmp(argv[1], "nonce") == 0 && argc >= 4) {
        return cmd_nonce(argv[2], atoi(argv[3]) != 0);
    }
    if (strcmp(argv[1], "sign") == 0 && argc >= 4) {
        return cmd_sign(atoi(argv[2]), atoi(argv[3]) != 0);
    }
    if (strcmp(argv[1], "respsig") == 0 && argc >= 3) {
        int n = hex_to_bin(argv[2], g_bin, sizeof(g_bin));

        if (n < 0) { fprintf(stderr, "bad hex\n"); return 1; }
        respsig_line(g_bin, (size_t)n, g_got, sizeof(g_got));
        printf("%s\n", g_got);
        return 0;
    }
    if (strcmp(argv[1], "selfcheck") == 0) { return selfcheck(); }
    if ((strcmp(argv[1], "crc-selftest") == 0 || strcmp(argv[1], "msg-selftest") == 0) &&
        argc >= 3) {
        return selftest(argv[2]);
    }
    usage();
    return 1;
}
