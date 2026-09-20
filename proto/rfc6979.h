/* rfc6979.h — RFC 6979 确定性 ECDSA 的 k 派生（**只做 NIST P-256 + SHA-256**）
 *
 * 纯逻辑：**无 stdio / malloc / 浮点 / string.h ⇒ 可编进固件**（`-nostdlib`）。
 *
 * 为什么要它（用户 2026-09-20 拍板）：设备侧**没有可信熵源**（ATECC608B 未接线，
 * CH32V203 也没有 RNG 可用），而 ECDSA 的 k **一旦重复/可预测就泄漏私钥**。
 * RFC 6979 用 `HMAC_DRBG` 从 (私钥 x, 消息哈希 h1) 确定性地导出 k ⇒ 不需要熵源，
 * 而且**同一输入必得同一签名** ⇒ 可拿官方向量逐字节对拍（这是可测性的关键）。
 *
 * ★ 与 §5.1 的关系：规范写的是 `sig = ECDSA-P256-sign(privkey, SHA-256(preimage))`，
 *   k 怎么来**规范不管**（`OrpahIDProtocol.md` §5.4 表的 `nonce` 才要求 ATECC608B RNG，
 *   那是**防重放字段**，与 k 是两件事）。本文件只负责 k。
 *
 * 单一源：RFC 6979 §3.2（算法）+ §A.2.5（NIST P-256/SHA-256 的官方向量，
 *   静态夹具 `test_vectors_ecdsa_rfc6979.txt`，见 `proto/README.md`）。
 *   ⚠ **不要**把 bits2octets 换成"直接用 h1"（§3.6 的变体）—— 那样就对不上官方向量了。
 *
 * 本文件**不**做：h1 的截断泛化（qlen=256=hlen ⇒ `bits2int` 就是纯大端整数，
 * 没有要截的位）、SHA-1/384/512 的变体（协议只用 SHA-256）。
 */
#ifndef ORPAH_RFC6979_H
#define ORPAH_RFC6979_H

#include <stddef.h>
#include <stdint.h>

#include "p256.h"
#include "sha256.h"

#define RFC6979_BYTES   P256_BYTES      /* qlen=256 ⇒ rlen=32 字节 */

#define RFC6979_E_ARG   (-1)            /* 空指针 */
#define RFC6979_E_RETRY (-2)            /* 1000 轮都拿不到合法 k（实际不可能，留个出口） */

/* 输入：x_be = 私钥（32 B 大端，必须在 [1, n-1]）、h1 = SHA-256(preimage)（32 B）。
 * 输出：k_be = 32 B 大端（保证在 [1, n-1]，RFC 6979 的循环已把不合格的丢掉）。
 * 返回 0 / RFC6979_E_ARG / RFC6979_E_RETRY。
 *
 * ⚠ k 是**私钥级秘密**（k 泄漏 = 私钥泄漏）：调用方用完必须清零，且不要打日志/发出去。
 *   本函数自己清掉中间的 K/V（栈上），但输出缓冲由调用方负责。*/
int rfc6979_k_p256_sha256(uint8_t k_be[RFC6979_BYTES],
                          const uint8_t x_be[P256_BYTES],
                          const uint8_t h1[SHA256_DIGEST_LEN]);

#endif /* ORPAH_RFC6979_H */
