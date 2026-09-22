/* id_nonce.h — `payload.nonce` 的来源（**可换后端**；本文件给"软熵"实现）
 *
 * 规范口径（`OrpahIDProtocol.md` §5.4 表）：nonce **要求 ATECC608B RNG**。而 CH32V203
 * **没有 TRNG**（EVT 全树 + 板卡资料查不到），ATECC608B 在 c 步还没接线 ⇒ c4-γ-1 先用
 * **软熵后端**顶着，并且**如实标"非生产强度"**（不塞假随机、不假装是 SE 的 RNG）。
 *
 * ⚠ 非生产强度到底弱在哪（如实）：
 *   · 熵只有**几十 bit**，而且 `boot_entropy` 由调用方给（固件那边是 tick/时序抖动）；
 *   · 它**不保证不可预测**。真正要紧的后果不是"被猜出来"，而是**撞车**：
 *     与历史 nonce 撞了 → 服务端按重放丢弃（§5.5）→ 对我们就是**一次漏报**。
 *   · 本后端**能保证**的只有：同一 boot 内严格不重复（计数器）、不同 SN 不同、
 *     且随 `boot_entropy` 变化。
 * ⇒ d 步换 ATECC608B 的 `Random(0x1B)`（同一接口，见下面的"换后端"说明）。
 *
 * ★ 换后端的**唯一交换点**：调用方（固件 `Core/id_core.c` 的 `idc_next_nonce()`）
 *   只依赖 `idn_next()` 这个形状（填 32 个大写十六进制字符）。写 SE 后端时新增
 *   `idn_se_*`，在那一处换实现即可 —— **别在业务代码里散落 nonce 生成**。
 *
 * 公式（**本仓定义**，规范只要求"来源是 SE 的 RNG"，没有规定算法 ⇒ 没有外部权威可对拍）：
 *   nonce = upperhex( SHA-256("orpah-nonce-soft-v1|" + sn + "|" + str(entropy) + "|" + str(ctr))[:16] )
 *   十六进制字符**大写**（与上游 `os.urandom(16).hex().upper()` 的写法一致）。
 */
#ifndef ORPAH_ID_NONCE_H
#define ORPAH_ID_NONCE_H

#include <stddef.h>
#include <stdint.h>

#define IDN_NONCE_BYTES 16
#define IDN_NONCE_LEN   32          /* 十六进制字符数（不含结尾 NUL）*/
#define IDN_SN_MAX      40

#define IDN_E_ARG  (-1)             /* 参数非法（SN 空/太长、出参 NULL）*/

typedef struct {
    char     sn[IDN_SN_MAX + 1];
    uint32_t boot_entropy;
    uint32_t ctr;
} idn_soft_t;

/* 初始化（拷贝 SN）。entropy 由调用方给：**换一次 boot 就该换一个值**，否则跨 boot 会重复。*/
int idn_soft_init(idn_soft_t *st, const char *sn, uint32_t boot_entropy);

/* 取下一条（ctr++ 后再算）：写 32 个大写十六进制字符 + NUL。*/
int idn_soft_next(idn_soft_t *st, char out[IDN_NONCE_LEN + 1]);

/* 纯函数形态（复现/对拍用）：同一 (sn, entropy, ctr) 必得同一结果。*/
int idn_soft_at(const char *sn, uint32_t boot_entropy, uint32_t ctr,
                char out[IDN_NONCE_LEN + 1]);

/* 字节 → 大写十六进制（out 至少 2*n+1）。给 CLI/固件复用，别各写一份。*/
void idn_hex_upper(const uint8_t *in, size_t n, char *out);

#endif /* ORPAH_ID_NONCE_H */
