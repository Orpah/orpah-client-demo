/* sha256.h — SHA-256（FIPS 180-4），纯 C99、无 stdio / 无 malloc
 *
 * 用途：这是 **c4 签名的前置** ——
 *   · ES256：`sig = ECDSA-P256-sign(privkey, SHA-256(preimage))`（规范 §5.1）
 *   · 降级 HS256：**直接对 preimage 做 HMAC**（不再先哈希）—— 见 §5.1 那句
 *   · `payload.nonce` 的白化 / 将来 (RFC 6979) 确定性 k 也要它
 *
 * 单一源：Python `hashlib.sha256`（交叉测试按这个对拍，`proto/run_cross_test.py`）。
 * ⚠ 本文件**不**做 HMAC（c4 再写，届时同样对拍 Python 的 `hmac`）。
 */
#ifndef ORPAH_SHA256_H
#define ORPAH_SHA256_H

#include <stddef.h>
#include <stdint.h>

#define SHA256_DIGEST_LEN 32

typedef struct {
    uint32_t h[8];
    uint64_t len;                 /* 已吃进的字节数 */
    unsigned char buf[64];
    size_t n;                     /* buf 里待处理的字节数 */
} sha256_ctx_t;

void sha256_init(sha256_ctx_t *c);
void sha256_update(sha256_ctx_t *c, const void *data, size_t len);
void sha256_final(sha256_ctx_t *c, unsigned char out[SHA256_DIGEST_LEN]);
void sha256(const void *data, size_t len, unsigned char out[SHA256_DIGEST_LEN]);

#endif /* ORPAH_SHA256_H */
