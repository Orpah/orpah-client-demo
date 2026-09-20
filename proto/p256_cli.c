/* p256_cli.c — P-256 的 **host 侧** CLI（对拍 / 调试用；**不编进固件**）
 *
 * 子命令：
 *   pubkey <d-hex>                  -> 04||X||Y（65 B hex）/ ERR <code>
 *   pubkey-sn <sn> [gen]            -> <d-hex><TAB>04||X||Y
 *   on-curve <x-hex> <y-hex>        -> 1 / 0
 *   fe <add|sub|mul|sqr|inv> <a-hex> [b-hex] -> 结果 hex / ERR <code>
 *   selfcheck                       -> C 侧不变量（不需要文件），末尾 PASS/FAIL
 *   pubkey-selftest <vector-file>    行格式见 proto/README.md
 *                                    （kind sn|d, a1, a2, d-hex, pub65-hex）
 *
 * 退出码：0 = 全 PASS；2 = 有 FAIL；1 = 用法/文件错误。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "p256.h"

#define LINE_MAX 8192

static char    g_line[LINE_MAX];
static char    g_tmp[LINE_MAX];
static char    g_want[LINE_MAX];
static uint8_t g_bin[256];

static void usage(void)
{
    fprintf(stderr,
            "usage: p256_cli <cmd> [args]\n"
            "  pubkey <d-hex>\n"
            "  pubkey-sn <sn> [gen]\n"
            "  on-curve <x-hex> <y-hex>\n"
            "  fe add|sub|mul|sqr|inv <a-hex> [b-hex]\n"
            "  selfcheck\n"
            "  pubkey-selftest <vector-file>\n");
}

/* ------------------------------------------------------------------ */
/* 小工具                                                             */
/* ------------------------------------------------------------------ */
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

/* 32 字节大端十六进制 → p256_fe_t；返回 0 / -1 */
static int fe_from_hex(p256_fe_t *r, const char *hex, uint8_t *scratch)
{
    if (hex_to_bin(hex, scratch, P256_BYTES) != P256_BYTES) {
        return -1;
    }
    p256_fe_from_bytes(r, scratch);
    return 0;
}

/* ------------------------------------------------------------------ */
/* 子命令                                                             */
/* ------------------------------------------------------------------ */
static int cmd_pubkey(const char *dhex, int print_d)
{
    uint8_t d[P256_BYTES], pub[65];
    int rc;

    if (hex_to_bin(dhex, d, sizeof(d)) != P256_BYTES) {
        printf("ERR -10\n");
        return 0;
    }
    rc = p256_pubkey_from_priv(pub, d);
    if (rc != 0) {
        printf("ERR %d\n", rc);
        return 0;
    }
    bin_to_hex(pub, 65, g_tmp);
    if (print_d) {
        char dhexout[P256_BYTES * 2 + 1];
        bin_to_hex(d, P256_BYTES, dhexout);
        printf("%s\t%s\n", dhexout, g_tmp);
    } else {
        printf("%s\n", g_tmp);
    }
    return 0;
}

static int cmd_pubkey_sn(const char *sn, int gen)
{
    uint8_t d[P256_BYTES];
    char dhex[P256_BYTES * 2 + 1];
    int rc;

    rc = p256_demo_priv_from_sn(d, sn, gen);
    if (rc != 0) {
        printf("ERR %d\n", rc);
        return 0;
    }
    bin_to_hex(d, P256_BYTES, dhex);
    return cmd_pubkey(dhex, 1);
}

static int cmd_on_curve(const char *xhex, const char *yhex)
{
    uint8_t x[P256_BYTES], y[P256_BYTES];

    if (hex_to_bin(xhex, x, sizeof(x)) != P256_BYTES ||
        hex_to_bin(yhex, y, sizeof(y)) != P256_BYTES) {
        printf("ERR -10\n");
        return 0;
    }
    printf("%d\n", p256_point_on_curve(x, y));
    return 0;
}

static int cmd_fe(const char *op, const char *ahex, const char *bhex)
{
    p256_fe_t a, b, r;
    uint8_t out[P256_BYTES];

    if (fe_from_hex(&a, ahex, g_bin) != 0) { printf("ERR -10\n"); return 0; }
    if (strcmp(op, "sqr") == 0 || strcmp(op, "inv") == 0) {
        if (strcmp(op, "sqr") == 0) {
            p256_fe_sqr(&r, &a);
        } else {
            p256_fe_inv(&r, &a);
        }
    } else {
        if (bhex == NULL || fe_from_hex(&b, bhex, g_bin) != 0) { printf("ERR -10\n"); return 0; }
        if (strcmp(op, "add") == 0) {
            p256_fe_add(&r, &a, &b);
        } else if (strcmp(op, "sub") == 0) {
            p256_fe_sub(&r, &a, &b);
        } else if (strcmp(op, "mul") == 0) {
            p256_fe_mul(&r, &a, &b);
        } else {
            usage();
            return 1;
        }
    }
    p256_fe_to_bytes(out, &r);
    bin_to_hex(out, P256_BYTES, g_tmp);
    printf("%s\n", g_tmp);
    return 0;
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
    static const uint8_t d1[32] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                                   0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1};
    static const uint8_t d2[32] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                                   0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2};
    uint8_t dzero[32], dn1[32], n[32];
    uint8_t g65[65], p1[65], p2[65], q[65];
    p256_fe_t a, b, r, s, t, u, one, zero;
    int rc;

    /* ① d=1 ⇒ 公钥就是生成元本身（顺带证明 Gx/Gy 常量与 ladder 的起手正确）*/
    rc = p256_pubkey_from_priv(g65, d1);
    sc("pubkey(1) 成功", rc == 0);
    p256_order_bytes(n);
    sc("n 首字节 0xFF / 末字节 0x51", n[0] == 0xFFu && n[31] == 0x51u);
    sc("pubkey(1)[0] = 04（未压缩）", g65[0] == 0x04u);
    sc("pubkey(1) 在曲线上", p256_point_on_curve(g65 + 1, g65 + 33) == 1);

    /* ② d=n-1 ⇒ 公钥 = (Gx, p-Gy)（点取逆；这是很强的内部不变量）*/
    memcpy(dn1, n, 32);
    {
        /* dn1 = n - 1（n 最低字节是 0x51 ⇒ 直接减 1）*/
        int i;
        for (i = 31; i >= 0; i--) {
            if (dn1[i] > 0) { dn1[i]--; break; }
            dn1[i] = 0xFFu;
        }
    }
    rc = p256_pubkey_from_priv(p1, dn1);
    sc("pubkey(n-1) 成功", rc == 0);
    {
        p256_fe_t gx, gy, negy, x1, y1;
        p256_fe_from_bytes(&gx, g65 + 1);
        p256_fe_from_bytes(&gy, g65 + 33);
        p256_fe_zero(&zero);
        p256_fe_sub(&negy, &zero, &gy);
        p256_fe_from_bytes(&x1, p1 + 1);
        p256_fe_from_bytes(&y1, p1 + 33);
        sc("pubkey(n-1).x == Gx", p256_fe_cmp(&x1, &gx) == 0);
        sc("pubkey(n-1).y == p-Gy", p256_fe_cmp(&y1, &negy) == 0);
    }

    /* ③ d=2 与 ladder 的 2G 一致（换个入口再算一次）*/
    rc = p256_pubkey_from_priv(p2, d2);
    sc("pubkey(2) 成功", rc == 0);
    {
        uint8_t gx[32], gy[32], bady[32];
        p256_fe_t fx, fy;
        p256_fe_from_bytes(&fx, g65 + 1);
        p256_fe_from_bytes(&fy, g65 + 33);
        p256_fe_to_bytes(gx, &fx);
        p256_fe_to_bytes(gy, &fy);
        rc = p256_point_mul(q, d2, gx, gy);       /* 从 G 出发算 2G */
        sc("2*G（走 point_mul）与 pubkey(2) 一致", rc == 0 && memcmp(q, p2 + 1, 64) == 0);
        memset(dzero, 0, sizeof(dzero));
        sc("k=0 ⇒ E_ZERO", p256_point_mul(q, dzero, gx, gy) == P256_E_ZERO);
        memcpy(bady, gy, 32);
        bady[31] ^= 0x01u;                        /* 把点挪出曲线（只改 y） */
        sc("不在曲线上的点 ⇒ E_CURVE", p256_point_mul(q, d2, gx, bady) == P256_E_CURVE);
    }

    /* ④ 域运算：a*inv(a)=1、(a+b)^2 = a^2+2ab+b^2、1*any=any、0-1=p-1 */
    {
        uint8_t hx[32];
        int i;
        for (i = 0; i < 32; i++) hx[i] = (uint8_t)(0x11u * (uint32_t)(i % 7) + 3u);
        p256_fe_from_bytes(&a, hx);
        p256_fe_inv(&r, &a);
        p256_fe_mul(&s, &a, &r);
        p256_fe_one(&one);
        sc("a * inv(a) == 1", p256_fe_cmp(&s, &one) == 0);
        p256_fe_zero(&zero);
        p256_fe_inv(&r, &zero);
        sc("inv(0) == 0", p256_fe_is_zero(&r) == 1);
    }
    {
        uint8_t hx[32];
        int i;
        for (i = 0; i < 32; i++) hx[i] = (uint8_t)(0xF0u - (uint32_t)i * 5u);
        p256_fe_from_bytes(&a, hx);
        p256_fe_zero(&zero);
        p256_fe_one(&one);
        p256_fe_mul(&r, &a, &one);
        sc("a*1 == a", p256_fe_cmp(&r, &a) == 0);
        p256_fe_mul(&r, &a, &zero);
        sc("a*0 == 0", p256_fe_is_zero(&r) == 1);
        p256_fe_sub(&r, &zero, &one);              /* 0-1 = p-1 */
        p256_fe_add(&r, &r, &one);                 /* +1 ⇒ 0 */
        sc("(0-1)+1 == 0", p256_fe_is_zero(&r) == 1);
        /* (a+1)^2 == a^2 + 2a + 1 */
        p256_fe_add(&b, &a, &one);
        p256_fe_sqr(&s, &b);                       /* LHS */
        p256_fe_sqr(&r, &a);                       /* a^2 */
        p256_fe_add(&t, &a, &a);                   /* 2a */
        p256_fe_add(&r, &r, &t);
        p256_fe_add(&r, &r, &one);
        sc("(a+1)^2 == a^2+2a+1", p256_fe_cmp(&s, &r) == 0);
        /* (a+1)*(a-1) == a^2 - 1 */
        p256_fe_add(&b, &a, &one);
        p256_fe_sub(&t, &a, &one);
        p256_fe_mul(&s, &b, &t);
        p256_fe_sqr(&u, &a);
        p256_fe_sub(&u, &u, &one);
        sc("(a+1)(a-1) == a^2-1", p256_fe_cmp(&s, &u) == 0);
    }

    /* ⑤ 阶 n 的字节形状（粗查常量有没有写反）*/
    p256_order_bytes(n);
    sc("n 首字节 0xFF / 末字节 0x51", n[0] == 0xFFu && n[31] == 0x51u);

    if (sc_bad == 0) {
        printf("PASS selfcheck（不变量全过）\n");
    } else {
        printf("FAIL selfcheck：%d 项不成立\n", sc_bad);
    }
}

/* ------------------------------------------------------------------ */
/* 向量自检                                                           */
/* ------------------------------------------------------------------ */
static int selftest(const char *path)
{
    FILE *fp = fopen(path, "r");
    int lineno = 0, nrow = 0, nbad = 0;

    if (fp == NULL) {
        fprintf(stderr, "打不开向量文件：%s\n", path);
        return 1;
    }
    while (fgets(g_line, (int)sizeof(g_line), fp) != NULL) {
        char *cols[8];
        uint8_t d[P256_BYTES], pub[65];
        int n, rc;

        lineno++;
        chomp(g_line);
        if (g_line[0] == '\0' || g_line[0] == '#') continue;
        n = split_tabs(g_line, cols, 5);
        nrow++;
        if (n != 5) {
            printf("FAIL %s:%d 列数=%d（要 5）\n", path, lineno, n);
            nbad++;
            continue;
        }
        if (strcmp(cols[0], "sn") == 0) {
            char dhex[P256_BYTES * 2 + 1];
            rc = p256_demo_priv_from_sn(d, cols[1], (int)strtol(cols[2], NULL, 10));
            if (rc != 0) {
                printf("FAIL %s:%d derive ERR %d\n", path, lineno, rc);
                nbad++;
                continue;
            }
            bin_to_hex(d, P256_BYTES, dhex);
            if (strcmp(dhex, cols[3]) != 0) {
                printf("FAIL %s:%d 派生出的 d 不一致\n     C=%s\n   FILE=%s\n",
                       path, lineno, dhex, cols[3]);
                nbad++;
                continue;
            }
        } else if (strcmp(cols[0], "d") == 0) {
            if (hex_to_bin(cols[1], d, sizeof(d)) != P256_BYTES) {
                printf("FAIL %s:%d d 非法（要 64 个十六进制字符 = 32 字节；实际 %d 个）\n",
                       path, lineno, (int)strlen(cols[1]));
                nbad++;
                continue;
            }
        } else {
            printf("FAIL %s:%d 未知 kind=%s\n", path, lineno, cols[0]);
            nbad++;
            continue;
        }
        rc = p256_pubkey_from_priv(pub, d);
        if (rc != 0) {
            printf("FAIL %s:%d pubkey ERR %d\n", path, lineno, rc);
            nbad++;
            continue;
        }
        bin_to_hex(pub, 65, g_tmp);
        if (strcmp(g_tmp, cols[4]) != 0) {
            printf("FAIL %s:%d 公钥不一致\n     C=%s\n   FILE=%s\n", path, lineno, g_tmp, cols[4]);
            nbad++;
        }
    }
    fclose(fp);
    if (nbad > 0) {
        printf("FAIL %d/%d 行不一致（%s）\n", nbad, nrow, path);
        return 2;
    }
    printf("PASS %d/%d 行（%s）\n", nrow, nrow, path);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc < 2) { usage(); return 1; }
    if (strcmp(argv[1], "pubkey") == 0 && argc >= 3) return cmd_pubkey(argv[2], 0);
    if (strcmp(argv[1], "pubkey-sn") == 0 && argc >= 3) {
        return cmd_pubkey_sn(argv[2], argc >= 4 ? (int)strtol(argv[3], NULL, 10) : 1);
    }
    if (strcmp(argv[1], "on-curve") == 0 && argc >= 4) return cmd_on_curve(argv[2], argv[3]);
    if (strcmp(argv[1], "fe") == 0 && argc >= 4) {
        return cmd_fe(argv[2], argv[3], argc >= 5 ? argv[4] : NULL);
    }
    if (strcmp(argv[1], "selfcheck") == 0) {
        selfcheck();
        return (sc_bad == 0) ? 0 : 2;
    }
    if (strcmp(argv[1], "pubkey-selftest") == 0 && argc >= 3) return selftest(argv[2]);

    usage();
    return 1;
}
