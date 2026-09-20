/* hmac.c — HMAC-SHA256 实现（RFC 2104）。无 stdio / malloc / libc 依赖。 */
#include "hmac.h"

#define HMAC_BLOCK 64

static void z_zero(unsigned char *p, size_t n)
{
    for (size_t i = 0; i < n; i++) p[i] = 0;
}

static void z_copy(unsigned char *d, const unsigned char *s, size_t n)
{
    for (size_t i = 0; i < n; i++) d[i] = s[i];
}

void hmac_sha256(const unsigned char *key, size_t keylen,
                 const unsigned char *msg, size_t msglen,
                 unsigned char out[SHA256_DIGEST_LEN])
{
    unsigned char k[HMAC_BLOCK];
    unsigned char pad[HMAC_BLOCK];
    unsigned char inner[SHA256_DIGEST_LEN];
    sha256_ctx_t c;

    /* ① 归一化密钥到 64 字节：长于分组先哈希，短的右侧补零 */
    if (keylen > HMAC_BLOCK) {
        sha256(key, keylen, k);
        z_zero(k + SHA256_DIGEST_LEN, HMAC_BLOCK - SHA256_DIGEST_LEN);
    } else {
        z_copy(k, key, keylen);
        z_zero(k + keylen, HMAC_BLOCK - keylen);
    }

    /* ② inner = H((K xor ipad) || msg) */
    for (size_t i = 0; i < HMAC_BLOCK; i++) pad[i] = (unsigned char)(k[i] ^ 0x36);
    sha256_init(&c);
    sha256_update(&c, pad, HMAC_BLOCK);
    sha256_update(&c, msg, msglen);
    sha256_final(&c, inner);

    /* ③ out = H((K xor opad) || inner) */
    for (size_t i = 0; i < HMAC_BLOCK; i++) pad[i] = (unsigned char)(k[i] ^ 0x5C);
    sha256_init(&c);
    sha256_update(&c, pad, HMAC_BLOCK);
    sha256_update(&c, inner, SHA256_DIGEST_LEN);
    sha256_final(&c, out);

    /* ④ 清掉栈上的密钥材料（密钥派生值不该留在栈上） */
    z_zero(k, sizeof(k));
    z_zero(pad, sizeof(pad));
    z_zero(inner, sizeof(inner));
}
