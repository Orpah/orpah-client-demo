/* id_keys.h — 设备**演示密钥材料**（EC 私钥 + HMAC 降级密钥）的 C 侧唯一入口
 *
 * 单一源：`orpah-over-halow/orpah_id.py`
 *   · `derive_demo_privkey(sn, gen)` → 32 B 标量大端（实现已在 `p256.c`：`p256_demo_priv_from_sn`）
 *   · `derive_demo_hmac(sn, gen)`    → `sha256(b"orpah-demo-hmac-v1|" + sn + b"|" + str(int(gen)))`
 *
 * ★ 为什么必须"只有一处"：这两个派生式一旦与 Python 差一个字节，**服务端就回 `signature_invalid`**
 *   （而 C 侧自己验自己还是过的）。所以：
 *     · 曲线私钥不在这里重写，直接调 `p256_demo_priv_from_sn()`（**已经是**上游那条式子）；
 *     · HMAC 只在 `idk_demo_hmac()` 里实现一次，交叉测试拿上游 `derive_demo_hmac` 逐字节比。
 *
 * ⚠ **仅演示**：真机密钥在 ATECC608B 内生成、**不可导出**（§6），派生式只是让同一 SN
 *   在重启/多进程后仍是同一把钥；`gen` 是演示用的"代次"，真机一代终身（§6.3.1）。
 */
#ifndef ORPAH_ID_KEYS_H
#define ORPAH_ID_KEYS_H

#include <stdint.h>

#include "p256.h"        /* P256_BYTES */

#define IDK_E_ARG  (-1)     /* 参数非法（SN 为空 / 出参为 NULL）*/

/* 演示 EC 私钥（32 B 大端标量 d）。成功返回 0（转发 `p256_demo_priv_from_sn` 的返回值）。*/
int idk_demo_ec(const char *sn, int gen, uint8_t out32[P256_BYTES]);

/* 演示 HMAC 降级密钥（32 B）。成功返回 0。*/
int idk_demo_hmac(const char *sn, int gen, uint8_t out32[32]);

#endif /* ORPAH_ID_KEYS_H */
