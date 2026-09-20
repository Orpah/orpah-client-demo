/* id_report.c — 设备侧 orpah-id-report 组装（HS256 / none），逐步对齐 Python。 */
#include "id_report.h"

#include "b64url.h"
#include "hmac.h"

#define IDR_PREIMAGE_MAX 768

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
              char *out, size_t outcap, size_t *outlen)
{
    jv_t *hdr, *payload, *rep;
    char pre[IDR_PREIMAGE_MAX];
    int pn, n;

    if (c == NULL || sn == NULL || nonce == NULL || out == NULL || outlen == NULL) {
        return IDR_E_ARG;
    }
    if (idr_alg_of(level) == NULL) return IDR_E_ARG;
    if (level == 0) return IDR_E_ES256;                 /* 明确报错，不假装签了 */
    if ((level == 1 || level == 2) && (hmac_key == NULL || hmac_keylen == 0)) {
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
        unsigned char mac[SHA256_DIGEST_LEN];
        char sig_b64[64];
        int m;

        hmac_sha256(hmac_key, hmac_keylen, (const unsigned char *)pre, (size_t)pn, mac);
        m = b64url_encode(mac, sizeof(mac), sig_b64, sizeof(sig_b64));
        if (m < 0) return IDR_E_CAP;
        sig_b64[m] = '\0';
        if (jcs_put_str(c, rep, "sig", sig_b64) != 0) return IDR_E_JCS;
    }

    n = jcs_encode_raw(rep, out, outcap);               /* 报文走插入序（= encode_msg）*/
    if (n < 0) return IDR_E_CAP;
    *outlen = (size_t)n;
    return 0;
}
