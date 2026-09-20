/* rfc6979.c — RFC 6979 §3.2 的 k 派生（NIST P-256 + SHA-256）。
 *
 * 流程（逐条对齐 RFC 6979 §3.2，别自行"简化"）：
 *   a. h1 = H(m)                                     ← 由调用方给
 *   b. V = 0x01 × 32（= 8*ceil(hlen/8) 位）
 *   c. K = 0x00 × 32
 *   d. K = HMAC_K(V || 0x00 || int2octets(x) || bits2octets(h1))
 *   e. V = HMAC_K(V)
 *   f. K = HMAC_K(V || 0x01 || int2octets(x) || bits2octets(h1))    ← 0x01 这份别丢
 *   g. V = HMAC_K(V)
 *   h. 循环：V = HMAC_K(V); T = T || V（tlen=qlen ⇒ 一次 HMAC 就够）
 *            k = bits2int(T)；若 1 <= k <= n-1 就用它；
 *            否则 K = HMAC_K(V || 0x00); V = HMAC_K(V); 回到循环开头。
 *
 * ⚠ RFC 6979 §3.2 结尾那句**要点**：k 是拿 T 的 bits2int 去**与 n 比较**，
 *   **不是** mod n —— 做 mod n 会引入偏置（会削弱安全性），也是官方实现明说不许的。
 *
 * qlen=256 且 hlen=256 ⇒ `bits2int` 就是"按大端读成整数"，`bits2octets` 只需一次
 * 条件减 n（因为 h1 < 2^256 < 2n，最多减一次 —— RFC §2.3.4 也是这么说的）。
 *
 * 单一源：`proto/test_vectors_ecdsa_rfc6979.txt`（RFC §A.2.5 官方两组向量）。
 * 实测：`proto/run_cross_test.py` 里 C 与 Python 各实现一份，**都**要复现该夹具。
 */
#include "rfc6979.h"

#include "hmac.h"

#define HLEN SHA256_DIGEST_LEN          /* 32 */
#define QLEN_BYTES P256_BYTES           /* 32 */

/* n 与 h1 都是 32 B 大端 ⇒ 直接按字节比大小（无 string.h） */
static int ge_n(const uint8_t a[QLEN_BYTES])
{
    uint8_t n[QLEN_BYTES];
    size_t i;

    p256_order_bytes(n);
    for (i = 0; i < QLEN_BYTES; i++) {
        if (a[i] > n[i]) { return 1; }
        if (a[i] < n[i]) { return 0; }
    }
    return 1;                            /* 相等也算 >= */
}

static void sub_n(uint8_t a[QLEN_BYTES])
{
    uint8_t n[QLEN_BYTES];
    int borrow = 0;
    size_t i;

    p256_order_bytes(n);
    i = QLEN_BYTES;
    while (i-- > 0) {
        int d = (int)a[i] - (int)n[i] - borrow;
        if (d < 0) { d += 256; borrow = 1; } else { borrow = 0; }
        a[i] = (uint8_t)d;
    }
}

static int is_zero32(const uint8_t a[QLEN_BYTES])
{
    size_t i;
    for (i = 0; i < QLEN_BYTES; i++) { if (a[i] != 0) { return 0; } }
    return 1;
}

static void copy_b(uint8_t *d, const uint8_t *s, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++) { d[i] = s[i]; }
}

static void zero_b(uint8_t *d, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++) { d[i] = 0; }
}

int rfc6979_k_p256_sha256(uint8_t k_be[RFC6979_BYTES],
                          const uint8_t x_be[P256_BYTES],
                          const uint8_t h1[SHA256_DIGEST_LEN])
{
    uint8_t V[HLEN], K[HLEN], bh[HLEN];
    uint8_t buf[HLEN + 1 + P256_BYTES + HLEN];
    size_t i, off;
    int round, rc = RFC6979_E_RETRY;

    if (k_be == NULL || x_be == NULL || h1 == NULL) { return RFC6979_E_ARG; }

    /* bits2octets(h1)：z2 = bits2int(h1) mod n ⇒ 最多减一次 n */
    copy_b(bh, h1, HLEN);
    if (ge_n(bh)) { sub_n(bh); }

    /* b / c */
    for (i = 0; i < HLEN; i++) { V[i] = 0x01u; K[i] = 0x00u; }

    /* d / f 共用同一段拼接：V || <0x00 或 0x01> || int2octets(x) || bits2octets(h1) */
    for (round = 0; round < 2; round++) {
        off = 0;
        copy_b(buf + off, V, HLEN);  off += HLEN;
        buf[off++] = (round == 0) ? 0x00u : 0x01u;
        copy_b(buf + off, x_be, P256_BYTES);  off += P256_BYTES;
        copy_b(buf + off, bh, HLEN);          off += HLEN;
        hmac_sha256(K, HLEN, buf, off, K);          /* d / f：K = HMAC_K(...) */
        hmac_sha256(K, HLEN, V, HLEN, V);           /* e / g：V = HMAC_K(V)  */
    }

    /* h：循环取 k 候选（tlen == qlen ⇒ 每次只用一次 HMAC 输出，不用再拼 T）*/
    for (round = 0; round < 1000; round++) {
        hmac_sha256(K, HLEN, V, HLEN, k_be);   /* T（直接写在输出上）*/
        hmac_sha256(K, HLEN, V, HLEN, V);      /* V = HMAC_K(V) */
        if (!is_zero32(k_be) && !ge_n(k_be)) {
            rc = 0;                            /* 1 <= k <= n-1 ⇒ 就用它 */
            break;
        }
        /* 不合格：K = HMAC_K(V || 0x00); V = HMAC_K(V)（§3.2 h 末段）*/
        copy_b(buf, V, HLEN);
        buf[HLEN] = 0x00u;
        hmac_sha256(K, HLEN, buf, HLEN + 1, K);
        hmac_sha256(K, HLEN, V, HLEN, V);
    }

    /* 清掉栈上的中间量（K/V 是"由私钥导出的秘密"，不该留在栈上）*/
    zero_b(V, sizeof(V));
    zero_b(K, sizeof(K));
    zero_b(bh, sizeof(bh));
    zero_b(buf, sizeof(buf));
    if (rc != 0) { zero_b(k_be, RFC6979_BYTES); }
    return rc;
}
