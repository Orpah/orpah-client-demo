/* ecdsa_cli.c — ECDSA(P-256)/RFC 6979 的 **host 侧** CLI（对拍 / 调试用；**不编进固件**）
 *
 * 子命令（所有 hex 都不带前缀；给 h1 的命令就是"绕过哈希"，便于逐层定位错在哪一层）：
 *   h1 <msg-hex>                       -> SHA-256(msg) hex
 *   k <d-hex> <h1-hex>                 -> RFC 6979 的 k hex
 *   k-msg <d-hex> <msg-hex>            -> 同上，但自己先哈希
 *   sign <d-hex> <h1-hex>              -> <k>\t<r||s>          （RFC 6979）
 *   sign-msg <d-hex> <msg-hex>         -> <k>\t<r||s>          （先 SHA-256 再签）
 *   sign-k <d-hex> <k-hex> <h1-hex>    -> r||s                 （显式 k，边界用）
 *   selfcheck                          -> C 侧不变量（不需要文件），末尾 PASS/FAIL
 *   k-selftest <vector-file>           -> 只比 k（把 RFC 6979 与 ECDSA 两层分开看）
 *   sign-selftest <vector-file>        -> 比 k / r / s 三者
 *
 * 向量文件列（见 proto/README.md）：<label>\t<d>\t<h1>\t<k>\t<r>\t<s>（小写 hex）
 *
 * 退出码：0 = 全 PASS；2 = 有 FAIL；1 = 用法/文件错误。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ecdsa.h"
#include "rfc6979.h"

#define LINE_MAX 4096

static char    g_line[LINE_MAX];
static uint8_t g_bin[512];

static void usage(void)
{
    fprintf(stderr,
            "usage: ecdsa_cli <cmd> [args]\n"
            "  h1 <msg-hex>\n"
            "  k <d-hex> <h1-hex>\n"
            "  k-msg <d-hex> <msg-hex>\n"
            "  sign <d-hex> <h1-hex>\n"
            "  sign-msg <d-hex> <msg-hex>\n"
            "  sign-k <d-hex> <k-hex> <h1-hex>\n"
            "  selfcheck\n"
            "  k-selftest <vector-file>\n"
            "  sign-selftest <vector-file>\n");
}

/* ------------------------------------------------------------------ */
/* 小工具（与 p256_cli.c 同一套写法）                                  */
/* ------------------------------------------------------------------ */
static void chomp(char *s)
{
    size_t n = strlen(s);
    while (n > 0 && (s[n - 1] == '\n' || s[n - 1] == '\r')) { s[--n] = '\0'; }
}

static int split_tabs(char *s, char *cols[], int ncols)
{
    int n = 0;
    char *p = s;

    if (ncols <= 0) return 0;
    cols[n++] = p;
    while (*p != '\0') {
        if (*p == '\t') {
            *p = '\0';
            if (n >= ncols) return n;
            cols[n++] = p + 1;
        }
        p++;
    }
    return n;
}

static int hexval(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int hex_to_bin(const char *hex, uint8_t *out, size_t cap)
{
    size_t n, i;

    if (hex == NULL) return -1;
    n = strlen(hex);
    if (n % 2 != 0) return -1;
    if (n / 2 > cap) return -1;
    for (i = 0; i < n / 2; i++) {
        int hi = hexval((unsigned char)hex[i * 2]);
        int lo = hexval((unsigned char)hex[i * 2 + 1]);
        if (hi < 0 || lo < 0) return -1;
        out[i] = (uint8_t)((hi << 4) | lo);
    }
    return (int)(n / 2);
}

static void bin_to_hex(const uint8_t *b, size_t n, char *out)
{
    static const char D[] = "0123456789abcdef";
    size_t i;
    for (i = 0; i < n; i++) {
        out[i * 2] = D[(b[i] >> 4) & 0x0F];
        out[i * 2 + 1] = D[b[i] & 0x0F];
    }
    out[n * 2] = '\0';
}

/* 32 字节大端 hex → 32 字节缓冲；返回 0 / -1 */
static int b32_from_hex(uint8_t out[P256_BYTES], const char *hex)
{
    return (hex_to_bin(hex, out, P256_BYTES) == P256_BYTES) ? 0 : -1;
}

/* a -= b（32 字节大端，要求 a >= b）—— 只给 selfcheck 算 h1-n 用 */
static void b32_sub(uint8_t a[P256_BYTES], const uint8_t b[P256_BYTES])
{
    int borrow = 0, i;
    for (i = P256_BYTES - 1; i >= 0; i--) {
        int d = (int)a[i] - (int)b[i] - borrow;
        if (d < 0) { d += 256; borrow = 1; } else { borrow = 0; }
        a[i] = (uint8_t)d;
    }
}

/* ------------------------------------------------------------------ */
/* 子命令                                                             */
/* ------------------------------------------------------------------ */
static int cmd_h1(const char *msghex)
{
    uint8_t msg[4096], h1[SHA256_DIGEST_LEN];
    char out[SHA256_DIGEST_LEN * 2 + 1];
    int n = hex_to_bin(msghex, msg, sizeof(msg));

    if (n < 0) { printf("ERR -10\n"); return 0; }
    sha256(msg, (size_t)n, h1);
    bin_to_hex(h1, SHA256_DIGEST_LEN, out);
    printf("%s\n", out);
    return 0;
}

static int cmd_k(const char *dhex, const char *h1hex, int hash_first, const char *msghtx)
{
    uint8_t d[P256_BYTES], h1[SHA256_DIGEST_LEN], k[P256_BYTES];
    uint8_t msg[4096];
    char out[P256_BYTES * 2 + 1];
    int rc;

    if (b32_from_hex(d, dhex) != 0) { printf("ERR -10\n"); return 0; }
    if (hash_first) {
        int n = hex_to_bin(msghtx, msg, sizeof(msg));
        if (n < 0) { printf("ERR -10\n"); return 0; }
        sha256(msg, (size_t)n, h1);
    } else if (b32_from_hex(h1, h1hex) != 0) {
        printf("ERR -10\n");
        return 0;
    }
    rc = rfc6979_k_p256_sha256(k, d, h1);
    if (rc != 0) { printf("ERR %d\n", rc); return 0; }
    bin_to_hex(k, P256_BYTES, out);
    printf("%s\n", out);
    return 0;
}

static int emit_sign(uint8_t out64[ECDSA_BYTES], uint8_t k[P256_BYTES], int show_k)
{
    char rhex[P256_BYTES * 2 + 1], shex[P256_BYTES * 2 + 1], khex[P256_BYTES * 2 + 1];

    bin_to_hex(out64, P256_BYTES, rhex);
    bin_to_hex(out64 + P256_BYTES, P256_BYTES, shex);
    if (show_k) {
        bin_to_hex(k, P256_BYTES, khex);
        printf("%s\t%s%s\n", khex, rhex, shex);
    } else {
        printf("%s%s\n", rhex, shex);
    }
    return 0;
}

static int cmd_sign(const char *dhex, const char *h1hex, int hash_first, const char *msghtx)
{
    uint8_t d[P256_BYTES], h1[SHA256_DIGEST_LEN], k[P256_BYTES], sig[ECDSA_BYTES];
    uint8_t msg[4096];
    int rc;

    if (b32_from_hex(d, dhex) != 0) { printf("ERR -10\n"); return 0; }
    if (hash_first) {
        int n = hex_to_bin(msghtx, msg, sizeof(msg));
        if (n < 0) { printf("ERR -10\n"); return 0; }
        sha256(msg, (size_t)n, h1);
    } else if (b32_from_hex(h1, h1hex) != 0) {
        printf("ERR -10\n");
        return 0;
    }
    rc = rfc6979_k_p256_sha256(k, d, h1);
    if (rc != 0) { printf("ERR %d\n", rc); return 0; }
    rc = ecdsa_sign_p256(sig, d, k, h1);
    if (rc != 0) { printf("ERR %d\n", rc); return 0; }
    return emit_sign(sig, k, 1);
}

static int cmd_sign_k(const char *dhex, const char *khex, const char *h1hex)
{
    uint8_t d[P256_BYTES], k[P256_BYTES], h1[SHA256_DIGEST_LEN], sig[ECDSA_BYTES];
    int rc;

    if (b32_from_hex(d, dhex) != 0 || b32_from_hex(k, khex) != 0 ||
        b32_from_hex(h1, h1hex) != 0) {
        printf("ERR -10\n");
        return 0;
    }
    rc = ecdsa_sign_p256(sig, d, k, h1);
    if (rc != 0) { printf("ERR %d\n", rc); return 0; }
    return emit_sign(sig, k, 0);
}

/* ------------------------------------------------------------------ */
/* selfcheck：C 侧不变量（不依赖外部参照）                             */
/* ------------------------------------------------------------------ */
static int sc_bad = 0;

static void sc(const char *name, int ok)
{
    if (!ok) {
        printf("FAIL %s\n", name);
        sc_bad++;
    }
}

static void selfcheck(void)
{
    static const uint8_t D1[32] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                                   0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1};
    static const uint8_t D2[32] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                                   0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2};
    static const uint8_t D3[32] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                                   0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 3};
    uint8_t n[P256_BYTES], dzero[32], kg65[65], gx[P256_BYTES];
    uint8_t h1z[32], h1f[32], h1f_minus_n[32];
    uint8_t sig1[ECDSA_BYTES], sig2[ECDSA_BYTES], siga[ECDSA_BYTES], sigb[ECDSA_BYTES];
    uint8_t k1[32], kk[32];
    int i, rc;

    memset(h1z, 0, sizeof(h1z));
    /* ⚠ 必须显式清零：下面几个"特殊值"缓冲都是**整段**参与运算的，
     *   只写末字节（k1[31]=1）会留下 31 字节栈垃圾 ⇒ 正确实现也会被判红（踩过）。*/
    memset(k1, 0, sizeof(k1));
    memset(h1f, 0, sizeof(h1f));

    /* ① n 的常量与阶一致；生成元 G = 1*G（后面几项都拿它当参照）*/
    p256_order_bytes(n);
    sc("n 首字节 0xff / 末字节 0x51", n[0] == 0xFFu && n[31] == 0x51u);
    rc = p256_pubkey_from_priv(kg65, D1);
    sc("pubkey(1) 成功", rc == 0);
    memcpy(gx, kg65 + 1, P256_BYTES);

    /* ② k=1 且 d=1、h1=0 ⇒ R=G ⇒ r=Gx，且 s = k⁻¹(h+d·r) = Gx（两者都该等于 Gx）
     *    —— 这是**不需要大整数**就能判的强不变量（r 与 s 同时被钉住）*/
    k1[31] = 1;
    rc = ecdsa_sign_p256(sig1, D1, k1, h1z);
    sc("k=1,d=1,h1=0 签名成功", rc == 0);
    sc("k=1,d=1,h1=0 ⇒ r == Gx", memcmp(sig1, gx, P256_BYTES) == 0);
    sc("k=1,d=1,h1=0 ⇒ s == Gx", memcmp(sig1 + P256_BYTES, gx, P256_BYTES) == 0);

    /* ③ RFC 6979 是确定性的：同一 (d,h1) 两次必须逐字节相同 */
    rc = ecdsa_sign_p256_rfc6979(sig1, D2, h1z);
    rc |= ecdsa_sign_p256_rfc6979(sig2, D2, h1z);
    sc("rfc6979 签名成功（d=2）", rc == 0);
    sc("同 (d,h1) 两次签名逐字节相同", memcmp(sig1, sig2, ECDSA_BYTES) == 0);
    sc("r != 0 且 s != 0", memcmp(sig1, h1z, 32) != 0 && memcmp(sig1 + 32, h1z, 32) != 0);

    /* ④ 换 h1 必须换签名（否则可能把 h1 漏进了某个缓存）*/
    h1f[0] = 0x5Au;
    rc = ecdsa_sign_p256_rfc6979(sig2, D2, h1f);
    sc("不同 h1 ⇒ 不同签名", rc == 0 && memcmp(sig1, sig2, ECDSA_BYTES) != 0);

    /* ⑤ h1 >= n 的分支（RFC 6979 的 bits2octets 要减一次 n；随机哈希几乎碰不到，
     *    所以这里**必须**手工造）：h1 = 0xff…ff 应与 h1-n 得到**同一个签名**
     *    （因为 h 与 bits2octets(h1) 都只依赖 h1 mod n）*/
    memset(h1f, 0xFF, sizeof(h1f));
    memcpy(h1f_minus_n, h1f, sizeof(h1f));
    b32_sub(h1f_minus_n, n);
    rc = ecdsa_sign_p256_rfc6979(siga, D2, h1f);
    rc |= ecdsa_sign_p256_rfc6979(sigb, D2, h1f_minus_n);
    sc("h1=0xff..ff（>=n）签名成功", rc == 0);
    sc("h1 与 h1-n 得同一签名（bits2octets 减 n 生效）",
       memcmp(siga, sigb, ECDSA_BYTES) == 0);

    /* ⑥ 非法输入必须有**明确错误码**，不许静默产出一个签名 */
    memset(dzero, 0, sizeof(dzero));
    sc("k=0 ⇒ E_ZERO", ecdsa_sign_p256(sig1, D2, dzero, h1z) == ECDSA_E_ZERO);
    sc("d=0 ⇒ E_ZERO", ecdsa_sign_p256(sig1, dzero, k1, h1z) == ECDSA_E_ZERO);
    sc("k=n（k*G=O）⇒ E_ZERO", ecdsa_sign_p256(sig1, D2, n, h1z) == ECDSA_E_ZERO);
    sc("d=n（私钥越界）⇒ E_ZERO", ecdsa_sign_p256(sig1, n, k1, h1z) == ECDSA_E_ZERO);
    sc("空指针 ⇒ E_ARG", ecdsa_sign_p256(NULL, D2, k1, h1z) == ECDSA_E_ARG);
    sc("rfc6979 空指针 ⇒ E_ARG", rfc6979_k_p256_sha256(kk, NULL, h1z) == RFC6979_E_ARG);

    /* ⑦ RFC 6979 的 k 必须在 [1, n-1]：抽查几把私钥/几个哈希，且两次一致 */
    for (i = 1; i <= 4; i++) {
        uint8_t dd[32];
        uint8_t hh[32];
        uint8_t kk2[32];
        int j;
        memset(dd, 0, sizeof(dd));
        dd[31] = (uint8_t)(i * 0x11);
        for (j = 0; j < 32; j++) { hh[j] = (uint8_t)((i * 37 + j * 11) & 0xFF); }
        sc("rfc6979 k 成功", rfc6979_k_p256_sha256(kk, dd, hh) == 0);
        sc("rfc6979 k 两次一致", rfc6979_k_p256_sha256(kk2, dd, hh) == 0 &&
                                memcmp(kk, kk2, 32) == 0);
        sc("rfc6979 k 非零", memcmp(kk, dzero, 32) != 0);
        /* k < n：逐字节比（相等也算越界，因为必须 <= n-1）*/
        {
            int lt = 0, jj;
            for (jj = 0; jj < 32; jj++) {
                if (kk[jj] < n[jj]) { lt = 1; break; }
                if (kk[jj] > n[jj]) { lt = 0; break; }
            }
            sc("rfc6979 k < n", lt == 1);
        }
    }

    /* ⑧ 便捷入口（先哈希）与手工两步等价 */
    {
        static const char msg[] = "orpah-selftest";
        uint8_t hh[32];
        sha256(msg, sizeof(msg) - 1, hh);
        rc = ecdsa_sign_p256_msg(siga, D3, msg, sizeof(msg) - 1);
        rc |= ecdsa_sign_p256_rfc6979(sigb, D3, hh);
        sc("sign_msg == sha256 + sign", rc == 0 && memcmp(siga, sigb, ECDSA_BYTES) == 0);
    }

    if (sc_bad == 0) { printf("PASS selfcheck（ECDSA/RFC6979 不变量）\n"); }
    else             { printf("FAIL selfcheck %d 项\n", sc_bad); }
}

/* ------------------------------------------------------------------ */
/* 向量自检                                                            */
/* ------------------------------------------------------------------ */
static int selftest(const char *path, int sign_too)
{
    FILE *f = fopen(path, "r");
    int nrows = 0, bad = 0;
    int lineno = 0;

    if (f == NULL) {
        printf("FAIL 打不开向量文件 %s\n", path);
        return 1;
    }
    while (fgets(g_line, sizeof(g_line), f) != NULL) {
        char *cols[8];
        char khex[P256_BYTES * 2 + 1], rhex[P256_BYTES * 2 + 1];
        char shex[P256_BYTES * 2 + 1], out[ECDSA_BYTES * 2 + 1];
        uint8_t d[P256_BYTES], h1[SHA256_DIGEST_LEN], k[P256_BYTES], sig[ECDSA_BYTES];
        int nc, rc;

        lineno++;
        chomp(g_line);
        if (g_line[0] == '\0' || g_line[0] == '#') { continue; }
        nc = split_tabs(g_line, cols, 8);
        if (nc < 6) {
            printf("FAIL %s:%d 列数不足（%d < 6）\n", path, lineno, nc);
            bad++;
            continue;
        }
        nrows++;
        if (b32_from_hex(d, cols[1]) != 0 || b32_from_hex(h1, cols[2]) != 0) {
            printf("FAIL %s:%d d/h1 不是 32 字节 hex\n", path, lineno);
            bad++;
            continue;
        }
        rc = rfc6979_k_p256_sha256(k, d, h1);
        bin_to_hex(k, P256_BYTES, khex);
        if (rc != 0 || strcmp(khex, cols[3]) != 0) {
            printf("FAIL %s:%d [%s] k = %s（期望 %s）rc=%d\n",
                   path, lineno, cols[0], khex, cols[3], rc);
            bad++;
            continue;
        }
        if (!sign_too) { continue; }
        rc = ecdsa_sign_p256(sig, d, k, h1);
        bin_to_hex(sig, P256_BYTES, rhex);
        bin_to_hex(sig + P256_BYTES, P256_BYTES, shex);
        if (rc != 0 || strcmp(rhex, cols[4]) != 0 || strcmp(shex, cols[5]) != 0) {
            printf("FAIL %s:%d [%s] r||s = %s%s（期望 %s%s）rc=%d\n",
                   path, lineno, cols[0], rhex, shex, cols[4], cols[5], rc);
            bad++;
            continue;
        }
        bin_to_hex(sig, ECDSA_BYTES, out);
    }
    fclose(f);
    if (bad != 0) {
        printf("FAIL %s：%d/%d 行不对\n", path, bad, nrows);
        return 2;
    }
    if (nrows == 0) {
        printf("FAIL %s：没有数据行\n", path);
        return 2;
    }
    printf("PASS %s-selftest %d/%d 行（%s）\n",
           sign_too ? "sign" : "k", nrows, nrows, path);
    return 0;
}

/* ------------------------------------------------------------------ */
int main(int argc, char **argv)
{
    const char *cmd;

    if (argc < 2) { usage(); return 1; }
    cmd = argv[1];

    if (strcmp(cmd, "h1") == 0 && argc == 3)            { return cmd_h1(argv[2]); }
    if (strcmp(cmd, "k") == 0 && argc == 4)             { return cmd_k(argv[2], argv[3], 0, NULL); }
    if (strcmp(cmd, "k-msg") == 0 && argc == 4)         { return cmd_k(argv[2], NULL, 1, argv[3]); }
    if (strcmp(cmd, "sign") == 0 && argc == 4)          { return cmd_sign(argv[2], argv[3], 0, NULL); }
    if (strcmp(cmd, "sign-msg") == 0 && argc == 4)      { return cmd_sign(argv[2], NULL, 1, argv[3]); }
    if (strcmp(cmd, "sign-k") == 0 && argc == 5)        { return cmd_sign_k(argv[2], argv[3], argv[4]); }
    if (strcmp(cmd, "selfcheck") == 0 && argc == 2)     { selfcheck(); return sc_bad == 0 ? 0 : 2; }
    if (strcmp(cmd, "k-selftest") == 0 && argc == 3)    { return selftest(argv[2], 0); }
    if (strcmp(cmd, "sign-selftest") == 0 && argc == 3) { return selftest(argv[2], 1); }

    usage();
    return 1;
}
