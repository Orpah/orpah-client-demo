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
    n = atecc_msg_read(pkt, sizeof(pkt), ATECC_ZONE_CONFIG, ATECC_CFG_ADDR_LOCK, 0);
    ck("Read 配置区 @0x54 无 CRC：op 0x02 / param1=zone 0x00 / 地址大端 00 54",
       n == ATECC_CMD_LEN_NOCRC && pkt[1] == ATECC_OP_READ && pkt[2] == ATECC_ZONE_CONFIG &&
       pkt[3] == 0x00u && pkt[4] == 0x54u);
    n = atecc_msg_read(pkt, sizeof(pkt), ATECC_ZONE_CONFIG, 0x0154u, 1);
    ck("Read 地址 0x0154 带 CRC：地址字节 = 01 54（大端）",
       n == ATECC_CMD_LEN_CRC && pkt[3] == 0x01u && pkt[4] == 0x54u);
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

    if (sc_bad != 0) {
        printf("FAIL ATECC608B 报文层自检（%d 项不符）\n", sc_bad);
        return 2;
    }
    printf("PASS ATECC608B 报文层自检（★ 原厂 atCRC 口径的 CRC + Random/Info/Read 包"
           " + 响应解析 9 类）\n");
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
    if (strcmp(argv[1], "selfcheck") == 0) { return selfcheck(); }
    if ((strcmp(argv[1], "crc-selftest") == 0 || strcmp(argv[1], "msg-selftest") == 0) &&
        argc >= 3) {
        return selftest(argv[2]);
    }
    usage();
    return 1;
}
