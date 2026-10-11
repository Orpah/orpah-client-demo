/* id_report.c — 设备侧 orpah-id-report 组装（ES256 / HS256 / none），逐步对齐 Python。 */
#include "id_report.h"

#include "b64url.h"
#include "ecdsa.h"
#include "hmac.h"

#define IDR_PREIMAGE_MAX 768
#define IDR_SIG_B64_MAX  128        /* HS256: 32 B → 43 字；ES256: 64 B → 86 字；留足余量 */

const char *idr_alg_of(int level)
{
    switch (level) {
    case 0: return IDR_ALG_ES256;
    case 1: return IDR_ALG_HS256;
    case 2: return IDR_ALG_HS256;
    case 3: return IDR_ALG_NONE;
    default: return NULL;
    }
}

int idr_build(jcs_ctx_t *c,
              const char *sn, long long ts, const char *nonce, int level,
              int cap_rtc, int battery_set, int battery_mv, const char *firmware,
              const jv_t *seen_routers,
              const unsigned char *hmac_key, size_t hmac_keylen,
              const unsigned char *ec_priv, size_t ec_priv_len,
              idr_sign_fn sign_fn, void *sign_ctx,
              char *out, size_t outcap, size_t *outlen)
{
    jv_t *hdr, *payload, *rep;
    char pre[IDR_PREIMAGE_MAX];
    int pn, n;

    if (c == NULL || sn == NULL || nonce == NULL || out == NULL || outlen == NULL) {
        return IDR_E_ARG;
    }
    if (idr_alg_of(level) == NULL) return IDR_E_ARG;
    if ((level == 1 || level == 2) && sign_fn == NULL
            && (hmac_key == NULL || hmac_keylen == 0)) {
        return IDR_E_ARG;
    }
    /* level=0：两条路**二选一** ——
     *   ① 注入的签名器（`sign_fn`；真机 = SE ⇒ **私钥不进 MCU** ✓）
     *   ② 软件替身：要 32 B 大端私钥 d（严格等长 —— 短了/长了都当调用方写错了，不默认填充）*/
    if (level == 0 && sign_fn == NULL && (ec_priv == NULL || ec_priv_len != P256_BYTES)) {
        return IDR_E_ARG;
    }

    /* ---- hdr：顺序与 Python 字面量一致（typ, ver, alg, level） ---- */
    hdr = jcs_obj(c);
    if (hdr == NULL) return IDR_E_JCS;
    if (jcs_put_str(c, hdr, "typ", "orpah-id-report") != 0) return IDR_E_JCS;
    if (jcs_put_int(c, hdr, "ver", 1) != 0) return IDR_E_JCS;
    if (jcs_put_str(c, hdr, "alg", idr_alg_of(level)) != 0) return IDR_E_JCS;
    if (jcs_put_int(c, hdr, "level", level) != 0) return IDR_E_JCS;

    /* ---- payload：sn, ts, nonce, seen_routers[, cap][, battery_mv][, firmware] ---- */
    payload = jcs_obj(c);
    if (payload == NULL) return IDR_E_JCS;
    if (jcs_put_str(c, payload, "sn", sn) != 0) return IDR_E_JCS;
    if (jcs_put_int(c, payload, "ts", ts) != 0) return IDR_E_JCS;
    if (jcs_put_str(c, payload, "nonce", nonce) != 0) return IDR_E_JCS;

    if (seen_routers != NULL) {
        if (jcs_put(c, payload, "seen_routers", (jv_t *)seen_routers) != 0) return IDR_E_JCS;
    } else {
        jv_t *empty = jcs_arr(c);
        if (empty == NULL) return IDR_E_JCS;
        if (jcs_put(c, payload, "seen_routers", empty) != 0) return IDR_E_JCS;
    }

    if (cap_rtc != MSG_CAP_NONE) {                      /* Python: if cap: */
        jv_t *cap = jcs_obj(c);
        if (cap == NULL) return IDR_E_JCS;
        if (jcs_put_bool(c, cap, "rtc", cap_rtc != 0) != 0) return IDR_E_JCS;
        if (jcs_put(c, payload, "cap", cap) != 0) return IDR_E_JCS;
    }
    if (battery_set) {                                  /* Python: if battery_mv is not None */
        if (jcs_put_int(c, payload, "battery_mv", battery_mv) != 0) return IDR_E_JCS;
    }
    if (firmware != NULL) {                             /* Python: if firmware is not None */
        if (jcs_put_str(c, payload, "firmware", firmware) != 0) return IDR_E_JCS;
    }

    /* ---- 预像（JCS 排序；与 Python jcs({"hdr","payload"}) 逐字节一致） ---- */
    pn = jcs_preimage(c, hdr, payload, pre, sizeof(pre));
    if (pn < 0) return IDR_E_JCS;

    /* ---- 报文：{"hdr":…,"payload":…[,"sig":…]} ---- */
    rep = jcs_obj(c);
    if (rep == NULL) return IDR_E_JCS;
    if (jcs_put(c, rep, "hdr", hdr) != 0) return IDR_E_JCS;
    if (jcs_put(c, rep, "payload", payload) != 0) return IDR_E_JCS;

    if (level != 3) {
        char sig_b64[IDR_SIG_B64_MAX];
        int m;

        if (level == 0) {
            /* ES256：预像 → r||s（64 B）→ b64url。
             *   · 注入了签名器（真机 = SE）：**哈希与私钥都归它管** ⇒ 这里只负责给预像 ✓
             *   · 否则：软件替身 `ecdsa_sign_p256_msg()`（内部 = SHA-256 + RFC 6979 确定性 k）
             * ⚠ 返回非 0 的情形：SE 不答/签不动，或 k/d/r/s 为 0（概率 ~2^-256）⇒ 如实报错。*/
            unsigned char raw[ECDSA_BYTES];

            if (sign_fn != 0) {
                size_t sl = 0u;
                if (sign_fn(IDR_ALG_ES256, raw, sizeof(raw), &sl, pre, (size_t)pn,
                            sign_ctx) != 0 || sl != sizeof(raw)) {
                    return IDR_E_ES256;
                }
            } else if (ecdsa_sign_p256_msg(raw, ec_priv, pre, (size_t)pn) != 0) {
                return IDR_E_ES256;
            }
            m = b64url_encode(raw, sizeof(raw), sig_b64, sizeof(sig_b64));
        } else {
            /* HS256：**优先用注入的签名器**（真机 = SE 内部的 HMAC 钥 ⇒ 钥不进 MCU 的 RAM ✓）；
             * 它没注入、或它失败时，退回软件 HMAC（用本地 `hmac_key` ✓）。
             * ★ 为什么"失败也要退回"（2026-10-11 交叉测试抓出来的 ✗）：
             *   注入器失败模拟的是"**SE 不答**" ✗ ⇒ 那是 **ES256 这条路**走不通 ✓ ⇒
             *   协议 §8.2 Step2 对它的处理**正是降到 L1** ✓ ⇒ 若降级后仍只认那个坏掉的注入器，
             *   "降级"就永远救不了场（实测：`id-cli selfcheck` rc=-104 ✗）。
             *   ★ 真机上 `hmac_key` 是 NULL（降级钥在 SE slot 4、读不出来 ✓）⇒ **不会**回退
             *     ⇒ SE 真坏了时失败如实暴露 ✓，不会被这层回退悄悄掩盖 ✓。*/
            unsigned char mac[SHA256_DIGEST_LEN];
            int got = 0;

            if (sign_fn != 0) {
                size_t sl = 0u;
                if (sign_fn(IDR_ALG_HS256, mac, sizeof(mac), &sl, pre, (size_t)pn,
                            sign_ctx) == 0 && sl == sizeof(mac)) {
                    got = 1;
                }
            }
            if (got == 0) {
                if (hmac_key == NULL || hmac_keylen == 0) { return IDR_E_ARG; }
                hmac_sha256(hmac_key, hmac_keylen, (const unsigned char *)pre, (size_t)pn, mac);
            }
            m = b64url_encode(mac, sizeof(mac), sig_b64, sizeof(sig_b64));
        }
        if (m < 0) return IDR_E_CAP;
        sig_b64[m] = '\0';
        if (jcs_put_str(c, rep, "sig", sig_b64) != 0) return IDR_E_JCS;
    }

    n = jcs_encode_raw(rep, out, outcap);               /* 报文走插入序（= encode_msg）*/
    if (n < 0) return IDR_E_CAP;
    *outlen = (size_t)n;
    return 0;
}
