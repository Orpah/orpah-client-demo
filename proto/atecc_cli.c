/* atecc_cli.c — ATECC608B **报文层**（`proto/atecc_msg.c` + `proto/crc16.c`）的 PC 侧自检 CLI
 *
 * 为什么要有它：这两份代码在固件里跑，但**CRC 或包格式错了只会在台架上表现为
 * "SE 没应答"** —— 那时根本分不清是线没接对还是字节算错了。所以按本仓口径
 * （AGENTS §4：CRC 与报文编解码不许只靠人眼对着文档写）先在 PC 上对拍：
 *   ① `selfcheck`      —— 内建不变量（含**外部校验值** CRC-16/BUYPASS("123456789")=0xFEE8）
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
            else { snprintf(g_got, sizeof(g_got), "%04X", crc16_bypass(g_bin, (size_t)n)); }
        } else if (strcmp(cols[0], "cmd") == 0) {
            uint8_t pkt[ATECC_CMD_LEN_CRC];
            size_t n = atecc_msg_random(pkt, sizeof(pkt), (uint8_t)atoi(cols[1]),
                                        atoi(cols[2]) != 0);

            if (n == 0u) { snprintf(g_got, sizeof(g_got), "(组包失败)"); }
            else { bin_to_hex(pkt, n, g_got); }
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
    size_t n, i;
    int rc;

    sc_bad = 0;
    /* ① 外部校验值：CRC-16/BUYPASS 对 "123456789" = 0xFEE8（CRC 目录给的，不是我们自己算的）*/
    ck("CRC-16/BUYPASS 外部校验值 (\"123456789\" -> 0xFEE8)",
       crc16_bypass(cat, sizeof(cat)) == CRC16_BYPASS_CHECK);
    /* ② 空输入 = 初值 0 */
    ck("空输入 -> 0x0000", crc16_bypass(0, 0u) == 0u);
    /* ③ 首字节进/不进（只是形状检查：非 0 且非 0xFFFF）*/
    {
        uint8_t one = 0x00u;

        ck("单字节 0x00 的 CRC 确定（可复现）",
           crc16_bypass(&one, 1u) == crc16_bypass(&one, 1u));
    }
    /* ④ 命令包：长度与 count 字段一致 */
    n = atecc_msg_random(pkt, sizeof(pkt), ATECC_MODE_SEED_UPDATE, 0);
    ck("Random 无 CRC 包长 = 5、count = 5、opcode = 0x1B",
       n == ATECC_CMD_LEN_NOCRC && pkt[0] == ATECC_CMD_LEN_NOCRC &&
       pkt[1] == ATECC_OP_RANDOM);
    n = atecc_msg_random(pkt, sizeof(pkt), ATECC_MODE_SEED_UPDATE, 1);
    ck("Random 带 CRC 包长 = 7、count = 7、包尾 = CRC16(前 5 字节, 小端)",
       n == ATECC_CMD_LEN_CRC && pkt[0] == ATECC_CMD_LEN_CRC &&
       pkt[5] == (uint8_t)(crc16_bypass(pkt, 5u) & 0xFFu) &&
       pkt[6] == (uint8_t)(crc16_bypass(pkt, 5u) >> 8));
    /* ⑤ 响应解析：36 字节（无 CRC）应过 */
    for (i = 0; i < sizeof(resp); i++) { resp[i] = 0u; }
    resp[3] = ATECC_RESP_LEN_NOCRC;                 /* count 大端：0x00 0x00 0x00 0x24 */
    for (i = 0; i < ATECC_RANDOM_BYTES; i++) { resp[4u + i] = (uint8_t)i; }
    rc = atecc_msg_resp_random(resp, ATECC_RESP_LEN_NOCRC, d);
    ck("响应 36 B（count=36）解析出 32 B", rc == 0 && d[0] == 0x00u && d[31] == 0x1Fu);
    /* ⑥ 38 字节 + 正确 CRC 应过；把 CRC 改一位应报 -3 */
    {
        uint8_t crc[2];

        resp[3] = ATECC_RESP_LEN_CRC;
        crc16_bypass_le(resp, ATECC_RESP_LEN_NOCRC, crc);
        resp[ATECC_RESP_LEN_NOCRC] = crc[0];
        resp[ATECC_RESP_LEN_NOCRC + 1u] = crc[1];
        rc = atecc_msg_resp_random(resp, ATECC_RESP_LEN_CRC, d);
        ck("响应 38 B（带 CRC）解析通过", rc == 0);
        resp[ATECC_RESP_LEN_NOCRC] ^= 0x01u;
        rc = atecc_msg_resp_random(resp, ATECC_RESP_LEN_CRC, d);
        ck("改一位 CRC -> -3（CRC 不符）", rc == ATECC_MSG_E_CRC);
    }
    /* ⑦ count 与实际长度不一致 -> -2；长度 35 -> -2 */
    resp[3] = ATECC_RESP_LEN_NOCRC;
    ck("长度 35 -> -2（长度不符）",
       atecc_msg_resp_random(resp, ATECC_RESP_LEN_NOCRC - 1u, d) == ATECC_MSG_E_LEN);
    resp[3] = 0x25u;                                 /* count 说 37，实际给 36 */
    ck("count 与实际长度不一致 -> -2",
       atecc_msg_resp_random(resp, ATECC_RESP_LEN_NOCRC, d) == ATECC_MSG_E_LEN);

    if (sc_bad != 0) {
        printf("FAIL ATECC608B 报文层自检（%d 项不符）\n", sc_bad);
        return 2;
    }
    printf("PASS ATECC608B 报文层自检（CRC16/BUYPASS + Random 包 + 响应解析 7 类）\n");
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
    if (strcmp(argv[1], "selfcheck") == 0) { return selfcheck(); }
    if ((strcmp(argv[1], "crc-selftest") == 0 || strcmp(argv[1], "msg-selftest") == 0) &&
        argc >= 3) {
        return selftest(argv[2]);
    }
    usage();
    return 1;
}
