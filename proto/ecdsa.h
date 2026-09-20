/* ecdsa.h — ECDSA over NIST P-256，输出 **r||s**（各 32 B 大端，共 64 B）
 *
 * 纯逻辑：**无 stdio / malloc / 浮点 / string.h ⇒ 可编进固件**（`-nostdlib`）。
 *
 * 与规范/上游的对齐：
 *   · 规范 §5.1：`ES256: sig = ECDSA-P256-sign(privkey, SHA-256(preimage))`
 *   · 上游 `orpah-over-halow/orpah_id.py` 的 `_sign_es256()` 产出的就是
 *     **raw r||s 64 字节**（不是 DER），再 `b64url` 塞进 `sig` 字段。
 *   · k 的来源由调用方给（本仓真机走 RFC 6979，见 `rfc6979.h`；规范里 k 无规定）。
 *
 * ★ 有意不做的（如实标注，别当成"已防护"）：
 *   · **不做常量时间**：mod n 归约是教科书式移位减法，且私钥相关分支/访存可辨。
 *     演示台架可以；真机若私钥在 MCU 内做签名必须换常量时间实现，或交给 ATECC608B 签。
 *   · **不做 low-s 归一化** —— RFC 6979 §A.2.5 的官方向量里 s **就不是**归一化的
 *     （"sample" 那条 s > n/2）。归一化会对不上官方夹具；验签方本来就两者都收。
 *   · 求逆用费马小定理（a^(n-2) mod n，256 步平方+乘）：**慢但好验证**。签名是低频动作
 *     （设计常态 60 s/次），够用。要快得上 Barrett/Montgomery 或二进制扩展欧几里得。
 *   · **不做验签**：自己验自己只是"自洽"，判据用外部实现更强 ——
 *     host 侧用 OpenSSL/Python `cryptography`，端到端用上游 `orpah_id.verify_report`
 *     （见 `proto/README.md` 与 `proto/run_cross_test.py`）。
 */
#ifndef ORPAH_ECDSA_H
#define ORPAH_ECDSA_H

#include <stddef.h>
#include <stdint.h>

#include "p256.h"
#include "sha256.h"

#define ECDSA_BYTES  (2 * P256_BYTES)     /* r||s = 64 B */

#define ECDSA_E_ARG   (-1)                /* 空指针 */
#define ECDSA_E_ZERO  (-2)                /* k=0 / d=0 / r=0 / s=0 ⇒ 必须换 k 重来 */
#define ECDSA_E_CURVE (-3)                /* 曲线层报错（点不在曲线上等） */

/* 用**显式 k** 签名。h1 = SHA-256(preimage)（32 B）。
 * k 的保密与唯一性由调用方负责（k 泄漏 = 私钥泄漏；k 复用 = 私钥可解）。
 * 成功 0 并把 r||s 写进 out64（r 在前）。*/
int ecdsa_sign_p256(uint8_t out64[ECDSA_BYTES],
                    const uint8_t d_be[P256_BYTES],
                    const uint8_t k_be[P256_BYTES],
                    const uint8_t h1[SHA256_DIGEST_LEN]);

/* 用 RFC 6979 确定性 k 签名（**不需要熵源**；同 (d, h1) 必得同一签名）。*/
int ecdsa_sign_p256_rfc6979(uint8_t out64[ECDSA_BYTES],
                            const uint8_t d_be[P256_BYTES],
                            const uint8_t h1[SHA256_DIGEST_LEN]);

/* 便捷：先 SHA-256(msg) 再用 RFC 6979 签名 —— 就是规范 §5.1 的 ES256 那一步。
 * （签名预像怎么拼由 `jcs.h` / `id_report.h` 管，本函数只管"给定字节的哈希"。）*/
int ecdsa_sign_p256_msg(uint8_t out64[ECDSA_BYTES],
                        const uint8_t d_be[P256_BYTES],
                        const void *msg, size_t msglen);

#endif /* ORPAH_ECDSA_H */
