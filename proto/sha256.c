/* sha256.c — SHA-256 实现（FIPS 180-4）。
 *
 * 写法照标准轮函数；不做任何“优化”（比如展开循环）——
 * 正确性靠 proto/run_cross_test.py 与 Python hashlib 对拍 + 分块自洽检查来保证。
 */
#include "sha256.h"

static const uint32_t K[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u,
    0x923f82a4u, 0xab1c5ed5u, 0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u,
    0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u, 0xe49b69c1u, 0xefbe4786u,
    0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u,
    0x06ca6351u, 0x14292967u, 0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u,
    0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u, 0xa2bfe8a1u, 0xa81a664bu,
    0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au,
    0x5b9cca4fu, 0x682e6ff3u, 0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
    0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u
};

static uint32_t rotr(uint32_t x, int n)
{
    return (x >> n) | (x << (32 - n));
}

static uint32_t load_be32(const unsigned char *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static void store_be32(unsigned char *p, uint32_t v)
{
    p[0] = (unsigned char)(v >> 24);
    p[1] = (unsigned char)(v >> 16);
    p[2] = (unsigned char)(v >> 8);
    p[3] = (unsigned char)v;
}

static void sha256_block(sha256_ctx_t *c, const unsigned char *p)
{
    uint32_t w[64];
    uint32_t a, b, cc, d, e, f, g, h;
    int i;

    for (i = 0; i < 16; i++) {
        w[i] = load_be32(p + i * 4);
    }
    for (i = 16; i < 64; i++) {
        uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }

    a = c->h[0]; b = c->h[1]; cc = c->h[2]; d = c->h[3];
    e = c->h[4]; f = c->h[5]; g = c->h[6];  h = c->h[7];

    for (i = 0; i < 64; i++) {
        uint32_t S1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
        uint32_t ch = (e & f) ^ ((~e) & g);
        uint32_t S0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
        uint32_t maj = (a & b) ^ (a & cc) ^ (b & cc);
        uint32_t t1 = h + S1 + ch + K[i] + w[i];
        uint32_t t2 = S0 + maj;

        h = g; g = f; f = e; e = d + t1;
        d = cc; cc = b; b = a; a = t1 + t2;
    }

    c->h[0] += a; c->h[1] += b; c->h[2] += cc; c->h[3] += d;
    c->h[4] += e; c->h[5] += f; c->h[6] += g;  c->h[7] += h;
}

void sha256_init(sha256_ctx_t *c)
{
    c->h[0] = 0x6a09e667u; c->h[1] = 0xbb67ae85u;
    c->h[2] = 0x3c6ef372u; c->h[3] = 0xa54ff53au;
    c->h[4] = 0x510e527fu; c->h[5] = 0x9b05688cu;
    c->h[6] = 0x1f83d9abu; c->h[7] = 0x5be0cd19u;
    c->len = 0;
    c->n = 0;
}

void sha256_update(sha256_ctx_t *c, const void *data, size_t len)
{
    const unsigned char *p = (const unsigned char *)data;

    if (p == NULL && len != 0) return;
    c->len += (uint64_t)len;

    if (c->n != 0) {                          /* 先把上次剩的凑满一块 */
        size_t need = 64 - c->n;
        size_t take = (len < need) ? len : need;
        for (size_t i = 0; i < take; i++) c->buf[c->n + i] = p[i];
        c->n += take;
        p += take;
        len -= take;
        if (c->n == 64) {
            sha256_block(c, c->buf);
            c->n = 0;
        }
    }
    while (len >= 64) {                       /* 整块直接算 */
        sha256_block(c, p);
        p += 64;
        len -= 64;
    }
    for (size_t i = 0; i < len; i++) c->buf[c->n + i] = p[i];
    c->n += len;
}

void sha256_final(sha256_ctx_t *c, unsigned char out[SHA256_DIGEST_LEN])
{
    uint64_t bits = c->len * 8ULL;
    unsigned char pad[72];
    size_t padlen;

    /* 0x80 + 若干个 0，使 (n + 1 + k) % 64 == 56 */
    pad[0] = 0x80;
    padlen = ((c->n + 1) <= 56) ? (56 - (c->n + 1)) : (56 + 64 - (c->n + 1));
    for (size_t i = 1; i <= padlen; i++) pad[i] = 0x00;
    sha256_update(c, pad, padlen + 1);

    /* 64 位大端比特长度 */
    {
        unsigned char lenbuf[8];
        for (int i = 0; i < 8; i++) {
            lenbuf[i] = (unsigned char)(bits >> (56 - 8 * i));
        }
        sha256_update(c, lenbuf, 8);
    }

    for (int i = 0; i < 8; i++) {
        store_be32(out + i * 4, c->h[i]);
    }
}

void sha256(const void *data, size_t len, unsigned char out[SHA256_DIGEST_LEN])
{
    sha256_ctx_t c;
    sha256_init(&c);
    sha256_update(&c, data, len);
    sha256_final(&c, out);
}
