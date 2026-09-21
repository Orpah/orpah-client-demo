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

/* 组装并输出**报文 JSON 字节**（信封那层由 msg_id_report() 再包）。
 *   nonce            : 32 个十六进制字符（16 字节）——设备侧生成规则见 ROADMAP §五
 *   level            : 0..3（0 = ES256；1/2 = HS256；3 = 不签名）
 *   cap_rtc          : MSG_CAP_NONE / MSG_CAP_FALSE / MSG_CAP_TRUE
 *   battery_set      : 0 = 不写 battery_mv
 *   firmware         : NULL = 不写
 *   seen_routers     : NULL = 空数组（与 Python 的 `seen_routers or []` 一致）
 *   hmac_key/keylen  : level=1/2 时必须给（32 字节）
 *   ec_priv/ec_priv_len : level=0 时必须给（**32 字节大端标量 d**）；ES256 走 RFC 6979
 * 成功返回 0，并写 *outlen（不含结尾 NUL）。*/
int idr_build(jcs_ctx_t *c,
              const char *sn, long long ts, const char *nonce, int level,
              int cap_rtc, int battery_set, int battery_mv, const char *firmware,
              const jv_t *seen_routers,
              const unsigned char *hmac_key, size_t hmac_keylen,
              const unsigned char *ec_priv, size_t ec_priv_len,
              char *out, size_t outcap, size_t *outlen);

/* level → alg 字符串（"ES256"/"HS256"/"none"）；越界返回 NULL */
const char *idr_alg_of(int level);

#endif /* ORPAH_ID_REPORT_H */
