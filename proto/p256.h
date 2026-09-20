/* p256.h — NIST P-256（secp256r1）：素域 + 群运算 + 标量乘 + 公钥派生
 *
 * 纯逻辑：**无 stdio / malloc / 浮点 / string.h ⇒ 可编进固件**（`-nostdlib`）。
 * 只做**椭圆曲线本身**；ECDSA 签名（`r||s`）与 RFC 6979 在 `ecdsa.{h,c}`。
 *
 * 单一源与常数来历：
 *   · 曲线参数（p / a=-3 / b / Gx / Gy / n）= **NIST P-256 公开常数**（FIPS 186-4 / SEC 2）。
 *     ⚠ 这些是**硬编码**的公开常数 —— 正确性不靠"我记得"，靠对拍证明：
 *     `p256-selftest` 用 **Python（OpenSSL）派生的公钥**逐字节比对（见 proto/README.md）。
 *   · `n` 与上游 `orpah-over-halow/orpah_id.py` 的 `_P256_ORDER` **逐位相同**。
 *   · 演示设备私钥的派生式 = 上游 `derive_demo_privkey(sn, gen)`：
 *     `d = (SHA-256("orpah-demo-ec-p256-v1|" + sn + "|" + gen) 按大端取整数) mod (n-1) + 1`
 *     ⚠ 真机私钥在 SE 内生成、不可导出；这条**只是演示**（同 SN 同代 → 同钥）。
 *
 * ★ 有意不做的（如实标注，别当"已防住"）：
 *   · **不做常量时间 / 不做侧信道防护**（有分支、有数据相关访存）。演示台架可以，
 *     真机若私钥在 MCU 里做签名，则必须换常量时间实现（或交给 ATECC608B 签）。
 *   · 不做点压缩/解压（签名只需要 `k*G`；`ecdsa_verify` 需要时再加）。
 */
#ifndef ORPAH_P256_H
#define ORPAH_P256_H

#include <stddef.h>
#include <stdint.h>

#define P256_BYTES 32              /* 一个域元素 / 标量的字节数 */
#define P256_NL    8               /* 32 位 limb 数 */

/* 错误码（0 = 成功） */
#define P256_E_ARG   (-1)          /* 空指针 */
#define P256_E_ZERO  (-2)          /* 标量为 0 ⇒ 结果是无穷远点（调用方多半传错了） */
#define P256_E_CURVE (-3)          /* 点不在曲线上 */

/* 素域元素：**plain 值**（不是 Montgomery 域；Montgomery 只在 fe_mul 内部出现） */
typedef struct { uint32_t v[P256_NL]; } p256_fe_t;

/* ---- 素域 ---- */
void p256_fe_zero(p256_fe_t *r);
void p256_fe_one(p256_fe_t *r);
void p256_fe_from_bytes(p256_fe_t *r, const uint8_t be[P256_BYTES]);   /* 大端 32 B */
void p256_fe_to_bytes(uint8_t be[P256_BYTES], const p256_fe_t *a);
int  p256_fe_is_zero(const p256_fe_t *a);                              /* 1/0 */
int  p256_fe_cmp(const p256_fe_t *a, const p256_fe_t *b);              /* -1 / 0 / 1 */

void p256_fe_add(p256_fe_t *r, const p256_fe_t *a, const p256_fe_t *b); /* r = a+b mod p */
void p256_fe_sub(p256_fe_t *r, const p256_fe_t *a, const p256_fe_t *b); /* r = a-b mod p */
void p256_fe_mul(p256_fe_t *r, const p256_fe_t *a, const p256_fe_t *b); /* r = a*b mod p */
void p256_fe_sqr(p256_fe_t *r, const p256_fe_t *a);
void p256_fe_inv(p256_fe_t *r, const p256_fe_t *a);                     /* a^(p-2)；a=0 → 0 */

/* 允许 r 与 a/b 是同一个对象（内部先算到临时量） */

/* ---- 群 ---- */
/* 点在曲线上？1 = 在（含 x=y=0 不是点的情形） */
int p256_point_on_curve(const uint8_t x_be[P256_BYTES], const uint8_t y_be[P256_BYTES]);

/* 标量乘：out64 = X||Y（各 32 B 大端，即未压缩点去掉 04 前缀）。
 *   k_be : 32 B 大端标量（**不取模**；> n 的标量按原样算，等价于对阶取模的结果）
 *   px/py: 仿射坐标（必须在曲线上，函数会自检）
 * 返回 0；P256_E_ZERO（k=0 或结果无穷远）、P256_E_CURVE（点不在曲线上）。*/
int p256_point_mul(uint8_t out64[2 * P256_BYTES], const uint8_t k_be[P256_BYTES],
                   const uint8_t px_be[P256_BYTES], const uint8_t py_be[P256_BYTES]);

/* 公钥派生：out65 = 04||X||Y（**未压缩点**，与 OpenSSL X962/UncompressedPoint 同格式）。
 * d_be 必须非 0。返回 0 / P256_E_ZERO。*/
int p256_pubkey_from_priv(uint8_t out65[65], const uint8_t d_be[P256_BYTES]);

/* 演示设备私钥（= 上游 derive_demo_privkey；**不是真机做法**，见文件头）。
 * 成功 0 并把 d 写入 d_be（大端 32 B）。*/
int p256_demo_priv_from_sn(uint8_t d_be[P256_BYTES], const char *sn, int gen);

/* 曲线阶 n（大端 32 B）—— ECDSA 侧要用（对 n 取模）。*/
void p256_order_bytes(uint8_t n_be[P256_BYTES]);

#endif /* ORPAH_P256_H */
