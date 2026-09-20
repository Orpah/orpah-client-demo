/* msg.c — 报文构造器实现（逐条对齐 orpah_proto._base / build_* 的字段与顺序）
 *
 * 构造出来的只是 arena 里的节点树；**要发出去时**用 jcs_encode_raw() 出字节（插入序）。
 */
#include "msg.h"

/* _base(mtype, sn, ts, **kw)：
 *   {"v":PROTO_VERSION, "type":mtype, "ts":int(ts)}
 *   if sn is not None: msg["sn"] = str(sn)
 * ⚠ sn 判断是 `is not None`（空串**照样写**），与 `if mac:` 那种真值判断不同 */
static jv_t *base(jcs_ctx_t *c, const char *mtype, const char *sn, long long ts)
{
    jv_t *m = jcs_obj(c);

    if (m == NULL) return NULL;
    if (jcs_put_int(c, m, "v", PROTO_VERSION) != 0) return NULL;
    if (jcs_put_str(c, m, "type", mtype) != 0) return NULL;
    if (jcs_put_int(c, m, "ts", ts) != 0) return NULL;
    if (sn != NULL) {
        if (jcs_put_str(c, m, "sn", sn) != 0) return NULL;
    }
    return m;
}

jv_t *msg_req_connect(jcs_ctx_t *c, const char *sn, long long ts,
                      const char *mac, const char *hw)
{
    jv_t *m = base(c, MSG_REQ_CONNECT, sn, ts);

    if (m == NULL) return NULL;
    if (mac != NULL && mac[0] != '\0') {                 /* Python: if mac: */
        if (jcs_put_str(c, m, "mac", mac) != 0) return NULL;
    }
    if (hw != NULL && hw[0] != '\0') {                   /* Python: if hw: */
        if (jcs_put_str(c, m, "hw", hw) != 0) return NULL;
    }
    return m;
}

jv_t *msg_report(jcs_ctx_t *c, const char *sn, long long ts, int seq,
                 int cap_rtc, int rssi_set, int rssi)
{
    jv_t *m = base(c, MSG_REPORT, sn, ts);

    if (m == NULL) return NULL;

    /* _base(MSG_REPORT, sn, ts, seq=int(seq)) —— seq 紧跟公共头 */
    if (jcs_put_int(c, m, "seq", seq) != 0) return NULL;

    /* Python: if cap: msg["cap"] = dict(cap)  —— 在 rssi **之前** */
    if (cap_rtc != MSG_CAP_NONE) {
        jv_t *cap = jcs_obj(c);
        if (cap == NULL) return NULL;
        if (jcs_put_bool(c, cap, "rtc", cap_rtc != 0) != 0) return NULL;
        if (jcs_put(c, m, "cap", cap) != 0) return NULL;
    }

    /* Python: if rssi is not None: msg["rssi"] = int(rssi) */
    if (rssi_set) {
        if (jcs_put_int(c, m, "rssi", rssi) != 0) return NULL;
    }
    return m;
}

jv_t *msg_id_report(jcs_ctx_t *c, const jv_t *signed_report,
                    const char *sn, long long ts)
{
    jv_t *m = base(c, MSG_ID_REPORT, sn, ts);

    if (m == NULL) return NULL;
    if (signed_report == NULL) return NULL;
    if (jcs_put(c, m, "report", (jv_t *)signed_report) != 0) return NULL;
    return m;
}
