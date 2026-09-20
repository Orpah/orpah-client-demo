/* hmac.h — HMAC-SHA256（RFC 2104），纯 C99、无 stdio / 无 malloc
 *
 * 用途：规范 §5.1 的**降级 HS256**：
 *     ES256: sig = ECDSA-P256-sign(privkey, SHA-256(preimage))
 *     HS256: sig = HMAC-SHA256(symkey, preimage)        ← 就是本文件
 * ⚠ 注意区别：HS256 **直接对 preimage 做 HMAC**，**不再先哈希**（哈希在 HMAC 内部做）。
 *   设备侧持有的一把 32 字节对称密钥（Slot 5 / CH32 保护区）就是这里的 key。
 *
 * 单一源：Python `hmac.new(key, msg, hashlib.sha256).digest()`（交叉测试按它拍）。
 */
#ifndef ORPAH_HMAC_H
#define ORPAH_HMAC_H

#include <stddef.h>

#include "sha256.h"

/* 输出固定 32 字节。key 可以比分组（64B）长（内部先哈希）、也可以为空。 */
void hmac_sha256(const unsigned char *key, size_t keylen,
                 const unsigned char *msg, size_t msglen,
                 unsigned char out[SHA256_DIGEST_LEN]);

#endif /* ORPAH_HMAC_H */
