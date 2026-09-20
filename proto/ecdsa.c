/* ecdsa.c — ECDSA(P-256) 签名：r||s。见 ecdsa.h 的文件头（做过/没做什么）。
 *
 * 内部只有一件事需要新写：**mod n 的 256 位算术**（r = R.x mod n、s = k⁻¹(h+d·r) mod n）。
 * 曲线侧（k*G、点位、域运算）全部复用 `p256.c`，一行都不重写 ——
 * 而且 `k*G` 直接用 `p256_pubkey_from_priv(k)`（"以 k 为私钥的公钥"就是 k*G），
 * 所以**不需要**再暴露生成元常量。
 *
 * mod n 的表示：8 个 32 位 limb，**小端**（v[0] 是最低位），与字节序的大端互转在
 * bn_from_bytes / bn_to_bytes 里各做一次。
 *
 * ★ 归约用"逐位长除法"（512 次 移位+比较+条件减）：慢、短、**好审计**。
 *   每次签名 384 次模乘（费马求逆 256 平方 + ~128 乘）⇒ host 上毫秒级、固件上秒级。
 *   这不是"性能疏忽"，是取舍：正确性优先，且签名是 60 s 一次的低频动作。
 */
#include "ecdsa.h"

#include "rfc6979.h"

#define NL 8                       /* 256 位 / 32 位 = 8 limb */

typedef struct { uint32_t v[NL]; } bn_t;

/* ------------------------------------------------------------------ */
/* 256 位小工具（无 string.h / 无结构体赋值：固件 -nostdlib 要能编）   */
/* ------------------------------------------------------------------ */
static void bn_zero(bn_t *r)
{
    int i;
    for (i = 0; i < NL; i++) { r->v[i] = 0u; }
}

static void bn_copy(bn_t *r, const bn_t *a)
{
    int i;
    for (i = 0; i < NL; i++) { r->v[i] = a->v[i]; }
}

static int bn_is_zero(const bn_t *a)
{
    int i;
    for (i = 0; i < NL; i++) { if (a->v[i] != 0u) { return 0; } }
    return 1;
}

static int bn_cmp(const bn_t *a, const bn_t *b)
{
    int i = NL;
    while (i-- > 0) {
        if (a->v[i] > b->v[i]) { return 1; }
        if (a->v[i] < b->v[i]) { return -1; }
    }
    return 0;
}

static void bn_from_bytes(bn_t *r, const uint8_t be[P256_BYTES])
{
    int i;
    for (i = 0; i < NL; i++) {
        int o = P256_BYTES - 4 * (i + 1);
        r->v[i] = ((uint32_t)be[o] << 24) | ((uint32_t)be[o + 1] << 16)
                | ((uint32_t)be[o + 2] << 8) | (uint32_t)be[o + 3];
    }
}

static void bn_to_bytes(uint8_t be[P256_BYTES], const bn_t *a)
{
    int i;
    for (i = 0; i < NL; i++) {
        int o = P256_BYTES - 4 * (i + 1);
        be[o]     = (uint8_t)(a->v[i] >> 24);
        be[o + 1] = (uint8_t)(a->v[i] >> 16);
        be[o + 2] = (uint8_t)(a->v[i] >> 8);
        be[o + 3] = (uint8_t)(a->v[i]);
    }
}

/* r = a - b（要求 a >= b） */
static void bn_sub(bn_t *r, const bn_t *a, const bn_t *b)
{
    uint64_t borrow = 0;
    int i;

    for (i = 0; i < NL; i++) {
        uint64_t t = (uint64_t)a->v[i] - (uint64_t)b->v[i] - borrow;
        r->v[i] = (uint32_t)t;
        borrow = (t >> 32) & 1u;
    }
}

/* r = a - k（a >= k；k 是个小常数，如 2） */
static void bn_sub_small(bn_t *r, const bn_t *a, uint32_t k)
{
    uint64_t borrow = (uint64_t)k;
    int i;

    for (i = 0; i < NL; i++) {
        uint64_t t = (uint64_t)a->v[i] - borrow;
        r->v[i] = (uint32_t)t;
        borrow = (t >> 32) & 1u;
    }
}

/* ------------------------------------------------------------------ */
/* 9-limb 临时量：给"移位后可能多一位"的中间结果用                     */
/* ------------------------------------------------------------------ */
/* t 与 n 比较（t[8] 是最高 limb；n < 2^256 ⇒ t[8] != 0 就直接更大） */
static int cmp9_n(const uint32_t t[NL + 1], const bn_t *n)
{
    int i;

    if (t[NL] != 0u) { return 1; }
    for (i = NL - 1; i >= 0; i--) {
        if (t[i] > n->v[i]) { return 1; }
        if (t[i] < n->v[i]) { return -1; }
    }
    return 0;
}

/* t -= n（要求 t >= n） */
static void sub9_n(uint32_t t[NL + 1], const bn_t *n)
{
    uint64_t borrow = 0;
    int i;

    for (i = 0; i < NL; i++) {
        uint64_t x = (uint64_t)t[i] - (uint64_t)n->v[i] - borrow;
        t[i] = (uint32_t)x;
        borrow = (x >> 32) & 1u;
    }
    t[NL] = (uint32_t)((uint64_t)t[NL] - borrow);
}

/* ------------------------------------------------------------------ */
/* mod n 的四则                                                        */
/* ------------------------------------------------------------------ */
/* r = (512 位大端数 be) mod n —— 逐位长除法（余数 r < n，故 9 limb 够）*/
static void mod_n(bn_t *r, const uint8_t be[2 * P256_BYTES], const bn_t *n)
{
    uint32_t rem[NL + 1];
    uint32_t bit;
    int i, j;

    for (j = 0; j <= NL; j++) { rem[j] = 0u; }
    for (i = 2 * P256_BYTES * 8 - 1; i >= 0; i--) {
        /* rem = rem*2 + bit_i(be)；rem < n < 2^256 ⇒ rem*2+1 < 2^257（9 limb 装得下，
         * 最高 limb 最多是 1，不会溢出）*/
        bit = (uint32_t)((be[(2 * P256_BYTES - 1) - (i >> 3)] >> (i & 7)) & 1u);
        for (j = 0; j <= NL; j++) {
            uint32_t nv = (rem[j] << 1) | bit;
            bit = rem[j] >> 31;
            rem[j] = nv;
        }
        if (cmp9_n(rem, n) >= 0) { sub9_n(rem, n); }
    }
    for (j = 0; j < NL; j++) { r->v[j] = rem[j]; }
}

static void bn_mulmod(bn_t *r, const bn_t *a, const bn_t *b, const bn_t *n)
{
    uint32_t p[NL * 2];
    uint8_t be[2 * P256_BYTES];
    int i, j;

    for (i = 0; i < NL * 2; i++) { p[i] = 0u; }
    for (i = 0; i < NL; i++) {                    /* 教科书式 8×8 → 16 limb */
        uint32_t carry = 0u;
        for (j = 0; j < NL; j++) {
            uint64_t t = (uint64_t)a->v[i] * (uint64_t)b->v[j]
                       + (uint64_t)p[i + j] + (uint64_t)carry;
            p[i + j] = (uint32_t)t;
            carry = (uint32_t)(t >> 32);
        }
        p[i + NL] = carry;                        /* 本轮最高位未被更早的轮次写过 */
    }
    for (i = 0; i < NL * 2; i++) {
        int o = (2 * P256_BYTES) - 4 * (i + 1);
        be[o]     = (uint8_t)(p[i] >> 24);
        be[o + 1] = (uint8_t)(p[i] >> 16);
        be[o + 2] = (uint8_t)(p[i] >> 8);
        be[o + 3] = (uint8_t)(p[i]);
    }
    mod_n(r, be, n);
}

static void bn_addmod(bn_t *r, const bn_t *a, const bn_t *b, const bn_t *n)
{
    uint32_t t[NL + 1];
    uint64_t carry = 0;
    int i;

    for (i = 0; i < NL; i++) {
        uint64_t s = (uint64_t)a->v[i] + (uint64_t)b->v[i] + carry;
        t[i] = (uint32_t)s;
        carry = s >> 32;
    }
    t[NL] = (uint32_t)carry;
    if (cmp9_n(t, n) >= 0) { sub9_n(t, n); }      /* a,b < n ⇒ 最多减一次 */
    for (i = 0; i < NL; i++) { r->v[i] = t[i]; }
}

/* r = a^(n-2) mod n（费马小定理；a=0 ⇒ r=0）。平方-乘，从最高位起。*/
static void bn_invmod(bn_t *r, const bn_t *a, const bn_t *n)
{
    bn_t e, base, acc;
    int i, started = 0;

    bn_sub_small(&e, n, 2u);
    bn_copy(&base, a);
    bn_zero(&acc);
    for (i = (NL * 32) - 1; i >= 0; i--) {
        if (started) { bn_mulmod(&acc, &acc, &acc, n); }
        if ((e.v[i >> 5] >> (i & 31)) & 1u) {
            if (!started) { bn_copy(&acc, &base); started = 1; }
            else          { bn_mulmod(&acc, &acc, &base, n); }
        }
    }
    bn_copy(r, &acc);
}

/* ------------------------------------------------------------------ */
/* 签名                                                                */
/* ------------------------------------------------------------------ */
int ecdsa_sign_p256(uint8_t out64[ECDSA_BYTES],
                    const uint8_t d_be[P256_BYTES],
                    const uint8_t k_be[P256_BYTES],
                    const uint8_t h1[SHA256_DIGEST_LEN])
{
    uint8_t n_be[P256_BYTES];
    uint8_t kg65[65];
    bn_t n, d, k, h, r, s, t, kinv;
    int rc;

    if (out64 == NULL || d_be == NULL || k_be == NULL || h1 == NULL) {
        return ECDSA_E_ARG;
    }

    p256_order_bytes(n_be);
    bn_from_bytes(&n, n_be);
    bn_from_bytes(&d, d_be);
    bn_from_bytes(&k, k_be);
    bn_from_bytes(&h, h1);

    if (bn_is_zero(&k) || bn_is_zero(&d)) { return ECDSA_E_ZERO; }
    if (bn_cmp(&d, &n) >= 0) { return ECDSA_E_ZERO; }   /* 私钥必须 < n */

    /* h = bits2int(h1) mod n（h1 < 2^256 < 2n ⇒ 最多减一次；与 RFC 6979 §2.4 一致）*/
    if (bn_cmp(&h, &n) >= 0) { bn_sub(&h, &h, &n); }

    /* R = k*G —— "以 k 为私钥的公钥"就是 k*G，直接复用（不必暴露生成元）*/
    rc = p256_pubkey_from_priv(kg65, k_be);
    if (rc != 0) { return (rc == P256_E_ZERO) ? ECDSA_E_ZERO : ECDSA_E_CURVE; }

    /* r = R.x mod n（R.x < p < 2n ⇒ 最多减一次）；r=0 必须换 k（RFC §3.4）*/
    bn_from_bytes(&r, kg65 + 1);
    if (bn_cmp(&r, &n) >= 0) { bn_sub(&r, &r, &n); }
    if (bn_is_zero(&r)) { return ECDSA_E_ZERO; }

    /* s = k⁻¹·(h + d·r) mod n；s=0 必须换 k。**不做 low-s 归一化**（见 ecdsa.h）*/
    bn_invmod(&kinv, &k, &n);
    bn_mulmod(&t, &d, &r, &n);
    bn_addmod(&t, &t, &h, &n);
    bn_mulmod(&s, &kinv, &t, &n);
    if (bn_is_zero(&s)) { return ECDSA_E_ZERO; }

    bn_to_bytes(out64, &r);
    bn_to_bytes(out64 + P256_BYTES, &s);

    bn_zero(&d); bn_zero(&k); bn_zero(&kinv); bn_zero(&t);
    return 0;
}

int ecdsa_sign_p256_rfc6979(uint8_t out64[ECDSA_BYTES],
                            const uint8_t d_be[P256_BYTES],
                            const uint8_t h1[SHA256_DIGEST_LEN])
{
    uint8_t k[P256_BYTES];
    int rc;

    if (out64 == NULL || d_be == NULL || h1 == NULL) { return ECDSA_E_ARG; }
    rc = rfc6979_k_p256_sha256(k, d_be, h1);
    if (rc != 0) { return ECDSA_E_ARG; }
    rc = ecdsa_sign_p256(out64, d_be, k, h1);
    {
        int i;
        for (i = 0; i < P256_BYTES; i++) { k[i] = 0u; }   /* k 用完即清 */
    }
    return rc;
}

int ecdsa_sign_p256_msg(uint8_t out64[ECDSA_BYTES],
                        const uint8_t d_be[P256_BYTES],
                        const void *msg, size_t msglen)
{
    uint8_t h1[SHA256_DIGEST_LEN];

    if (out64 == NULL || d_be == NULL || (msg == NULL && msglen != 0)) {
        return ECDSA_E_ARG;
    }
    sha256(msg, msglen, h1);
    return ecdsa_sign_p256_rfc6979(out64, d_be, h1);
}
