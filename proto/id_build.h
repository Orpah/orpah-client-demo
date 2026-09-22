/* id_build.h — 设备侧"身份 → 已签报文 → 链路信封 → 以太帧"的**一条流水线**（唯一实现）
 *
 * 为什么要有它：这条流水线（选级 → 取钥 → 取 nonce → `idr_build()` → 包信封 → 套以太头）
 * 在**固件主循环**和 **PC 侧 CLI/交叉测试**里都要跑。两处各写一遍的坏法是"参数/顺序差一点"，
 * 现象是服务端回 `signature_invalid`（而 C 侧自己看是自洽的）—— 所以只写一份，两侧都调它。
 *
 * 单一源（逐字对应）：
 *   · 选级        `orpah-over-halow/orpah_id.pick_level` + `LEVEL_MODES`（见 id_level.h）
 *   · 密钥材料    `derive_demo_privkey` / `derive_demo_hmac`（见 id_keys.h）
 *   · 报文+信封   `orpah_id.Device.report` / `orpah_proto.build_id_report`（见 id_report.h、msg.h）
 *   · 以太帧      `orpah_proto.build_eth_frame`（dst 缺省=广播 ff:ff:ff:ff:ff:ff、
 *                 ethertype=0x88B5、14 B 头 + payload，**不填充到 60 B**）
 *   · nonce       `id_nonce.h`（软熵后端；d 步换 SE 只换那一处）
 *
 * 本流水线**如实**写下这几个字段（别改，改了就不是设备的真实状态了）：
 *   `ts=0`（无 RTC）、`cap.rtc=false`（三态声明，谎报会被服务端抓）、不写 `battery_mv`（没有 ADC）、
 *   `seen_routers` 给空数组（下行还没接）。
 *
 * ⚠ RAM 归**调用方**：jcs arena 与三个输出缓冲都由调用方给（单片只有 20 KB，
 *   库自己藏静态大缓冲会把固件坑死）。本文件里的 static 只用于 SHA 上下文（见 id_keys.c 的说明）。
 */
#ifndef ORPAH_ID_BUILD_H
#define ORPAH_ID_BUILD_H

#include <stddef.h>
#include <stdint.h>

#include "jcs.h"
#include "id_nonce.h"

#define IDB_ETHERTYPE  0x88B5
#define IDB_MAC_LEN    6
#define IDB_HDR_LEN    14       /* 以太头：dst(6)+src(6)+type(2) */

#define IDB_E_ARG   (-1)        /* 参数非法 / 缓冲太小 */
#define IDB_E_MODE  (-2)        /* 模式名不认识（见 idl_mode_inputs）*/
#define IDB_E_KEYS  (-3)        /* 演示密钥派生失败 */
#define IDB_E_SIGN  (-4)        /* `idr_build()` 失败（`last_idr_rc` 存它的错误码）*/
#define IDB_E_ENV   (-5)        /* 信封（json_parse/msg_id_report/jcs_encode_raw）失败 */

/* ★ **推荐缓冲尺寸**（实测：报文 ≤616 B / 信封 ≤~700 B / 帧 ≤790 B；留了余量）。
 * 固件（`Core/id_core.h`）直接用它 —— 免得两侧各拍一个数，上机才发现“缓冲不够”。
 * C 侧自检（`id_cli selfcheck`）也按这几个尺寸跑四档级别。*/
#define IDB_REC_REPORT_CAP 768u
#define IDB_REC_ENV_CAP    864u
#define IDB_REC_FRAME_CAP  896u

/* nonce provider 的形状：成功写 32 个大写 hex + NUL 并返回 0；否则返回非 0。
 * （长度固定 `IDN_NONCE_LEN` = 16 字节 —— 与软熵后端**同长度** ⇒ 报文形状不变。）*/
typedef int (*idb_nonce_fn)(char out[IDN_NONCE_LEN + 1], void *ctx);

typedef struct {
    /* 身份（构造时拷贝）*/
    char        sn[IDN_SN_MAX + 1];
    int         gen;
    const char *firmware;            /* 可 NULL = 不写 firmware 字段 */
    uint8_t     mac[IDB_MAC_LEN];    /* 源 MAC（用 idb_mac_default 给缺省）*/

    /* nonce 后端状态（软熵；仅当 `nonce_fn == NULL` 时用）*/
    idn_soft_t  nonce;

    /* 密钥材料缓存（首次用到才派生；见 id_keys.h）*/
    uint8_t     ec[32];
    uint8_t     hmac[32];
    int         have_ec;
    int         have_hmac;

    /* ★ 测试钩子（**固件不用**，默认 0）：非 0 = **模拟 §8.2 Step2 的 atcab_sign 失败**
     *   （SE 未接线，没法真失败）→ 用来在 PC 上跑通"签名失败 → 降级 L1"那条分支。*/
    int         test_sign_fail;

    /* ★ nonce 来源（可注入）：非 NULL 时优先用它取 nonce（写 32 个大写 hex + NUL）。
     *   · NULL = 用**软熵后端**（`id_nonce.h`）—— PC 侧交叉测试就是这个（结果可复现）；
     *   · 固件 c4-γ-2 起注入 `Periph/atecc.c` 的 `atecc_nonce_hex()`（SE 的 `Random(0x1B)`）。
     *   为什么做成注入而不是在 `id_build.c` 里分支：本文件与 PC 侧**同一份源码**，
     *   而 `id_build.c` 不允许依赖硬件（它得能在 MSVC 上单独编过）。*/
    idb_nonce_fn nonce_fn;
    void        *nonce_ctx;

    /* 调用方给的 arena 与缓冲 */
    jcs_ctx_t  *jc;
    char       *rep;   size_t repcap;      /* 内层已签报文 */
    char       *env;   size_t envcap;      /* 链路信封 */
    char       *frame; size_t framecap;    /* 以太帧（14 + env）*/

    /* 计数与"最近一次"（控制台/日志用）*/
    uint32_t built;
    uint32_t failed;
    uint32_t fell_back;              /* ES256 失败后按 §8.2 Step2 降到 L1 的次数 */
    int      last_level;
    const char *last_reason;
    int      last_idr_rc;
    int      last_err;
    char     last_nonce[IDN_NONCE_LEN + 1];
    const char *last_nonce_src;      /* "se"（注入的 provider）/ "soft"（软熵）*/
    uint32_t last_frame_len;
    /* ⚠ `idr_build()` / `jcs_encode_raw()` 的输出**不带结尾 NUL**（同 jcs 家族）
     *   ⇒ 这两个长度是**读它们的唯一正确办法**（用 strlen 会读到上一行的残留）。*/
    size_t   last_rep_len;
    size_t   last_env_len;
} idb_t;

/* 上游 client 的缺省源 MAC（`client.py`: 4A:06:59:00:00:01）——演示用，真机应给模块自己的 MAC。*/
void idb_mac_default(uint8_t out[IDB_MAC_LEN]);

/* 注入 nonce 来源（NULL = 回到软熵后端）。在 `idb_init()` 之后调。*/
void idb_set_nonce_fn(idb_t *b, idb_nonce_fn fn, void *ctx);

/* 初始化。失败返回负码（IDB_E_ARG）。*/
int idb_init(idb_t *b, jcs_ctx_t *jc,
             char *rep, size_t repcap, char *env, size_t envcap,
             char *frame, size_t framecap,
             const char *sn, int gen, const char *firmware,
             const uint8_t src_mac[IDB_MAC_LEN], uint32_t boot_entropy);

/* 组装一帧：`mode` = §8.2 故障注入模式（auto/sign_fail/se_fail/no_key）。
 * 成功返回 0，写 *framelen（以太帧字节数）、*level_out、*reason_out（可 NULL）。
 * 失败返回负码并记进 `b->last_err`（`b->last_idr_rc` 存 `idr_build` 的原始返回码）。
 *
 * ★ 与 §8.2 Step2 一致：level=0 且**签名本体**失败（`IDR_E_ES256`）时，自动降到 L1（HMAC）
 *   重来一次，并 `fell_back++`；`*reason_out` 变成 "slot0_sign_failed"。
 *   ⚠ 只对这一种失败降级 —— 缓冲/参数类错误（IDR_E_JCS/IDR_E_ARG/CAP）**不降级**，
 *     否则会用"降级"把编码 bug 盖住。*/
int idb_build(idb_t *b, const char *mode, size_t *framelen,
              int *level_out, const char **reason_out);

#endif /* ORPAH_ID_BUILD_H */
