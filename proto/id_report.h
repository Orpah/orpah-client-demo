/* id_report.h — 设备侧组装 orpah-id-report（含签名）
 *
 * 单一源：`orpah-over-halow/orpah_id.py` 的 `Device.report()` + `sign_preimage()`。
 *   hdr     = {"typ":"orpah-id-report","ver":1,"alg":<ES256|HS256|none>,"level":0..3}
 *   payload = {"sn","ts","nonce","seen_routers"[,"cap"][,"battery_mv"][,"firmware"]}
 *   preimage= jcs({"hdr":hdr,"payload":payload})            ← 见 jcs.h
 *   sig     = ES256: ECDSA-P256(SHA-256(preimage)) / HS256: HMAC-SHA256(key, preimage)
 *             并写进报文：report = {"hdr":…,"payload":…[,"sig":b64url(sig)]}
 *
 * ★ 级别与签名（c4-β-3 起四档齐）：
 *   level=0 → **ES256**：ECDSA-P256 / SHA-256，签名 **r||s（各 32 B 大端）= 64 B** → b64url；
 *             k 走 **RFC 6979 确定性派生**（`ecdsa.h`）——不依赖熵源，同 (d, 预像) 必得同签名。
 *   level=1/2 → HS256（HMAC-SHA256，降级链）；
 *   level=3 → 不签名（`alg=none`，规范 §8.3 的“仅覆盖”）。
 *
 * ⚠ **私钥来源（如实）**：c/d 步的演示是**调用方传入 32 B 标量 d**（`ec_priv`）——
 *   规范 §6 的真机是密钥在安全元件内生成且**不可导出**、签名也应在 SE 里做；
 *   所以 d 由参数传入（而不是从报文/全局里取），将来换 ATECC608B 只改调用方。
 *   本实现**不做常量时间**（见 `ecdsa.h` 的“有意不做的”），真机私钥若要留在 MCU 内必须先解决这条。
 *
 * ★★ **d 步（2026-10-11）：签名器可注入**（`idr_sign_fn`）——
 *   真机 = ATECC608B 的 `Sign(0x41)`（见 `Periph/atecc.c`、`proto/atecc_msg.c`）；
 *   注入后**私钥永远不进 MCU 的 RAM** ✓（`ec_priv` 可以完全是 `NULL` ✓），
 *   而且不再跑软件 P-256 签名（台架上那是**秒级**：8 MHz 下 ~9 s ⇒ 换 SE 后 build_ms 大幅下降 ✓）。
 *   不注入 = 软件替身（PC 侧交叉测试走的就是它，结果可复现 ✓）。
 */
#ifndef ORPAH_ID_REPORT_H
#define ORPAH_ID_REPORT_H

#include <stddef.h>

#include "jcs.h"
#include "msg.h"          /* MSG_CAP_NONE/FALSE/TRUE 三态 */

#define IDR_ALG_ES256  "ES256"
#define IDR_ALG_HS256  "HS256"
#define IDR_ALG_NONE   "none"

#define IDR_E_ES256    (-1)     /* level=0 的 ES256 路径出错（私钥非法 / 签名器报错）*/
#define IDR_E_ARG      (-2)     /* 参数不合法（level 越界 / 该级别缺 key）*/
#define IDR_E_JCS      (-3)     /* 预像/编码出错（arena 或缓冲不够）*/
#define IDR_E_CAP      (-4)     /* 输出缓冲不够 */

/* ★★ d 步：签名器可注入（真机 = ATECC608B 的一颗芯片）。
 * 形状（2026-10-11 起改成**按算法分流**，方案 A）——
 *   `alg`   : `IDR_ALG_ES256` / `IDR_ALG_HS256`（只会是这两个 ✓）
 *   `out`   : 输出缓冲；`outcap` = 容量；`*outlen` = 实际写出的字节数
 *             · ES256 ⇒ 写满 **`r||s` 64 B**（各 32 B 大端 ✓）
 *             · HS256 ⇒ 写 **32 B**（HMAC-SHA256 原文，b64url 之前 ✓）
 *   返回 0 成功；非 0 = 该算法这条路失败（ES256 ⇒ 调用方按 §8.2 Step2 降到 L1 ✓；
 *   HS256 ⇒ `idr_build` 返回 `IDR_E_ARG` ✗）。
 * ★ 为什么带 `alg`（而不是再加一个注入点）：一个 provider = **一块 SE** ✓，两种算法是同一颗
 *   芯片的两条命令（`Sign(0x41)` / `SHA+HAMAC` ✓）⇒ 一个点最省、语义也最清 ✓。
 * ★ 传进来的是**预像**（JCS 后的 `{"hdr":…,"payload":…}`，还没哈希）：哈希归 provider 自己 ——
 *   ES256 ⇒ 固件 SHA-256 → `Nonce(0x16)` 装摘要 → `Sign(0x41)` ✓；HS256 ⇒ 预像直接喂
 *   `SHA(HMAC_START/UPDATE/608_HMAC_END)` ✓。
 * ★★ 时序（**真机两条路都要** ✗）：SE provider 内部必须留足器件收尾时间 ——
 *   SHA/HMAC 三步之间 ≥150 ms、**`Sign` 之后 ≈1.2 s**、收到 `0x03` 要**递增退避后重发** ✓
 *   （出处与完整实测见 `tools/se_provision.py` 的同名常量 `HMAC_STEP_GAP_MS` / `SIGN_SETTLE_MS`
 *   与 `docs/atecc608b-se.md` §8.24 ✓）—— 否则 `0x03` 会连拒 ✓。
 */
typedef int (*idr_sign_fn)(const char *alg,
                           unsigned char *out, size_t outcap, size_t *outlen,
                           const void *pre, size_t prelen, void *ctx);

/* 组装并输出**报文 JSON 字节**（信封那层由 msg_id_report() 再包）。
 *   nonce            : 32 个十六进制字符（16 字节）——设备侧生成规则见 ROADMAP §五
 *   level            : 0..3（0 = ES256；1/2 = HS256；3 = 不签名）
 *   cap_rtc          : MSG_CAP_NONE / MSG_CAP_FALSE / MSG_CAP_TRUE
 *   battery_set      : 0 = 不写 battery_mv
 *   firmware         : NULL = 不写
 *   seen_routers     : NULL = 空数组（与 Python 的 `seen_routers or []` 一致）
 *   hmac_key/keylen  : level=1/2 时必须给（32 字节）
 *   ec_priv/ec_priv_len : level=0 **且没注入签名器**时必须给（**32 字节大端标量 d**）；
 *                     走了 RFC 6979
 *   sign_fn/sign_ctx : ★ d 步：ES256 的签名器（NULL = 用上面的软件替身）。
 *                     非 NULL 时 `ec_priv` **不需要**给（私钥在 SE 里 ✓）。
 * 成功返回 0，并写 *outlen（不含结尾 NUL）。*/
int idr_build(jcs_ctx_t *c,
              const char *sn, long long ts, const char *nonce, int level,
              int cap_rtc, int battery_set, int battery_mv, const char *firmware,
              const jv_t *seen_routers,
              const unsigned char *hmac_key, size_t hmac_keylen,
              const unsigned char *ec_priv, size_t ec_priv_len,
              idr_sign_fn sign_fn, void *sign_ctx,
              char *out, size_t outcap, size_t *outlen);

/* level → alg 字符串（"ES256"/"HS256"/"none"）；越界返回 NULL */
const char *idr_alg_of(int level);

#endif /* ORPAH_ID_REPORT_H */
