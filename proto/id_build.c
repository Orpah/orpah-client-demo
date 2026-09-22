/* id_build.c — 设备侧流水线（纯逻辑；无 stdio / 无 string.h / 无 malloc）。*/
#include "id_build.h"

#include "id_keys.h"
#include "id_level.h"
#include "id_report.h"
#include "msg.h"

/* 缺省源 MAC = 上游 client.py 的 `4A:06:59:00:00:01` */
void idb_mac_default(uint8_t out[IDB_MAC_LEN])
{
    static const uint8_t mac[IDB_MAC_LEN] = { 0x4Au, 0x06u, 0x59u, 0x00u, 0x00u, 0x01u };

    if (out != 0) {
        int i;

        for (i = 0; i < IDB_MAC_LEN; i++) {
            out[i] = mac[i];
        }
    }
}

static unsigned long _slen(const char *s)
{
    unsigned long n = 0;

    if (s != 0) {
        while (s[n] != '\0') { n++; }
    }
    return n;
}

int idb_init(idb_t *b, jcs_ctx_t *jc,
             char *rep, size_t repcap, char *env, size_t envcap,
             char *frame, size_t framecap,
             const char *sn, int gen, const char *firmware,
             const uint8_t src_mac[IDB_MAC_LEN], uint32_t boot_entropy)
{
    unsigned long n;
    int i;

    if (b == 0 || jc == 0 || rep == 0 || env == 0 || frame == 0 ||
        sn == 0 || sn[0] == '\0') {
        return IDB_E_ARG;
    }
    if (repcap == 0u || envcap == 0u || framecap <= (size_t)IDB_HDR_LEN) {
        return IDB_E_ARG;
    }
    n = _slen(sn);
    if (n > (unsigned long)IDN_SN_MAX) {
        return IDB_E_ARG;
    }
    for (i = 0; i < (int)n; i++) {
        b->sn[i] = sn[i];
    }
    b->sn[(int)n] = '\0';

    b->gen = gen;
    b->firmware = firmware;
    if (src_mac != 0) {
        for (i = 0; i < IDB_MAC_LEN; i++) {
            b->mac[i] = src_mac[i];
        }
    } else {
        idb_mac_default(b->mac);
    }

    b->jc = jc;
    b->rep = rep;   b->repcap = repcap;
    b->env = env;   b->envcap = envcap;
    b->frame = frame; b->framecap = framecap;

    b->built = 0u;
    b->failed = 0u;
    b->fell_back = 0u;
    b->last_level = -1;
    b->last_reason = 0;
    b->last_idr_rc = 0;
    b->last_err = 0;
    b->last_frame_len = 0u;
    b->last_rep_len = 0u;
    b->last_env_len = 0u;
    b->last_nonce[0] = '\0';
    /* ★ 必须显式清零：不清零的话 `_ensure_keys()` 会把**栈垃圾**当成"已派生过"
     *   ⇒ 直接拿垃圾字节当密钥签名。现象极隐：**同进程的第一条对、第二条起错**
     *   （一次一进程的 CLI 恰好掩盖它 —— 2026-09-22 实测踩到；同 c4-β-2 那次
     *   `ecdsa_cli` 的 `k1` 只写了末字节／前 31 字节是栈垃圾的教训）。*/
    b->have_ec = 0;
    b->have_hmac = 0;
    {
        int i;

        for (i = 0; i < 32; i++) {
            b->ec[i] = 0u;
            b->hmac[i] = 0u;
        }
    }
    b->test_sign_fail = 0;
    b->nonce_fn = 0;
    b->nonce_ctx = 0;
    b->last_nonce_src = "soft";

    if (idn_soft_init(&b->nonce, b->sn, boot_entropy) != 0) {
        return IDB_E_ARG;
    }
    return 0;
}

/* 以太帧 = dst(广播) + src + type(0x88B5 大端) + payload（照 orpah_proto.build_eth_frame）。*/static size_t _wrap_eth(idb_t *b, const char *payload, size_t plen)
{
    static const uint8_t bcast[IDB_MAC_LEN] = { 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu };
    size_t i;

    if (plen + (size_t)IDB_HDR_LEN > b->framecap) {
        return 0u;
    }
    for (i = 0; i < (size_t)IDB_MAC_LEN; i++) {
        b->frame[i] = (char)bcast[i];
        b->frame[(size_t)IDB_MAC_LEN + i] = (char)b->mac[i];
    }
    b->frame[12] = (char)((IDB_ETHERTYPE >> 8) & 0xFF);
    b->frame[13] = (char)(IDB_ETHERTYPE & 0xFF);
    for (i = 0; i < plen; i++) {
        b->frame[(size_t)IDB_HDR_LEN + i] = payload[i];
    }
    return plen + (size_t)IDB_HDR_LEN;
}

/* 按级别备好密钥材料（懒派生 + 缓存）。成功返回 0。*/
static int _ensure_keys(idb_t *b, int level)
{
    if (level == IDL_L0) {
        if (!b->have_ec) {
            if (idk_demo_ec(b->sn, b->gen, b->ec) != 0) {
                return IDB_E_KEYS;
            }
            b->have_ec = 1;
        }
        return 0;
    }
    if (level == IDL_L1 || level == IDL_L2) {
        if (!b->have_hmac) {
            if (idk_demo_hmac(b->sn, b->gen, b->hmac) != 0) {
                return IDB_E_KEYS;
            }
            b->have_hmac = 1;
        }
        return 0;
    }
    return 0;                       /* L3：不签名，不需要钥 */
}

/* 真正调 idr_build 的那一步（level 已定）。成功 0。*/
static int _build_report(idb_t *b, int level, size_t *replen)
{
    const unsigned char *hmac = 0;
    size_t hmaclen = 0u;
    const unsigned char *ec = 0;
    size_t eclen = 0u;
    int rc;

    if (level == IDL_L0) {
        ec = b->ec;
        eclen = 32u;
    } else if (level == IDL_L1 || level == IDL_L2) {
        hmac = b->hmac;
        hmaclen = 32u;
    }

    jcs_init(b->jc);
    if (level == IDL_L0 && b->test_sign_fail) {
        /* 测试钩子：模拟"SE 签名失败"（§8.2 Step2）—— 真机是 `atcab_sign(Slot0)` 报错，
         *   这里没有 SE，就用这个开关让同一条分支可测。**只给 PC 侧用**。*/
        b->last_idr_rc = IDR_E_ES256;
        return IDR_E_ES256;
    }
    /* ts=0（无 RTC）；cap.rtc=false 如实声明；不写 battery_mv；seen_routers 空数组 */
    rc = idr_build(b->jc, b->sn, 0LL, b->last_nonce, level,
                   MSG_CAP_FALSE, 0, 0, b->firmware,
                   0,
                   hmac, hmaclen, ec, eclen,
                   b->rep, b->repcap, replen);
    b->last_idr_rc = rc;
    return rc;
}

void idb_set_nonce_fn(idb_t *b, idb_nonce_fn fn, void *ctx)
{
    if (b == 0) {
        return;
    }
    b->nonce_fn = fn;
    b->nonce_ctx = ctx;
    b->last_nonce_src = (fn != 0) ? "se" : "soft";
}

int idb_build(idb_t *b, const char *mode, size_t *framelen,
              int *level_out, const char **reason_out){
    int se_ok = 1, sign_ok = 1, hmac_ok = 1;
    const char *reason = 0;
    int level;
    size_t replen = 0u;
    size_t envlen;
    jv_t *rep_node = 0;
    jv_t *env_node;
    int rc, en;

    if (b == 0 || mode == 0) {
        return IDB_E_ARG;
    }
    if (idl_mode_inputs(mode, &se_ok, &sign_ok, &hmac_ok) != 0) {
        b->last_err = IDB_E_MODE;
        return IDB_E_MODE;
    }
    level = idl_pick(se_ok, sign_ok, hmac_ok, &reason);

    /* nonce：**每次组装都取一条新的**。
     * 来源优先用**注入的 provider**（固件 = ATECC608B 的 `Random(0x1B)`，c4-γ-2）；
     * 没注入就用软熵后端（PC 侧交叉测试走这条 ⇒ 结果可复现）。*/
    if (b->nonce_fn != 0) {
        if (b->nonce_fn(b->last_nonce, b->nonce_ctx) != 0) {
            b->failed++;
            b->last_err = IDB_E_ARG;
            return IDB_E_ARG;
        }
        b->last_nonce_src = "se";
    } else {
        if (idn_soft_next(&b->nonce, b->last_nonce) != 0) {
            b->failed++;
            b->last_err = IDB_E_ARG;
            return IDB_E_ARG;
        }
        b->last_nonce_src = "soft";
    }

    if (_ensure_keys(b, level) != 0) {
        b->failed++;
        b->last_err = IDB_E_KEYS;
        return IDB_E_KEYS;
    }

    rc = _build_report(b, level, &replen);
    if (rc == IDR_E_ES256 && level == IDL_L0) {
        /* §8.2 Step2：Slot0 签名失败 → 降到 L1（HMAC）。**只对这一种失败降级**（见头文件）。
         * nonce 不重新取：同一份内容重签一次，用同一条 nonce 更干净（也不会多消耗计数器）。*/
        b->fell_back++;
        level = IDL_L1;
        reason = "slot0_sign_failed";
        if (_ensure_keys(b, level) != 0) {
            b->failed++;
            b->last_err = IDB_E_KEYS;
            return IDB_E_KEYS;
        }
        rc = _build_report(b, level, &replen);
    }
    if (rc != 0) {
        b->failed++;
        b->last_err = IDB_E_SIGN;
        return IDB_E_SIGN;
    }

    /* 包链路信封：jcs arena 里已有内层报文（重新解析一次，照 jcs_cli.c 的做法）*/
    if (json_parse(b->jc, b->rep, replen, &rep_node) != 0) {
        b->failed++;
        b->last_err = IDB_E_ENV;
        return IDB_E_ENV;
    }
    env_node = msg_id_report(b->jc, rep_node, b->sn, 0LL);
    if (env_node == 0) {
        b->failed++;
        b->last_err = IDB_E_ENV;
        return IDB_E_ENV;
    }
    en = jcs_encode_raw(env_node, b->env, b->envcap);
    if (en < 0) {
        b->failed++;
        b->last_err = IDB_E_ENV;
        return IDB_E_ENV;
    }
    envlen = (size_t)en;
    b->last_rep_len = replen;
    b->last_env_len = envlen;

    {
        size_t flen = _wrap_eth(b, b->env, envlen);

        if (flen == 0u) {
            b->failed++;
            b->last_err = IDB_E_ARG;
            return IDB_E_ARG;
        }
        b->last_frame_len = (uint32_t)flen;
        if (framelen != 0) { *framelen = flen; }
    }

    b->built++;
    b->last_level = level;
    b->last_reason = reason;
    b->last_err = 0;
    if (level_out != 0) { *level_out = level; }
    if (reason_out != 0) { *reason_out = reason; }
    return 0;
}
